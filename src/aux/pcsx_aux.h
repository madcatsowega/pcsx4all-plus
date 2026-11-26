#ifndef _PCSX_AUX_H_
#define _PCSX_AUX_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize / shutdown the unified JZ4760B 330 MHz AUX+MXU coprocessor
 * running pcsx_hybrid_fw (SPU + MDEC + future GPU strip). */
void pcsx_aux_init(void);
void pcsx_aux_shutdown(void);
int  pcsx_aux_is_active(void);

/* Reap any completed slots (non-blocking) or flush all in-flight slots (blocking). */
void pcsx_aux_reap_ready(void);
void pcsx_aux_flush(void);
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

/* Telemetry for frontend/log */
void pcsx_aux_get_stats(uint32_t *spu_jobs, uint32_t *mdec_jobs, uint32_t *waits);

#ifdef __cplusplus
}
#endif

#endif /* _PCSX_AUX_H_ */
