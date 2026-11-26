#ifndef _PCSX_AUX_H_
#define _PCSX_AUX_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize / shutdown the unified JZ4760B 330 MHz AUX+MXU coprocessor
 * running pcsx_hybrid_fw v3 (SPU + MDEC + DMAGP1 Half-Screen GPU). */
void pcsx_aux_init(void);
void pcsx_aux_shutdown(void);
int  pcsx_aux_is_active(void);

/* Reap any completed slots (non-blocking) or flush all in-flight slots (blocking). */
void pcsx_aux_reap_ready(void);
void pcsx_aux_flush(void);
void pcsx_aux_flush_spu(void);
void pcsx_aux_flush_mdec(void);

/* SPU hooks (called by spu_jzaux.c) */
typedef void (*pcsx_aux_spu_reap_fn)(uint32_t slot_idx);
void pcsx_aux_register_spu(pcsx_aux_spu_reap_fn reap_fn);
void *pcsx_aux_get_sram(void);
void *pcsx_aux_get_slot(uint32_t slot_idx);
int   pcsx_aux_acquire_slot(uint32_t *out_slot_idx);
void  pcsx_aux_submit_slot(uint32_t slot_idx, uint32_t cmd_type);

/* MDEC hooks (called by mdec.cpp) */
void pcsx_aux_mdec_set_iq(const int *iq_y, const int *iq_uv);
int  pcsx_aux_mdec_submit(const uint16_t *rl_src, uint32_t rl_words,
                          uint32_t n_blocks, uint32_t flags,
                          uint8_t *dst_out, uint32_t out_bytes);

/* Contiguous cached DDR2 VRAM + Half-Screen AUX GPU hooks (gpulib + gpu_unai) */
void *pcsx_aux_vram_alloc(size_t size);
void  pcsx_aux_vram_set_ptr(void *aligned_vram);
void  pcsx_aux_vram_free(void);

struct pcsx_aux_gpu_params {
	uint32_t dl_words;
	uint16_t strip_x0;
	uint16_t strip_x1;
	uint16_t band_y0;
	uint16_t band_y1;
	uint32_t init_gp1;
	uint32_t init_texwin;
	int16_t  init_off_x;
	int16_t  init_off_y;
	uint16_t init_draw_x0;
	uint16_t init_draw_x1;
	uint16_t init_draw_y0;
	uint16_t init_draw_y1;
	uint8_t  blit_mask;
	uint8_t  ilace_mask;
	uint8_t  init_masking;
	uint8_t  init_msb;
};

int       pcsx_aux_gpu_ready(void);
int       pcsx_aux_gpu_split_pct(void);
int       pcsx_aux_gpu_verify_mode(void);
uint32_t *pcsx_aux_gpu_dl_ptr(uint32_t *out_max_words);
int       pcsx_aux_gpu_submit(const struct pcsx_aux_gpu_params *p);
void      pcsx_aux_gpu_flush(void);
void      pcsx_aux_gpu_flush_rect(int x0, int y0, int x1, int y1);

/* Telemetry for frontend/log */
void pcsx_aux_get_stats(uint32_t *spu_jobs, uint32_t *mdec_jobs, uint32_t *waits);
void pcsx_aux_get_gpu_stats(uint32_t *gpu_jobs, uint32_t *gpu_waits, uint32_t *gpu_prims, uint32_t *tile_miss);

#ifdef __cplusplus
}
#endif

#endif /* _PCSX_AUX_H_ */
