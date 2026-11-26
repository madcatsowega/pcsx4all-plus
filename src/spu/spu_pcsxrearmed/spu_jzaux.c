/*
 * JZ4760B 330 MHz Hybrid AUX+MXU Coprocessor SPU Offload Backend
 * Uses the shared 2-slot zero-wait-state FIFO ring in src/aux/pcsx_aux.c
 * (TCSM0 Slot 0 + upper TCSM1 Slot 1 + shared SB[] in SRAM Bank 0).
 */
#include "aux/pcsx_aux.h"
#include "aux/pcsx_prof.h"
#include "/home/sean/retrofw-aux/firmware/pcsx_hybrid_proto.h"

static volatile struct aux_hybrid_sram *g_spu_aux_sram = NULL;
static volatile struct aux_spu_slot *g_spu_aux_slots[2] = { NULL, NULL };
static int g_spu_aux_active = 0;
#define g_aux_active g_spu_aux_active
static int *g_spu_aux_orig_sb = NULL;
static uint32_t g_spu_aux_fallbacks = 0;

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

static void spu_jzaux_on_reap(uint32_t idx)
{
	volatile struct aux_spu_slot *slot = g_spu_aux_slots[idx & 1U];
	int ns_to = (int)slot->ns_to;
	int n2 = ns_to * 2;
	const volatile int32_t *src_lr = slot->SSumLR;
	int *dst_lr = spu.SSumLR;
	int i = 0;
	for (; i + 4 <= n2; i += 4) {
		dst_lr[i]     = src_lr[i];
		dst_lr[i + 1] = src_lr[i + 1];
		dst_lr[i + 2] = src_lr[i + 2];
		dst_lr[i + 3] = src_lr[i + 3];
	}
	for (; i < n2; i++)
		dst_lr[i] = src_lr[i];

	if (slot->do_rvb && slot->rvb_addr && spu.rvb->StartAddr &&
	    slot->rvb_addr >= (uint32_t)spu.rvb->StartAddr && slot->rvb_addr < 0x40000U) {
		uint64_t _rt0 = __builtin_expect(g_pcsx_prof.enabled, 0) ? pcsx_prof_now_us() : 0;
		const volatile int32_t *src_r = slot->RVB;
		int *dst_r = RVB;
		int j = 0;
		for (; j + 4 <= n2; j += 4) {
			dst_r[j]     = src_r[j];
			dst_r[j + 1] = src_r[j + 1];
			dst_r[j + 2] = src_r[j + 2];
			dst_r[j + 3] = src_r[j + 3];
		}
		for (; j < n2; j++)
			dst_r[j] = src_r[j];
		REVERBDo(spu.SSumLR, RVB, ns_to, (int)slot->rvb_addr);
		if (__builtin_expect(g_pcsx_prof.enabled, 0))
			g_pcsx_prof.rvb_us += pcsx_prof_now_us() - _rt0;
	}

	uint32_t dch = slot->decode_dirty_ch;
	if (dch & (1U << 1)) {
		spu_jzaux_copy_decode_buf(spu.spuMem, 0, g_spu_aux_sram->decode_v1[idx & 1U], ns_to, (int)slot->decode_pos);
		spu.decode_dirty_ch |= (1 << 1);
	}
	if (dch & (1U << 3)) {
		spu_jzaux_copy_decode_buf(spu.spuMem, 1, g_spu_aux_sram->decode_v3[idx & 1U], ns_to, (int)slot->decode_pos);
		spu.decode_dirty_ch |= (1 << 3);
	}

	if ((unsigned char *)spu.pS + ns_to * 4 > spu.pSpuBuffer + 32768)
		spu.pS = (short *)spu.pSpuBuffer;
	do_samples_finish(spu.SSumLR, ns_to, (int)slot->channels_silent, (int)slot->decode_pos);
}

static void spu_jzaux_reap_ready(void)
{
	if (!g_spu_aux_active) return;
	pcsx_aux_reap_ready();
	if (!pcsx_aux_is_active())
		g_spu_aux_active = 0;
}

static void spu_jzaux_flush(void)
{
	if (!g_spu_aux_active) return;
	pcsx_aux_flush_spu();
	if (!pcsx_aux_is_active())
		g_spu_aux_active = 0;
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
				return -1;
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
	if (!g_spu_aux_active || !pcsx_aux_is_active() || ns_to <= 0 || ns_to > AUX_SPU_MAX_NS)
		return 0;

	/* Check if any active channel uses FMod (rare); if so, handle locally */
	unsigned int on_mask = (spu.dwChannelOn | spu.dwNewChannel) & 0xffffffU;
	for (unsigned int m = on_mask, ch = 0; m != 0; ch++, m >>= 1) {
		if ((m & 1U) && spu.s_chan[ch].bFMod != 0)
			return 0;
	}

	uint32_t idx;
	if (!pcsx_aux_acquire_slot(&idx)) {
		g_spu_aux_active = 0;
		return 0;
	}

	struct {
		unsigned char *pCurr;
		unsigned char *pLoop;
		int spos;
		int iSBPos;
		int prevflags;
		int adsr_state;
		int adsr_env;
		unsigned char bRVBActive;
	} saved_ch[24];
	unsigned int saved_new = spu.dwNewChannel;
	unsigned int saved_on = spu.dwChannelOn;
	unsigned int saved_dead = spu.dwChannelDead;
	int saved_rvb_curr = spu.rvb->CurrAddr;

	volatile struct aux_spu_slot *slot = g_spu_aux_slots[idx];

	uint32_t new_mask = spu.dwNewChannel & 0xffffffU;
	uint32_t saved_mask = 0;
	for (uint32_t m = new_mask, ch = 0; m != 0; ch++, m >>= 1) {
		if (m & 1U) {
			SPUCHAN *sc = &spu.s_chan[ch];
			saved_ch[ch].pCurr      = sc->pCurr;
			saved_ch[ch].pLoop      = sc->pLoop;
			saved_ch[ch].spos       = sc->spos;
			saved_ch[ch].iSBPos     = sc->iSBPos;
			saved_ch[ch].prevflags  = sc->prevflags;
			saved_ch[ch].adsr_state = sc->ADSRX.State;
			saved_ch[ch].adsr_env   = sc->ADSRX.EnvelopeVol;
			saved_ch[ch].bRVBActive = sc->bRVBActive;
			saved_mask |= (1U << ch);
			StartSoundMain(ch);
		}
	}

	uint32_t active_mask = spu.dwChannelOn & 0xffffffU;
	uint32_t blk_total = 0;

	for (uint32_t m = active_mask, ch = 0; m != 0; ch++, m >>= 1) {
		if (!(m & 1U)) continue;
		SPUCHAN *s_chan = &spu.s_chan[ch];
		if (!(saved_mask & (1U << ch))) {
			saved_ch[ch].pCurr      = s_chan->pCurr;
			saved_ch[ch].pLoop      = s_chan->pLoop;
			saved_ch[ch].spos       = s_chan->spos;
			saved_ch[ch].iSBPos     = s_chan->iSBPos;
			saved_ch[ch].prevflags  = s_chan->prevflags;
			saved_ch[ch].adsr_state = s_chan->ADSRX.State;
			saved_ch[ch].adsr_env   = s_chan->ADSRX.EnvelopeVol;
			saved_ch[ch].bRVBActive = s_chan->bRVBActive;
			saved_mask |= (1U << ch);
		}
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
				for (uint32_t rm = saved_mask, rch = 0; rm != 0; rch++, rm >>= 1) {
					if (rm & 1U) {
						SPUCHAN *sc = &spu.s_chan[rch];
						sc->pCurr             = saved_ch[rch].pCurr;
						sc->pLoop             = saved_ch[rch].pLoop;
						sc->spos              = saved_ch[rch].spos;
						sc->iSBPos            = saved_ch[rch].iSBPos;
						sc->prevflags         = saved_ch[rch].prevflags;
						sc->ADSRX.State       = saved_ch[rch].adsr_state;
						sc->ADSRX.EnvelopeVol = saved_ch[rch].adsr_env;
						sc->bRVBActive        = saved_ch[rch].bRVBActive;
					}
				}
				spu.dwNewChannel  = saved_new;
				spu.dwChannelOn   = saved_on;
				spu.dwChannelDead = saved_dead;
				spu.rvb->CurrAddr = saved_rvb_curr;
				g_spu_aux_fallbacks++;
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

	pcsx_aux_submit_slot(idx, AUX_CMD_SPU);
	return 1;
}

static void spu_jzaux_init(void)
{
	pcsx_aux_init();
	if (!pcsx_aux_is_active())
		return;

	g_spu_aux_sram     = (volatile struct aux_hybrid_sram *)pcsx_aux_get_sram();
	g_spu_aux_slots[0] = (volatile struct aux_spu_slot *)pcsx_aux_get_slot(0);
	g_spu_aux_slots[1] = (volatile struct aux_spu_slot *)pcsx_aux_get_slot(1);
	g_spu_aux_fallbacks = 0;

	pcsx_aux_register_spu(spu_jzaux_on_reap);

	/* Share SB[] directly with AUX SRAM so savestates & fallbacks stay 100% coherent */
	g_spu_aux_orig_sb = spu.SB;
	memcpy((void *)g_spu_aux_sram->SB, spu.SB, MAXCHAN * SB_SIZE * sizeof(int));
	spu.SB = (int *)g_spu_aux_sram->SB;

	g_spu_aux_active = 1;
}

static void spu_jzaux_shutdown(void)
{
	if (!g_spu_aux_active && !pcsx_aux_is_active())
		return;
	spu_jzaux_flush();
	if (g_spu_aux_orig_sb && g_spu_aux_sram) {
		memcpy(g_spu_aux_orig_sb, (const void *)g_spu_aux_sram->SB, MAXCHAN * SB_SIZE * sizeof(int));
		spu.SB = g_spu_aux_orig_sb;
		g_spu_aux_orig_sb = NULL;
	}
	pcsx_aux_register_spu(NULL);
	g_spu_aux_active = 0;
	pcsx_aux_shutdown();
	g_spu_aux_sram = NULL;
	g_spu_aux_slots[0] = NULL;
	g_spu_aux_slots[1] = NULL;
}
