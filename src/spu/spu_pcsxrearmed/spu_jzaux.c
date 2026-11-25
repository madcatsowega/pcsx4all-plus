/*
 * JZ4760B 339 MHz AUX/VPU XBurst+MXU Coprocessor SPU Offload Backend
 * Double-buffered zero-wait-state execution in TCSM0 (Slot 0) + upper TCSM1 (Slot 1)
 * + shared SB[] history in SRAM (0x132D0000).
 */
#include <libjzaux.h>
#include "/home/sean/retrofw-aux/firmware/pcsx_spu_proto.h"
#include "/home/sean/retrofw-aux/firmware/aux_pcsx_spu_fw.h"

static jz_aux_ctx_t *g_aux_ctx = NULL;
static volatile struct aux_spu_sram *g_aux_sram = NULL;
static volatile struct aux_spu_slot *g_aux_slots[2] = { NULL, NULL };
static uint32_t g_aux_submit_seq = 0;
static uint32_t g_aux_reap_seq = 0;
static int g_aux_active = 0;
static int *g_aux_orig_sb = NULL;
static uint32_t g_aux_prof_jobs = 0;
static uint32_t g_aux_prof_fallbacks = 0;
static uint32_t g_aux_prof_waits = 0;

static void spu_jzaux_copy_decode_buf(unsigned short *mem, int which,
                                      const volatile int16_t *src,
                                      int count, int decode_pos)
{
	unsigned short *dst = &mem[0x800 / 2 + which * 0x400 / 2];
	int cursor = decode_pos;
	while (count-- > 0) {
		cursor &= 0x1ff;
		dst[cursor] = (unsigned short)(*src++);
		cursor++;
	}
}

static void spu_jzaux_reap_one(void)
{
	uint32_t seq = g_aux_reap_seq;
	uint32_t idx = seq & 1U;
	volatile struct aux_spu_slot *slot = g_aux_slots[idx];
	int timeout = 5000000;
	int waited = 0;

	while (g_aux_sram->done_seq <= seq) {
		waited = 1;
		if (--timeout <= 0) {
			/* Safety bailout: disable AUX and fall back to main CPU */
			printf("[pcsx-aux] WARNING: AUX SPU timeout (done=%u want=%u), disabling AUX!\n",
			       g_aux_sram->done_seq, seq + 1);
			g_aux_active = 0;
			g_aux_reap_seq = g_aux_submit_seq;
			return;
		}
	}
	if (waited)
		g_aux_prof_waits++;

	int ns_to = (int)slot->ns_to;
	int n2 = ns_to * 2;
	const volatile int32_t *src_lr = slot->SSumLR;
	int *dst_lr = spu.SSumLR;
	for (int i = 0; i < n2; i++)
		dst_lr[i] = src_lr[i];

	if (slot->do_rvb && slot->rvb_addr && spu.rvb->StartAddr &&
	    slot->rvb_addr >= (uint32_t)spu.rvb->StartAddr && slot->rvb_addr < 0x40000U) {
		const volatile int32_t *src_r = slot->RVB;
		int *dst_r = RVB;
		for (int i = 0; i < n2; i++)
			dst_r[i] = src_r[i];
		REVERBDo(spu.SSumLR, RVB, ns_to, (int)slot->rvb_addr);
	}

	uint32_t dch = slot->decode_dirty_ch;
	if (dch & (1U << 1)) {
		spu_jzaux_copy_decode_buf(spu.spuMem, 0, g_aux_sram->decode_v1[idx], ns_to, (int)slot->decode_pos);
		spu.decode_dirty_ch |= (1 << 1);
	}
	if (dch & (1U << 3)) {
		spu_jzaux_copy_decode_buf(spu.spuMem, 1, g_aux_sram->decode_v3[idx], ns_to, (int)slot->decode_pos);
		spu.decode_dirty_ch |= (1 << 3);
	}

	if ((unsigned char *)spu.pS + ns_to * 4 > spu.pSpuBuffer + 32768)
		spu.pS = (short *)spu.pSpuBuffer;
	do_samples_finish(spu.SSumLR, ns_to, (int)slot->channels_silent, (int)slot->decode_pos);
	g_aux_reap_seq++;
	g_aux_prof_jobs++;
}

static void spu_jzaux_reap_ready(void)
{
	if (!g_aux_active) return;
	while (g_aux_reap_seq != g_aux_submit_seq && g_aux_sram->done_seq > g_aux_reap_seq)
		spu_jzaux_reap_one();
}

static void spu_jzaux_flush(void)
{
	if (!g_aux_active) return;
	while (g_aux_reap_seq != g_aux_submit_seq && g_aux_active)
		spu_jzaux_reap_one();
}

static int skip_block_collect(int ch, volatile uint8_t (*dst_blocks)[16], uint32_t *blk_total)
{
	SPUCHAN *s_chan = &spu.s_chan[ch];
	unsigned char *start = s_chan->pCurr;
	int flags;
	int ret = 0;

	if (start == spu.spuMemC)
		ret = 1;

	if (s_chan->prevflags & 1) {
		if (!(s_chan->prevflags & 2))
			ret = 1;
		start = s_chan->pLoop;
	} else {
		check_irq(ch, start);
	}

	if (start < spu.spuMemC || start + 16 > spu.spuMemC + 0x80000)
		start = spu.spuMemC;

	if (*blk_total >= AUX_SPU_MAX_BLOCKS)
		return -1;

	/* Copy 16-byte ADPCM block into TCSM as 4 aligned 32-bit words when possible */
	volatile uint32_t *d32 = (volatile uint32_t *)dst_blocks[(*blk_total)++];
	if (((uintptr_t)start & 3U) == 0) {
		const uint32_t *s32 = (const uint32_t *)start;
		d32[0] = s32[0];
		d32[1] = s32[1];
		d32[2] = s32[2];
		d32[3] = s32[3];
	} else {
		volatile uint8_t *d8 = dst_blocks[(*blk_total) - 1];
		for (int k = 0; k < 16; k++)
			d8[k] = start[k];
	}

	flags = start[1];
	if (flags & 4)
		s_chan->pLoop = start;

	start += 16;
	if (flags & 1) {
		start = s_chan->pLoop;
		check_irq(ch, start);
	}
	if (start - spu.spuMemC >= 0x80000)
		start = spu.spuMemC;

	s_chan->pCurr = start;
	s_chan->prevflags = flags;
	return ret;
}

static int do_samples_skip_collect(int ch, int ns_to,
                                   volatile uint8_t (*dst_blocks)[16],
                                   uint32_t *blk_total)
{
	SPUCHAN *s_chan = &spu.s_chan[ch];
	int spos = s_chan->spos;
	int sinc = s_chan->sinc;
	int ret = ns_to, ns, d;

	spos += s_chan->iSBPos << 16;
	for (ns = 0; ns < ns_to; ns++) {
		spos += sinc;
		while (spos >= 28 * 0x10000) {
			d = skip_block_collect(ch, dst_blocks, blk_total);
			if (d < 0)
				return -1; /* ADPCM block buffer overflow */
			if (d && ns < ret)
				ret = ns;
			spos -= 28 * 0x10000;
		}
	}

	s_chan->iSBPos = spos >> 16;
	s_chan->spos = spos & 0xffff;
	return ret;
}

static int spu_jzaux_queue_slice(int ns_to, unsigned int silentch)
{
	if (!g_aux_active || ns_to <= 0 || ns_to > AUX_SPU_MAX_NS)
		return 0;

	/* Check if any active channel uses FMod (rare); if so, handle locally */
	unsigned int on_mask = (spu.dwChannelOn | spu.dwNewChannel) & 0xffffffU;
	for (unsigned int m = on_mask, ch = 0; m != 0; ch++, m >>= 1) {
		if ((m & 1U) && spu.s_chan[ch].bFMod != 0)
			return 0;
	}

	spu_jzaux_reap_ready();
	if ((g_aux_submit_seq - g_aux_reap_seq) >= 2U)
		spu_jzaux_reap_one();
	if (!g_aux_active)
		return 0;

	/* Snapshot mutable state in case ADPCM block buffer overflows (>544 blocks) */
	SPUCHAN saved_ch[24];
	unsigned int saved_new = spu.dwNewChannel;
	unsigned int saved_on = spu.dwChannelOn;
	unsigned int saved_dead = spu.dwChannelDead;
	int saved_rvb_curr = spu.rvb->CurrAddr;

	uint32_t idx = g_aux_submit_seq & 1U;
	volatile struct aux_spu_slot *slot = g_aux_slots[idx];

	uint32_t new_mask = spu.dwNewChannel & 0xffffffU;
	for (uint32_t m = new_mask, ch = 0; m != 0; ch++, m >>= 1) {
		if (m & 1U) {
			saved_ch[ch] = spu.s_chan[ch];
			StartSoundMain(ch);
		}
	}

	uint32_t active_mask = spu.dwChannelOn & 0xffffffU;
	uint32_t blk_total = 0;

	for (uint32_t m = active_mask, ch = 0; m != 0; ch++, m >>= 1) {
		if (!(m & 1U)) continue;
		if (!(new_mask & (1U << ch)))
			saved_ch[ch] = spu.s_chan[ch];

		SPUCHAN *s_chan = &spu.s_chan[ch];
		volatile struct aux_spu_ch *ac = &slot->ch[ch];

		ac->spos  = s_chan->spos;
		ac->sbpos = s_chan->iSBPos;
		ac->sinc  = s_chan->sinc;
		ac->vol_l = (int16_t)s_chan->iLeftVolume;
		ac->vol_r = (int16_t)s_chan->iRightVolume;

		ac->adsr_state        = (uint8_t)s_chan->ADSRX.State;
		ac->adsr_attack_exp   = (uint8_t)s_chan->ADSRX.AttackModeExp;
		ac->adsr_attack_rate  = (uint8_t)s_chan->ADSRX.AttackRate;
		ac->adsr_decay_rate   = (uint8_t)s_chan->ADSRX.DecayRate;
		ac->adsr_sustain_lvl  = (uint8_t)s_chan->ADSRX.SustainLevel;
		ac->adsr_sustain_exp  = (uint8_t)s_chan->ADSRX.SustainModeExp;
		ac->adsr_sustain_inc  = (uint8_t)s_chan->ADSRX.SustainIncrease;
		ac->adsr_sustain_rate = (uint8_t)s_chan->ADSRX.SustainRate;
		ac->adsr_release_exp  = (uint8_t)s_chan->ADSRX.ReleaseModeExp;
		ac->adsr_release_rate = (uint8_t)s_chan->ADSRX.ReleaseRate;
		ac->adsr_env_vol      = s_chan->ADSRX.EnvelopeVol;
		ac->flags = (uint8_t)((s_chan->bNoise ? 1U : 0U) | (s_chan->bRVBActive ? 2U : 0U));

		uint32_t blk_off = blk_total;
		int d;
		if (s_chan->bNoise) {
			d = do_samples_skip(ch, ns_to);
		} else {
			d = do_samples_skip_collect(ch, ns_to, slot->adpcm_blocks, &blk_total);
			if (d < 0) {
				/* Overflow (>544 ADPCM blocks in 1 slice): restore & run locally */
				for (uint32_t rm = (active_mask | new_mask), rch = 0; rm != 0; rch++, rm >>= 1)
					if (rm & 1U) spu.s_chan[rch] = saved_ch[rch];
				spu.dwNewChannel  = saved_new;
				spu.dwChannelOn   = saved_on;
				spu.dwChannelDead = saved_dead;
				spu.rvb->CurrAddr = saved_rvb_curr;
				g_aux_prof_fallbacks++;
				return 0;
			}
		}

		ac->blk_off = (uint16_t)blk_off;
		ac->blk_cnt = (uint16_t)(blk_total - blk_off);
		ac->ns_to   = d;

		d = SkipADSR(&s_chan->ADSRX, d);
		if (d < ns_to) {
			spu.dwChannelOn &= ~(1U << ch);
			s_chan->ADSRX.EnvelopeVol = 0;
		}
	}

	uint32_t do_rvb = (spu.rvb->StartAddr && spu_config.iUseReverb) ? 1U : 0U;
	slot->ns_to           = (uint32_t)ns_to;
	slot->interp_mode     = (uint32_t)spu_config.iUseInterpolation;
	slot->do_rvb          = do_rvb;
	slot->spu_ctrl        = (uint32_t)spu.spuCtrl;
	slot->channels_new    = new_mask;
	slot->channels_on     = active_mask;
	slot->channels_silent = silentch;
	slot->decode_pos      = (uint32_t)spu.decode_pos;
	slot->rvb_addr        = do_rvb ? (uint32_t)spu.rvb->CurrAddr : 0U;
	slot->total_blocks    = blk_total;
	slot->decode_dirty_ch = 0;

	if (spu.rvb->StartAddr) {
		spu.rvb->CurrAddr += ns_to / 2;
		while (spu.rvb->CurrAddr >= 0x40000)
			spu.rvb->CurrAddr -= 0x40000 - spu.rvb->StartAddr;
	}

	__sync_synchronize();
	g_aux_submit_seq++;
	g_aux_sram->submit_seq = g_aux_submit_seq;
	return 1;
}

static void spu_jzaux_init(void)
{
	if (g_aux_ctx)
		return;

	jz_aux_ctx_t *ctx = jz_aux_open();
	if (!ctx) {
		printf("-> /dev/jz_aux not available; SPU running on main CPU.\n");
		return;
	}
	if (!ctx->sram || ctx->sram_mapped_size < sizeof(struct aux_spu_sram)) {
		printf("-> /dev/jz_aux SRAM too small (%u bytes); SPU on main CPU.\n",
		       (unsigned)ctx->sram_mapped_size);
		jz_aux_close(ctx);
		return;
	}
	volatile uint8_t *tcsm1 = (volatile uint8_t *)jz_aux_map_tcsm1(ctx);
	if (!tcsm1) {
		printf("-> /dev/jz_aux TCSM1 mmap failed; SPU on main CPU.\n");
		jz_aux_close(ctx);
		return;
	}

	/* Zero shared SRAM control + SB[] before starting AUX */
	volatile uint32_t *s32 = (volatile uint32_t *)ctx->sram;
	for (size_t i = 0; i < (sizeof(struct aux_spu_sram) + 3) / 4; i++)
		s32[i] = 0;

	if (jz_aux_load(ctx, aux_pcsx_spu_fw, sizeof(aux_pcsx_spu_fw), 1, 1) < 0) {
		printf("-> /dev/jz_aux firmware load failed; SPU on main CPU.\n");
		jz_aux_close(ctx);
		return;
	}

	volatile struct aux_spu_sram *sram = (volatile struct aux_spu_sram *)ctx->sram;
	int tries = 200000;
	while (sram->magic != AUX_SPU_MAGIC && --tries > 0)
		;
	if (sram->magic != AUX_SPU_MAGIC) {
		printf("-> /dev/jz_aux firmware boot timeout (magic=0x%08x); SPU on main CPU.\n", sram->magic);
		jz_aux_close(ctx);
		return;
	}

	g_aux_ctx      = ctx;
	g_aux_sram     = sram;
	g_aux_slots[0] = (volatile struct aux_spu_slot *)ctx->tcsm0;
	g_aux_slots[1] = (volatile struct aux_spu_slot *)(tcsm1 + AUX_SPU_SLOT1_OFFSET);
	g_aux_submit_seq = 0;
	g_aux_reap_seq   = 0;
	g_aux_prof_jobs  = 0;
	g_aux_prof_fallbacks = 0;
	g_aux_prof_waits = 0;

	/* Share SB[] directly with AUX SRAM so savestates & fallbacks stay 100% coherent */
	g_aux_orig_sb = spu.SB;
	memcpy((void *)sram->SB, spu.SB, MAXCHAN * SB_SIZE * sizeof(int));
	spu.SB = (int *)sram->SB;

	g_aux_active = 1;
	printf("-> JZ4760B AUX+MXU Coprocessor SPU OFFLOAD ACTIVE! (%u MHz HCLK, fw=%uB, 2-slot pipeline)\n",
	       ctx->info.hclk_mhz, (unsigned)sizeof(aux_pcsx_spu_fw));
	{
		FILE *lf = fopen("/tmp/pcsx_aux.log", "w");
		if (lf) {
			fprintf(lf, "ACTIVE hclk=%uMHz cclk=%uMHz fw=%uB\n",
			        ctx->info.hclk_mhz, ctx->info.cclk_mhz, (unsigned)sizeof(aux_pcsx_spu_fw));
			fclose(lf);
		}
	}
}

static void spu_jzaux_shutdown(void)
{
	if (!g_aux_ctx)
		return;
	spu_jzaux_flush();
	printf("-> JZ4760B AUX SPU stats: jobs=%u fallbacks=%u waits=%u\n",
	       g_aux_prof_jobs, g_aux_prof_fallbacks, g_aux_prof_waits);
	{
		FILE *lf = fopen("/tmp/pcsx_aux.log", "a");
		if (lf) {
			fprintf(lf, "SHUTDOWN jobs=%u fallbacks=%u waits=%u\n",
			        g_aux_prof_jobs, g_aux_prof_fallbacks, g_aux_prof_waits);
			fclose(lf);
		}
	}
	if (g_aux_orig_sb) {
		memcpy(g_aux_orig_sb, (const void *)g_aux_sram->SB, MAXCHAN * SB_SIZE * sizeof(int));
		spu.SB = g_aux_orig_sb;
		g_aux_orig_sb = NULL;
	}
	g_aux_active = 0;
	jz_aux_close(g_aux_ctx);
	g_aux_ctx = NULL;
	g_aux_sram = NULL;
	g_aux_slots[0] = NULL;
	g_aux_slots[1] = NULL;
}
