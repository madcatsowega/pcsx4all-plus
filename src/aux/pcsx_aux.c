/*
 * Unified JZ4760B 330 MHz AUX+MXU Coprocessor Manager for PCSX4All+
 *
 * Manages /dev/jz_aux with:
 *   - Dedicated 16 KB TCSM0 slot for PS1 SPU & PS1 MDEC
 *   - Dedicated 12 KB upper TCSM1 DMAGP1 arena for Half-Screen AUX GPU
 *   - 12 KB SRAM Bank 0 for FIFO control, SPU state, LightLUT, and GPU job header
 *   - 2 MB contiguous cached DDR2 DMA buffer (1 MB PS1 VRAM + 960 KB GPU display list)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "pcsx_aux.h"
#include "pcsx_prof.h"

struct pcsx_prof_state g_pcsx_prof;

void pcsx_prof_frame_tick(void)
{
	static int checked = 0;
	static uint32_t bench_target = 0;
	static uint32_t bench_warmup = 30;
	static uint32_t bench_seen = 0;
	static uint64_t bench_t0 = 0;
	static uint64_t b_gpu_us = 0, b_vout_us = 0, b_spu_us = 0, b_rvb_us = 0, b_cd_us = 0, b_mdec_us = 0;
	if (!checked) {
		checked = 1;
		const char *bf = getenv("PCSX_BENCH_FRAMES");
		if (bf && atoi(bf) > 0)
			bench_target = (uint32_t)atoi(bf);
		g_pcsx_prof.enabled = (bench_target > 0 ||
		                       getenv("PCSX_PROF") != NULL ||
		                       access("/tmp/pcsx_prof.enable", F_OK) == 0);
		if (g_pcsx_prof.enabled) {
			g_pcsx_prof.frame_start_us = pcsx_prof_now_us();
			FILE *f = fopen("/tmp/pcsx_prof.log", "w");
			if (f) { fprintf(f, "# PCSX4All+ Live Frame Profiler\n"); fclose(f); }
		}
	}
	if (!g_pcsx_prof.enabled)
		return;

	if (bench_target > 0) {
		bench_seen++;
		if (bench_seen == bench_warmup) {
			bench_t0 = pcsx_prof_now_us();
			b_gpu_us = b_vout_us = b_spu_us = b_rvb_us = b_cd_us = b_mdec_us = 0;
			g_pcsx_prof.gpu_cmd_us = g_pcsx_prof.vout_us = g_pcsx_prof.spu_us = 0;
			g_pcsx_prof.rvb_us = g_pcsx_prof.cd_us = g_pcsx_prof.mdec_us = 0;
		} else if (bench_seen > bench_warmup) {
			b_gpu_us  += g_pcsx_prof.gpu_cmd_us;  g_pcsx_prof.gpu_cmd_us = 0;
			b_vout_us += g_pcsx_prof.vout_us;     g_pcsx_prof.vout_us = 0;
			b_spu_us  += g_pcsx_prof.spu_us;      g_pcsx_prof.spu_us = 0;
			b_rvb_us  += g_pcsx_prof.rvb_us;      g_pcsx_prof.rvb_us = 0;
			b_cd_us   += g_pcsx_prof.cd_us;       g_pcsx_prof.cd_us = 0;
			b_mdec_us += g_pcsx_prof.mdec_us;     g_pcsx_prof.mdec_us = 0;
			if (bench_seen >= bench_warmup + bench_target) {
				uint64_t span = pcsx_prof_now_us() - bench_t0;
				if (span == 0) span = 1;
				double f = (double)bench_target;
				double fps = (f * 1000000.0) / (double)span;
				double fms = (double)span / (f * 1000.0);
				double gms = (double)b_gpu_us / (f * 1000.0);
				double vms = (double)b_vout_us / (f * 1000.0);
				double sms = (double)b_spu_us / (f * 1000.0);
				double rms = (double)b_rvb_us / (f * 1000.0);
				double cms = (double)b_cd_us / (f * 1000.0);
				double mms = (double)b_mdec_us / (f * 1000.0);
				double cpu = fms - gms - vms - sms - cms - mms;
				if (cpu < 0.0) cpu = 0.0;
				uint32_t aspu = 0, amdec = 0, awaits = 0;
				uint32_t agpu = 0, agw = 0, agp = 0, agtm = 0;
				pcsx_aux_get_stats(&aspu, &amdec, &awaits);
				pcsx_aux_get_gpu_stats(&agpu, &agw, &agp, &agtm);
				printf("[BENCH] %ux%u frames=%u elapsed=%.1fms FPS=%.2f frame=%.2fms | gpu=%.2f vout=%.2f spu=%.2f(rvb=%.2f) cd=%.2f mdec=%.2f cpu+gte=%.2f | aux: spu=%u mdec=%u gpu=%u(w=%u p=%u tm=%u) w=%u\n",
				       g_pcsx_prof.res_w, g_pcsx_prof.res_h, bench_target,
				       (double)span / 1000.0, fps, fms, gms, vms, sms, rms, cms, mms, cpu,
				       aspu, amdec, agpu, agw, agp, agtm, awaits);
				fflush(stdout);
				exit(0);
			}
		}
		return;
	}

	g_pcsx_prof.frames++;
	if (g_pcsx_prof.frames >= 60) {
		uint64_t now = pcsx_prof_now_us();
		uint64_t span = now - g_pcsx_prof.frame_start_us;
		if (span == 0) span = 1;
		double fps = (60.0 * 1000000.0) / (double)span;
		double frame_ms = (double)span / (60.0 * 1000.0);
		double gpu_ms   = (double)g_pcsx_prof.gpu_cmd_us / (60.0 * 1000.0);
		double vout_ms  = (double)g_pcsx_prof.vout_us / (60.0 * 1000.0);
		double spu_ms   = (double)g_pcsx_prof.spu_us / (60.0 * 1000.0);
		double rvb_ms   = (double)g_pcsx_prof.rvb_us / (60.0 * 1000.0);
		double cd_ms    = (double)g_pcsx_prof.cd_us / (60.0 * 1000.0);
		double mdec_ms  = (double)g_pcsx_prof.mdec_us / (60.0 * 1000.0);
		double cpu_ms   = frame_ms - gpu_ms - vout_ms - spu_ms - cd_ms - mdec_ms;
		if (cpu_ms < 0.0) cpu_ms = 0.0;

		uint32_t aspu = 0, amdec = 0, awaits = 0;
		uint32_t agpu = 0, agw = 0, agp = 0, agtm = 0;
		pcsx_aux_get_stats(&aspu, &amdec, &awaits);
		pcsx_aux_get_gpu_stats(&agpu, &agw, &agp, &agtm);

		FILE *f = fopen("/tmp/pcsx_prof.log", "a");
		if (f) {
			fprintf(f, "FPS=%5.1f %ux%u frame=%5.2fms | gpu=%5.2f vout=%4.2f spu=%4.2f(rvb=%4.2f) cd=%4.2f mdec=%4.2f cpu+gte=%5.2f | "
			           "prims/f: F=%u FT=%u G=%u GT=%u(p=%u) Spr=%u Tile=%u Blend=%u (4b=%u 8b=%u 16b=%u fb=%u) | "
			           "aux: spu=%u mdec=%u gpu=%u(w=%u) w=%u\n",
			        fps, g_pcsx_prof.res_w, g_pcsx_prof.res_h,
			        frame_ms, gpu_ms, vout_ms, spu_ms, rvb_ms, cd_ms, mdec_ms, cpu_ms,
			        g_pcsx_prof.prim_f / 60, g_pcsx_prof.prim_ft / 60,
			        g_pcsx_prof.prim_g / 60, g_pcsx_prof.prim_gt / 60,
			        g_pcsx_prof.prim_gt_promo / 60,
			        g_pcsx_prof.prim_spr / 60, g_pcsx_prof.prim_tile / 60,
			        g_pcsx_prof.prim_blend / 60,
			        g_pcsx_prof.prim_tex4 / 60, g_pcsx_prof.prim_tex8 / 60, g_pcsx_prof.prim_tex16 / 60,
			        g_pcsx_prof.prim_fb_spans / 60,
			        aspu, amdec, agpu, agw, awaits);
			fclose(f);
		}

		g_pcsx_prof.frames = 0;
		g_pcsx_prof.frame_start_us = now;
		g_pcsx_prof.gpu_cmd_us = 0;
		g_pcsx_prof.vout_us = 0;
		g_pcsx_prof.spu_us = 0;
		g_pcsx_prof.rvb_us = 0;
		g_pcsx_prof.cd_us = 0;
		g_pcsx_prof.mdec_us = 0;
		g_pcsx_prof.prim_f = g_pcsx_prof.prim_ft = 0;
		g_pcsx_prof.prim_g = g_pcsx_prof.prim_gt = 0;
		g_pcsx_prof.prim_spr = g_pcsx_prof.prim_tile = 0;
		g_pcsx_prof.prim_blend = 0;
		g_pcsx_prof.prim_tex4 = g_pcsx_prof.prim_tex8 = g_pcsx_prof.prim_tex16 = 0;
		g_pcsx_prof.prim_fb_spans = g_pcsx_prof.prim_gt_promo = 0;
	}
}

#ifdef RS97
#include <libjzaux.h>
#include "/home/sean/retrofw-aux/firmware/pcsx_hybrid_proto.h"
#include "/home/sean/retrofw-aux/firmware/aux_pcsx_hybrid_fw.h"
#include "gauss_i.h"

#define AUX_DMA_VRAM_SIZE  (2U * 1024U * 1024U) /* 8KB front guard + 1MB VRAM + 1016KB tail guard */
#define AUX_DMA_DL_SIZE    (512U * 1024U)       /* 2 x 256 KB ping-pong display lists! */
#define AUX_DMA_DL_HALF    (256U * 1024U)       /* 65,536 u32 words per half */

struct mdec_pending_slot {
	uint8_t *dst;
	uint32_t out_bytes;
};

static jz_aux_ctx_t *g_aux_ctx = NULL;
static volatile struct aux_hybrid_sram *g_aux_sram = NULL;
static volatile union aux_hybrid_slot *g_aux_slot0 = NULL;

/* Contiguous cached DDR2 buffers: 2 MB PS1 VRAM (slot 0) + 2x256 KB GPU DL (slot 1) */
static jz_aux_dma_buf_t g_aux_vram_dma;
static jz_aux_dma_buf_t g_aux_dl_dma;
static int      g_aux_dma_ok = 0;
static uint32_t g_vram_dma_off = 4096U;
static uint32_t g_vram_phys = 0;
static uint32_t g_dl_phys = 0;
static uint32_t g_dl_idx = 0; /* Ping-pong 0/1 so Core 0 NEVER overwrites AUX's active DL! */

/* Independent SPU/MDEC ring (TCSM0: 2 x 8KB SPU ping-pong OR 16KB MDEC) */
static uint32_t g_spu_submit = 0;
static uint32_t g_spu_reap = 0;
static uint32_t g_spu_owner[2] = { AUX_CMD_NONE, AUX_CMD_NONE };
static int      g_spu_busy[2] = { 0, 0 };
static uint32_t g_spu_next = 0;
static int      g_mdec_inflight = 0;
static struct mdec_pending_slot g_mdec_pend;

/* Independent GPU doorbell (TCSM1+0x5000 DMAGP1 arena) */
static uint32_t g_gpu_submit = 0;
static uint32_t g_gpu_reap = 0;
static int      g_gpu_inflight = 0;
static uint32_t g_gpu_sync_off = 0;
static uint32_t g_gpu_sync_len = 0;
static int      g_gpu_x0 = 0, g_gpu_y0 = 0, g_gpu_x1 = 0, g_gpu_y1 = 0;
static int      g_aux_active = 0;

static int      g_gpu_enabled = 1;
static int      g_gpu_split_pct = 60;
static int      g_gpu_split_pinned = 0;
static uint32_t g_gpu_tune_jobs = 0;
static uint32_t g_gpu_tune_waits = 0;
static int      g_gpu_verify = 0;
static int      g_gpu_env_checked = 0;

static pcsx_aux_spu_reap_fn g_spu_reap_cb = NULL;

static uint32_t g_prof_spu_jobs = 0;
static uint32_t g_prof_mdec_jobs = 0;
static uint32_t g_prof_waits = 0;
static uint32_t g_prof_gpu_jobs = 0;
static uint32_t g_prof_gpu_waits = 0;
static uint32_t g_prof_gpu_prims = 0;
static uint32_t g_prof_gpu_tmiss = 0;

static void check_gpu_env(void)
{
	if (g_gpu_env_checked) return;
	g_gpu_env_checked = 1;
	const char *sp = getenv("PCSX_AUX_GPU_SPLIT");
	if (sp) {
		int v = atoi(sp);
		g_gpu_split_pinned = 1;
		if (v <= 0) {
			g_gpu_enabled = 0;
			g_gpu_split_pct = 0;
		} else {
			g_gpu_enabled = 1;
			if (v > 90) v = 90;
			g_gpu_split_pct = v;
		}
	}
	const char *vf = getenv("PCSX_AUX_GPU_VERIFY");
	if (vf && atoi(vf) > 0)
		g_gpu_verify = atoi(vf);
}

static void copy_from_tcsm_u8(uint8_t *dst, const volatile uint8_t *src, uint32_t n)
{
	if ((((uintptr_t)dst | (uintptr_t)src) & 3U) == 0) {
		uint32_t *d32 = (uint32_t *)dst;
		const volatile uint32_t *s32 = (const volatile uint32_t *)src;
		uint32_t nw = n >> 2;
		while (nw >= 4) {
			d32[0] = s32[0]; d32[1] = s32[1];
			d32[2] = s32[2]; d32[3] = s32[3];
			d32 += 4; s32 += 4; nw -= 4;
		}
		while (nw--) *d32++ = *s32++;
		dst = (uint8_t *)d32;
		src = (const volatile uint8_t *)s32;
		n &= 3U;
	}
	while (n--) *dst++ = *src++;
}

static void copy_to_tcsm_u16(volatile uint16_t *dst, const uint16_t *src, uint32_t nwords)
{
	if ((((uintptr_t)dst | (uintptr_t)src) & 3U) == 0) {
		volatile uint32_t *d32 = (volatile uint32_t *)dst;
		const uint32_t *s32 = (const uint32_t *)src;
		uint32_t n32 = nwords >> 1;
		while (n32 >= 4) {
			d32[0] = s32[0]; d32[1] = s32[1];
			d32[2] = s32[2]; d32[3] = s32[3];
			d32 += 4; s32 += 4; n32 -= 4;
		}
		while (n32--) *d32++ = *s32++;
		if (nwords & 1U) dst[nwords - 1] = src[nwords - 1];
		return;
	}
	for (uint32_t i = 0; i < nwords; i++) dst[i] = src[i];
}

static void pcsx_aux_disable_timeout(const char *kind, uint32_t done, uint32_t want)
{
	printf("[pcsx-aux] WARNING: Hybrid v4 AUX %s timeout (done=%u want=%u), disabling AUX!\n",
	       kind, done, want);
	FILE *lf = fopen("/tmp/pcsx_aux.log", "a");
	if (lf) {
		fprintf(lf, "TIMEOUT %s done=%u want=%u spu=%u mdec=%u gpu=%u\n",
		        kind, done, want, g_prof_spu_jobs, g_prof_mdec_jobs, g_prof_gpu_jobs);
		fclose(lf);
	}
	g_aux_active = 0;
	g_spu_reap = g_spu_submit;
	g_gpu_reap = g_gpu_submit;
	g_spu_busy[0] = g_spu_busy[1] = 0;
	g_mdec_inflight = 0;
	g_gpu_inflight = 0;
}

static void pcsx_aux_reap_spu_one(void)
{
	uint32_t seq = g_spu_reap;
	uint32_t fidx = seq & 1U;
	int timeout = 15000000;
	int waited = 0;

	while (g_aux_sram->spu_done <= seq) {
		waited = 1;
		if (--timeout <= 0) {
			pcsx_aux_disable_timeout("SPU/MDEC", g_aux_sram->spu_done, seq + 1);
			return;
		}
	}
	if (waited)
		g_prof_waits++;

	uint32_t owner = g_spu_owner[fidx];
	g_spu_owner[fidx] = AUX_CMD_NONE;
	g_spu_reap++;

	if (owner == AUX_CMD_SPU0 || owner == AUX_CMD_SPU1) {
		uint32_t sidx = (owner == AUX_CMD_SPU1) ? 1U : 0U;
		g_prof_spu_jobs++;
		g_spu_busy[sidx] = 0;
		if (g_spu_reap_cb)
			g_spu_reap_cb(sidx);
	} else if (owner == AUX_CMD_MDEC) {
		if (g_mdec_pend.dst && g_mdec_pend.out_bytes)
			copy_from_tcsm_u8(g_mdec_pend.dst, g_aux_slot0->mdec.out_px, g_mdec_pend.out_bytes);
		g_mdec_pend.dst = NULL;
		g_mdec_pend.out_bytes = 0;
		if (g_mdec_inflight > 0) g_mdec_inflight--;
		g_prof_mdec_jobs++;
	}
}

static void pcsx_aux_reap_gpu_one(void)
{
	if (!g_gpu_inflight) return;
	uint32_t seq = g_gpu_reap;
	int timeout = 15000000;
	int waited = 0;

	while (g_aux_sram->gpu_done <= seq) {
		waited = 1;
		if (--timeout <= 0) {
			pcsx_aux_disable_timeout("GPU", g_aux_sram->gpu_done, seq + 1);
			return;
		}
	}
	if (waited) {
		g_prof_waits++;
		g_prof_gpu_waits++;
	}
	g_gpu_reap++;
	g_prof_gpu_jobs++;
	g_prof_gpu_prims += g_aux_sram->gpu_job.aux_prims_done;
	g_prof_gpu_tmiss += g_aux_sram->gpu_job.aux_tile_miss;
	g_gpu_inflight = 0;
	if (g_aux_dma_ok && g_gpu_sync_len > 0) {
		jz_aux_dma_sync(g_aux_ctx, &g_aux_vram_dma, g_gpu_sync_off, g_gpu_sync_len, JZ_AUX_SYNC_TO_CPU);
		g_gpu_sync_len = 0;
	}
	/* Dynamic Core 0 / Core 1 load balancer (0%..84%: allows 100% async AUX GPU on CPU-bound games!) */
	if (!g_gpu_split_pinned && g_gpu_enabled) {
		g_gpu_tune_jobs++;
		if (waited) g_gpu_tune_waits++;
		if (g_gpu_tune_jobs >= 8U) {
			if (g_gpu_tune_waits >= 4U && g_gpu_split_pct < 84) {
				g_gpu_split_pct += 4;
				if (g_gpu_split_pct > 84) g_gpu_split_pct = 84;
			} else if (g_gpu_tune_waits <= 1U && g_gpu_split_pct > 0) {
				g_gpu_split_pct -= 4;
				if (g_gpu_split_pct < 0) g_gpu_split_pct = 0;
			}
			g_gpu_tune_jobs = 0;
			g_gpu_tune_waits = 0;
		}
	}
}

void pcsx_aux_reap_ready(void)
{
	if (!g_aux_active) return;
	while (g_spu_reap != g_spu_submit && g_aux_sram->spu_done > g_spu_reap)
		pcsx_aux_reap_spu_one();
	if (g_gpu_inflight && g_aux_sram->gpu_done > g_gpu_reap)
		pcsx_aux_reap_gpu_one();
}

void pcsx_aux_flush(void)
{
	if (!g_aux_active) return;
	while (g_spu_reap != g_spu_submit && g_aux_active)
		pcsx_aux_reap_spu_one();
	if (g_gpu_inflight && g_aux_active)
		pcsx_aux_reap_gpu_one();
}

void pcsx_aux_flush_spu(void)
{
	if (!g_aux_active) return;
	while (g_spu_reap != g_spu_submit && g_aux_active)
		pcsx_aux_reap_spu_one();
}

void pcsx_aux_flush_mdec(void)
{
	if (!g_aux_active || g_mdec_inflight == 0) return;
	while (g_mdec_inflight > 0 && g_spu_reap != g_spu_submit && g_aux_active)
		pcsx_aux_reap_spu_one();
}

void pcsx_aux_gpu_flush(void)
{
	if (!g_aux_active || !g_gpu_inflight) return;
	pcsx_aux_reap_gpu_one();
}

void pcsx_aux_gpu_flush_rect(int x0, int y0, int x1, int y1)
{
	if (!g_aux_active || !g_gpu_inflight) return;
	if (g_aux_sram->gpu_done > g_gpu_reap) {
		pcsx_aux_reap_gpu_one();
		return;
	}
	if (x1 <= g_gpu_x0 || x0 >= g_gpu_x1 || y1 <= g_gpu_y0 || y0 >= g_gpu_y1)
		return; /* Non-overlapping (e.g. double-buffered vout displaying Buffer A while AUX draws Buffer B!) */
	pcsx_aux_reap_gpu_one();
}

int pcsx_aux_is_active(void) { return g_aux_active; }
void pcsx_aux_register_spu(pcsx_aux_spu_reap_fn reap_fn) { g_spu_reap_cb = reap_fn; }
void *pcsx_aux_get_sram(void) { return (void *)g_aux_sram; }
void *pcsx_aux_get_slot(uint32_t slot_idx) { return (void *)&g_aux_slot0->spu[slot_idx & 1U].s; }

int pcsx_aux_acquire_slot(uint32_t *out_slot_idx)
{
	if (!g_aux_active) return 0;
	pcsx_aux_reap_ready();
	uint32_t sidx = g_spu_next;
	/* Independent SPU ring: NEVER blocks on GPU! AUX preemptively services SPU between GPU bands! */
	while ((g_mdec_inflight || g_spu_busy[sidx] || (g_spu_submit - g_spu_reap) >= 2U) && g_aux_active)
		pcsx_aux_reap_spu_one();
	if (!g_aux_active) return 0;
	g_spu_next = sidx ^ 1U;
	*out_slot_idx = sidx;
	return 1;
}

void pcsx_aux_submit_slot(uint32_t slot_idx, uint32_t cmd_type)
{
	if (cmd_type == AUX_CMD_GPU) {
		__sync_synchronize();
		g_gpu_submit++;
		g_aux_sram->total_submit = g_spu_submit + g_gpu_submit;
		g_aux_sram->gpu_submit = g_gpu_submit;
		return;
	}
	uint32_t fidx = g_spu_submit & 1U;
	uint32_t wire_cmd = cmd_type;
	if (cmd_type == AUX_CMD_SPU) {
		uint32_t sidx = slot_idx & 1U;
		g_spu_busy[sidx] = 1;
		wire_cmd = sidx ? AUX_CMD_SPU1 : AUX_CMD_SPU0;
	}
	g_spu_owner[fidx] = wire_cmd;
	g_aux_sram->spu_cmd[fidx] = wire_cmd;
	__sync_synchronize();
	g_spu_submit++;
	g_aux_sram->total_submit = g_spu_submit + g_gpu_submit;
	g_aux_sram->spu_submit = g_spu_submit;
}

void pcsx_aux_mdec_set_iq(const int *iq_y, const int *iq_uv)
{
	if (!g_aux_active || !g_aux_sram) return;
	pcsx_aux_flush_mdec();
	if (!g_aux_active) return;
	for (int i = 0; i < 64; i++) {
		g_aux_sram->iq_y[i]  = (int32_t)iq_y[i];
		g_aux_sram->iq_uv[i] = (int32_t)iq_uv[i];
	}
	__sync_synchronize();
}

int pcsx_aux_mdec_submit(const uint16_t *rl_src, uint32_t rl_words,
                         uint32_t n_blocks, uint32_t flags,
                         uint8_t *dst_out, uint32_t out_bytes)
{
	if (!g_aux_active || n_blocks == 0 || rl_words == 0 ||
	    rl_words > AUX_MDEC_MAX_RL || out_bytes > AUX_MDEC_MAX_OUT)
		return 0;

	/* MDEC uses full 16 KB TCSM0: drain any in-flight SPU/MDEC first */
	pcsx_aux_reap_ready();
	while ((g_spu_busy[0] || g_spu_busy[1] || g_mdec_inflight || (g_spu_submit - g_spu_reap) >= 2U) && g_aux_active)
		pcsx_aux_reap_spu_one();
	if (!g_aux_active)
		return 0;

	volatile struct aux_mdec_slot *ms = &g_aux_slot0->mdec;
	ms->n_blocks     = n_blocks;
	ms->flags        = flags;
	ms->rl_max_words = rl_words;
	ms->rl_consumed  = 0;
	ms->blocks_done  = 0;
	copy_to_tcsm_u16(ms->rl_in, rl_src, rl_words);

	g_mdec_pend.dst       = dst_out;
	g_mdec_pend.out_bytes = out_bytes;
	g_mdec_inflight++;

	pcsx_aux_submit_slot(0U, AUX_CMD_MDEC);
	return 1;
}

/* ============================================================================
 * Contiguous Cached DDR2 VRAM + Half-Screen AUX GPU Interface
 * ============================================================================ */

void *pcsx_aux_vram_alloc(size_t size)
{
	(void)size; /* 2 MB provides 4KB front guard + 1MB real PS1 VRAM + 1020KB tail guard */
	if (!g_aux_active || !g_aux_ctx)
		return NULL;
	if (g_aux_dma_ok)
		return g_aux_vram_dma.virt;
	if (jz_aux_dma_alloc(g_aux_ctx, AUX_DMA_VRAM_SIZE, JZ_AUX_DMA_CACHED, &g_aux_vram_dma) < 0) {
		printf("-> /dev/jz_aux DMA VRAM alloc failed (older jz_aux.ko?); half-screen AUX GPU disabled.\n");
		return NULL;
	}
	if (jz_aux_dma_alloc(g_aux_ctx, AUX_DMA_DL_SIZE, JZ_AUX_DMA_CACHED, &g_aux_dl_dma) < 0) {
		jz_aux_dma_free(g_aux_ctx, &g_aux_vram_dma);
		printf("-> /dev/jz_aux DMA DL alloc failed; half-screen AUX GPU disabled.\n");
		return NULL;
	}
	memset(g_aux_vram_dma.virt, 0, AUX_DMA_VRAM_SIZE);
	g_aux_dma_ok = 1;
	g_vram_dma_off = 4096U;
	g_vram_phys = g_aux_vram_dma.phys + g_vram_dma_off;
	g_dl_phys   = g_aux_dl_dma.phys;
	g_dl_idx    = 0;
	check_gpu_env();
	printf("-> AUX GPU DDR2 DMA VRAM ready! vram=0x%08x(2MB) dl=0x%08x(2x256KB) en=%d split=%d%% Main / %d%% AUX\n",
	       g_aux_vram_dma.phys, g_aux_dl_dma.phys, g_gpu_enabled, g_gpu_split_pct, 100 - g_gpu_split_pct);
	return g_aux_vram_dma.virt;
}

void pcsx_aux_vram_set_ptr(void *aligned_vram)
{
	if (!g_aux_dma_ok || !aligned_vram) return;
	uintptr_t off = (uintptr_t)aligned_vram - (uintptr_t)g_aux_vram_dma.virt;
	g_vram_dma_off = (uint32_t)off;
	g_vram_phys    = g_aux_vram_dma.phys + g_vram_dma_off;
}

void pcsx_aux_vram_free(void)
{
	pcsx_aux_gpu_flush();
	if (g_aux_dma_ok && g_aux_ctx) {
		jz_aux_dma_free(g_aux_ctx, &g_aux_dl_dma);
		jz_aux_dma_free(g_aux_ctx, &g_aux_vram_dma);
		g_aux_dma_ok = 0;
		g_vram_phys = 0;
		g_dl_phys = 0;
	}
}

int pcsx_aux_gpu_ready(void)
{
	check_gpu_env();
	return (g_aux_active && g_aux_dma_ok && g_gpu_enabled);
}

int pcsx_aux_gpu_split_pct(void)
{
	check_gpu_env();
	return g_gpu_split_pct;
}

int pcsx_aux_gpu_verify_mode(void)
{
	check_gpu_env();
	return g_gpu_verify;
}

uint32_t *pcsx_aux_gpu_dl_ptr(uint32_t *out_max_words)
{
	if (!g_aux_dma_ok) {
		if (out_max_words) *out_max_words = 0;
		return NULL;
	}
	if (out_max_words) *out_max_words = AUX_DMA_DL_HALF >> 2;
	return (uint32_t *)((uint8_t *)g_aux_dl_dma.virt + (g_dl_idx * AUX_DMA_DL_HALF));
}

int pcsx_aux_gpu_submit(const struct pcsx_aux_gpu_params *p)
{
	if (!g_aux_active || !g_aux_dma_ok || !p || p->dl_words == 0)
		return 0;

	/* Wait only if previous GPU job is still in flight (NEVER waits on SPU!) */
	if (g_gpu_inflight)
		pcsx_aux_reap_gpu_one();
	if (!g_aux_active) return 0;

	/* Flush Main CPU L1 D-cache -> DDR2 (size >= 16KB triggers one r4k_blast_dcache()
	 * in ~60us, flushing ALL dirty L1 lines across BOTH VRAM and DL buffer!) */
	jz_aux_dma_sync(g_aux_ctx, &g_aux_vram_dma, 0U, AUX_DMA_VRAM_SIZE, JZ_AUX_SYNC_TO_DEV);

	uint32_t dl_phys_cur = g_dl_phys + (g_dl_idx * AUX_DMA_DL_HALF);
	g_dl_idx ^= 1U; /* Flip ping-pong DL half so next batch NEVER races active AUX reads! */

	volatile struct aux_gpu_job *j = &g_aux_sram->gpu_job;
	j->vram_phys      = g_vram_phys;
	j->dl_phys        = dl_phys_cur;
	j->dl_words       = p->dl_words;
	j->strip_x0       = p->strip_x0;
	j->strip_x1       = p->strip_x1;
	j->band_y0        = p->band_y0;
	j->band_y1        = p->band_y1;
	j->init_gp1       = p->init_gp1;
	j->init_texwin    = p->init_texwin;
	j->init_off_x     = p->init_off_x;
	j->init_off_y     = p->init_off_y;
	j->init_draw_x0   = p->init_draw_x0;
	j->init_draw_x1   = p->init_draw_x1;
	j->init_draw_y0   = p->init_draw_y0;
	j->init_draw_y1   = p->init_draw_y1;
	j->blit_mask      = p->blit_mask;
	j->ilace_mask     = p->ilace_mask;
	j->init_masking   = p->init_masking;
	j->init_msb       = p->init_msb;
	j->aux_bands_done = 0;
	j->aux_prims_done = 0;
	j->aux_tile_miss  = 0;

	/* Record exact active AUX rect + DDR2 range for overlap-aware flush & cache inv */
	g_gpu_x0       = p->strip_x0;
	g_gpu_x1       = p->strip_x1;
	g_gpu_y0       = p->band_y0;
	g_gpu_y1       = p->band_y1;
	g_gpu_sync_off = g_vram_dma_off + ((uint32_t)p->band_y0 << 11);
	g_gpu_sync_len = (uint32_t)(p->band_y1 - p->band_y0) << 11;
	g_gpu_inflight = 1;

	pcsx_aux_submit_slot(1U, AUX_CMD_GPU);
	return 1;
}

void pcsx_aux_get_stats(uint32_t *spu_jobs, uint32_t *mdec_jobs, uint32_t *waits)
{
	if (spu_jobs)  *spu_jobs  = g_prof_spu_jobs;
	if (mdec_jobs) *mdec_jobs = g_prof_mdec_jobs;
	if (waits)     *waits     = g_prof_waits;
}

void pcsx_aux_get_gpu_stats(uint32_t *gpu_jobs, uint32_t *gpu_waits, uint32_t *gpu_prims, uint32_t *tile_miss)
{
	if (gpu_jobs)  *gpu_jobs  = g_prof_gpu_jobs;
	if (gpu_waits) *gpu_waits = g_prof_gpu_waits;
	if (gpu_prims) *gpu_prims = g_prof_gpu_prims;
	if (tile_miss) *tile_miss = g_prof_gpu_tmiss;
}

void pcsx_aux_init(void)
{
	if (g_aux_ctx)
		return;

	jz_aux_ctx_t *ctx = jz_aux_open();
	if (!ctx) {
		printf("-> /dev/jz_aux not available; SPU, MDEC & GPU running on main CPU.\n");
		return;
	}
	if (!ctx->sram || ctx->sram_mapped_size < sizeof(struct aux_hybrid_sram)) {
		printf("-> /dev/jz_aux SRAM too small (%u < %u bytes); running on main CPU.\n",
		       (unsigned)ctx->sram_mapped_size, (unsigned)sizeof(struct aux_hybrid_sram));
		jz_aux_close(ctx);
		return;
	}

	volatile uint32_t *s32 = (volatile uint32_t *)ctx->sram;
	for (size_t i = 0; i < (sizeof(struct aux_hybrid_sram) + 3) / 4; i++)
		s32[i] = 0;

	/* Populate PS1 SPU Gaussian table in SRAM (moved from TCSM1 .rodata -> +2 KB stack!) */
	volatile struct aux_hybrid_sram *sram = (volatile struct aux_hybrid_sram *)ctx->sram;
	for (size_t i = 0; i < 1024; i++)
		sram->gauss[i] = (int16_t)gauss[i];

	if (jz_aux_load(ctx, aux_pcsx_hybrid_fw, sizeof(aux_pcsx_hybrid_fw), 1, 1) < 0) {
		printf("-> /dev/jz_aux hybrid v4 firmware load failed; running on main CPU.\n");
		jz_aux_close(ctx);
		return;
	}

	int tries = 300000;
	while (sram->magic != AUX_HYBRID_MAGIC && --tries > 0)
		;
	if (sram->magic != AUX_HYBRID_MAGIC) {
		printf("-> /dev/jz_aux hybrid v4 firmware boot timeout (magic=0x%08x).\n", sram->magic);
		jz_aux_close(ctx);
		return;
	}

	g_aux_ctx      = ctx;
	g_aux_sram     = sram;
	g_aux_slot0    = (volatile union aux_hybrid_slot *)ctx->tcsm0;
	g_spu_submit   = 0;
	g_spu_reap     = 0;
	g_spu_owner[0] = g_spu_owner[1] = AUX_CMD_NONE;
	g_spu_busy[0]  = g_spu_busy[1] = 0;
	g_spu_next     = 0;
	g_mdec_inflight = 0;
	g_gpu_submit   = 0;
	g_gpu_reap     = 0;
	g_gpu_inflight = 0;
	g_prof_spu_jobs = 0;
	g_prof_mdec_jobs = 0;
	g_prof_waits   = 0;
	g_prof_gpu_jobs = 0;
	g_prof_gpu_waits = 0;
	g_prof_gpu_prims = 0;
	g_prof_gpu_tmiss = 0;
	g_aux_active   = 1;

	printf("-> JZ4760B HYBRID v4 AUX+MXU+DMAGP1 COPROCESSOR ACTIVE! (%u MHz HCLK, fw=%uB, preempt-SPU+MDEC+GPU)\n",
	       ctx->info.hclk_mhz, (unsigned)sizeof(aux_pcsx_hybrid_fw));
	FILE *lf = fopen("/tmp/pcsx_aux.log", "w");
	if (lf) {
		fprintf(lf, "ACTIVE HYBRID v4 hclk=%uMHz cclk=%uMHz fw=%uB\n",
		        ctx->info.hclk_mhz, ctx->info.cclk_mhz, (unsigned)sizeof(aux_pcsx_hybrid_fw));
		fclose(lf);
	}
}

void pcsx_aux_shutdown(void)
{
	if (!g_aux_ctx)
		return;
	pcsx_aux_flush();
	pcsx_aux_vram_free();
	printf("-> JZ4760B Hybrid v4 AUX stats: spu=%u mdec=%u gpu=%u(w=%u p=%u tm=%u) waits=%u\n",
	       g_prof_spu_jobs, g_prof_mdec_jobs, g_prof_gpu_jobs, g_prof_gpu_waits,
	       g_prof_gpu_prims, g_prof_gpu_tmiss, g_prof_waits);
	FILE *lf = fopen("/tmp/pcsx_aux.log", "a");
	if (lf) {
		fprintf(lf, "SHUTDOWN spu=%u mdec=%u gpu=%u(w=%u p=%u tm=%u) waits=%u\n",
		        g_prof_spu_jobs, g_prof_mdec_jobs, g_prof_gpu_jobs, g_prof_gpu_waits,
		        g_prof_gpu_prims, g_prof_gpu_tmiss, g_prof_waits);
		fclose(lf);
	}
	g_aux_active = 0;
	jz_aux_close(g_aux_ctx);
	g_aux_ctx = NULL;
	g_aux_sram = NULL;
	g_aux_slot0 = NULL;
}

#else /* !RS97 */

void pcsx_aux_init(void) {}
void pcsx_aux_shutdown(void) {}
int  pcsx_aux_is_active(void) { return 0; }
void pcsx_aux_reap_ready(void) {}
void pcsx_aux_flush(void) {}
void pcsx_aux_flush_spu(void) {}
void pcsx_aux_flush_mdec(void) {}
void pcsx_aux_register_spu(pcsx_aux_spu_reap_fn reap_fn) { (void)reap_fn; }
void *pcsx_aux_get_sram(void) { return NULL; }
void *pcsx_aux_get_slot(uint32_t slot_idx) { (void)slot_idx; return NULL; }
int   pcsx_aux_acquire_slot(uint32_t *out_slot_idx) { (void)out_slot_idx; return 0; }
void  pcsx_aux_submit_slot(uint32_t slot_idx, uint32_t cmd_type) { (void)slot_idx; (void)cmd_type; }
void pcsx_aux_mdec_set_iq(const int *iq_y, const int *iq_uv) { (void)iq_y; (void)iq_uv; }
int  pcsx_aux_mdec_submit(const uint16_t *rl_src, uint32_t rl_words,
                          uint32_t n_blocks, uint32_t flags,
                          uint8_t *dst_out, uint32_t out_bytes)
{
	(void)rl_src; (void)rl_words; (void)n_blocks; (void)flags; (void)dst_out; (void)out_bytes;
	return 0;
}
void *pcsx_aux_vram_alloc(size_t size) { (void)size; return NULL; }
void  pcsx_aux_vram_set_ptr(void *aligned_vram) { (void)aligned_vram; }
void  pcsx_aux_vram_free(void) {}
int   pcsx_aux_gpu_ready(void) { return 0; }
int   pcsx_aux_gpu_split_pct(void) { return 0; }
int   pcsx_aux_gpu_verify_mode(void) { return 0; }
uint32_t *pcsx_aux_gpu_dl_ptr(uint32_t *out_max_words) { if (out_max_words) *out_max_words = 0; return NULL; }
int   pcsx_aux_gpu_submit(const struct pcsx_aux_gpu_params *p) { (void)p; return 0; }
void  pcsx_aux_gpu_flush(void) {}
void  pcsx_aux_gpu_flush_rect(int x0, int y0, int x1, int y1) { (void)x0; (void)y0; (void)x1; (void)y1; }
void pcsx_aux_get_stats(uint32_t *spu_jobs, uint32_t *mdec_jobs, uint32_t *waits)
{
	if (spu_jobs)  *spu_jobs  = 0;
	if (mdec_jobs) *mdec_jobs = 0;
	if (waits)     *waits     = 0;
}
void pcsx_aux_get_gpu_stats(uint32_t *gpu_jobs, uint32_t *gpu_waits, uint32_t *gpu_prims, uint32_t *tile_miss)
{
	if (gpu_jobs)  *gpu_jobs  = 0;
	if (gpu_waits) *gpu_waits = 0;
	if (gpu_prims) *gpu_prims = 0;
	if (tile_miss) *tile_miss = 0;
}
#endif
