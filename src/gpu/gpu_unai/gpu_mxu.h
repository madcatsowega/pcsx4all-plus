#ifndef _GPU_MXU_H_
#define _GPU_MXU_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set by spu_pcsxrearmed / port init when kernel MXU context-switch is active */
extern int spu_use_mxu;

/* BGR555 -> RGB565 scanline blitters (MXU 1.0) */
void gpu_blit320_mxu(const uint16_t *src, uint16_t *dst);
void gpu_blit512_skip_mxu(const uint16_t *src, uint16_t *dst);
void gpu_blit640_skip_mxu(const uint16_t *src, uint16_t *dst);

/* Untextured flat semi-transparent spans (Modes 0, 1/3, 2) */
void gpu_blend_flat_m0_fast(uint16_t *dst, uint32_t count, uint32_t pix15, uint32_t maskor);
void gpu_blend_flat_m1_mxu(uint16_t *dst, uint32_t count, uint32_t pix15, uint32_t maskor);
void gpu_blend_flat_m2_mxu(uint16_t *dst, uint32_t count, uint32_t pix15, uint32_t maskor);

/* Untextured Gouraud opaque span (2-px unrolled, aligned 32-bit stores) */
void gpu_poly_gouraud_fast(uint16_t *dst, uint32_t count, uint32_t gCol, uint32_t gInc, uint32_t maskor);

/* Colored Gouraud-lit 4bpp & 8bpp textured spans using MXU Q8MUL (0 LightLUT loads)
 * params[6] = { l_u, l_v, (uint32_t)l_u_inc, (uint32_t)l_v_inc, l_gCol, l_gInc }
 * uclut[i]  = 0 if transparent; else (stp_or_mask << 24) | (B5<<16) | (G5<<8) | R5
 *             where stp_or_mask = 0x80 if (maskset || (raw & 0x8000)) else 0x01
 */
void gpu_poly_gt4_rgb_mxu(uint16_t *dst, uint32_t count, const uint8_t *tba8,
                          const uint32_t *uclut, const uint32_t *params);
void gpu_poly_gt8_rgb_mxu(uint16_t *dst, uint32_t count, const uint8_t *tba8,
                          const uint32_t *uclut, const uint32_t *params);

#ifdef __cplusplus
}
#endif

#endif /* _GPU_MXU_H_ */
