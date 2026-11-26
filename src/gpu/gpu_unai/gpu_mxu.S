/*
 * Ingenic JZ4760B Base MXU 1.0 SIMD kernels for PCSX4All+ (gpu_unai & vout_port).
 *
 * All MXU opcodes are generated and verified against Ingenic's official
 * mipsel-linux-as assembler so GCC 7.3+ / 11.2+ binutils assembles cleanly.
 * Base MXU 1.0 ISA only (no MXU-enhance S32AND/S32OR/S32XOR).
 *
 * Kernels:
 *   void gpu_blit320_mxu(const uint16_t *src, uint16_t *dst)
 *   void gpu_blend_flat_m0_fast(uint16_t *dst, uint32_t count, uint32_t pix15, uint32_t maskor)
 *   void gpu_blend_flat_m1_mxu(uint16_t *dst, uint32_t count, uint32_t pix15, uint32_t maskor)
 *   void gpu_blend_flat_m2_mxu(uint16_t *dst, uint32_t count, uint32_t pix15, uint32_t maskor)
 *   void gpu_poly_gouraud_fast(uint16_t *dst, uint32_t count, uint32_t gCol, uint32_t gInc, uint32_t maskor)
 *   void gpu_poly_gt4_rgb_mxu(uint16_t *dst, uint32_t count, const uint8_t *tba8,
 *                             const uint32_t *uclut, const uint32_t *params)
 *   void gpu_poly_gt8_rgb_mxu(uint16_t *dst, uint32_t count, const uint8_t *tba8,
 *                             const uint32_t *uclut, const uint32_t *params)
 */
	.text
	.set	noreorder

/* ==========================================================================
 * void gpu_blit320_mxu(const uint16_t *src, uint16_t *dst)
 * $4 = src (320 BGR555 pixels, 2-byte aligned; fast path if 4-byte aligned)
 * $5 = dst (320 RGB565 pixels, 4-byte aligned)
 * Converts BGR555 (-bbbbbgggggrrrrr) -> RGB565 (rrrrrggggg0bbbbb), 4 px/iter.
 * ========================================================================== */
	.globl	gpu_blit320_mxu
	.type	gpu_blit320_mxu, @function
gpu_blit320_mxu:
	andi	$8, $4, 3
	bnez	$8, .Lblit320_unaligned
	li	$6, 40			/* 40 iters * 8 px = 320 px */
	addiu	$4, $4, -4		/* pre-dec for S32LDI +4 */
	addiu	$5, $5, -4		/* pre-dec for S32SDI +4 */
.Lblit320_loop8:
	pref	0, 68($4)
	/* First 4 pixels: xr1, xr2 */
	.word	0x70800454		/* S32LDI xr1, $4, 4 */
	.word	0x70800494		/* S32LDI xr2, $4, 4 */
	.word	0x72d084f4		/* Q16SLL xr3, xr1, xr2, xr4, 11 - R -> bits 15..11 */
	.word	0x71988574		/* Q16SLL xr5, xr1, xr2, xr6, 6 - G -> bits 15..11 */
	.word	0x706085f4		/* Q16SLL xr7, xr1, xr2, xr8, 1 - B -> bits 15..11 (clears bit 15 STP) */
	.word	0x72d99575		/* Q16SLR xr5, xr5, xr6, xr6, 11 - G -> bits 4..0 */
	.word	0x72e21df5		/* Q16SLR xr7, xr7, xr8, xr8, 11 - B -> bits 4..0 */
	.word	0x71999574		/* Q16SLL xr5, xr5, xr6, xr6, 6 - G -> bits 10..6 */
	.word	0x70014cce		/* Q16ADD xr3, xr3, xr5, xr0, AA, WW */
	.word	0x7001910e		/* Q16ADD xr4, xr4, xr6, xr0, AA, WW */
	.word	0x7001ccce		/* Q16ADD xr3, xr3, xr7, xr0, AA, WW */
	.word	0x7002110e		/* Q16ADD xr4, xr4, xr8, xr0, AA, WW */
	.word	0x70a004d5		/* S32SDI xr3, $5, 4 */
	.word	0x70a00515		/* S32SDI xr4, $5, 4 */
	/* Second 4 pixels: xr1, xr2 */
	.word	0x70800454		/* S32LDI xr1, $4, 4 */
	.word	0x70800494		/* S32LDI xr2, $4, 4 */
	.word	0x72d084f4		/* Q16SLL xr3, xr1, xr2, xr4, 11 */
	.word	0x71988574		/* Q16SLL xr5, xr1, xr2, xr6, 6 */
	.word	0x706085f4		/* Q16SLL xr7, xr1, xr2, xr8, 1 */
	.word	0x72d99575		/* Q16SLR xr5, xr5, xr6, xr6, 11 */
	.word	0x72e21df5		/* Q16SLR xr7, xr7, xr8, xr8, 11 */
	.word	0x71999574		/* Q16SLL xr5, xr5, xr6, xr6, 6 */
	addiu	$6, $6, -1
	.word	0x70014cce		/* Q16ADD xr3, xr3, xr5, xr0, AA, WW */
	.word	0x7001910e		/* Q16ADD xr4, xr4, xr6, xr0, AA, WW */
	.word	0x7001ccce		/* Q16ADD xr3, xr3, xr7, xr0, AA, WW */
	.word	0x7002110e		/* Q16ADD xr4, xr4, xr8, xr0, AA, WW */
	.word	0x70a004d5		/* S32SDI xr3, $5, 4 */
	bgtz	$6, .Lblit320_loop8
	.word	0x70a00515		/* S32SDI xr4, $5, 4 */
	jr	$31
	nop

.Lblit320_unaligned:
	/* src is 2-byte aligned (odd display_x), dst is 4-byte aligned */
	li	$6, 80			/* 80 iters * 4 px = 320 px */
	addiu	$5, $5, -4
.Lblit320_ul:
	lwl	$8, 3($4)
	lwr	$8, 0($4)
	lwl	$9, 7($4)
	lwr	$9, 4($4)
	.word	0x7008006f		/* S32I2M xr1, $8 */
	.word	0x700900af		/* S32I2M xr2, $9 */
	.word	0x72d084f4		/* Q16SLL xr3, xr1, xr2, xr4, 11 */
	.word	0x71988574		/* Q16SLL xr5, xr1, xr2, xr6, 6 */
	.word	0x706085f4		/* Q16SLL xr7, xr1, xr2, xr8, 1 */
	.word	0x72d99575		/* Q16SLR xr5, xr5, xr6, xr6, 11 */
	.word	0x72e21df5		/* Q16SLR xr7, xr7, xr8, xr8, 11 */
	.word	0x71999574		/* Q16SLL xr5, xr5, xr6, xr6, 6 */
	addiu	$6, $6, -1
	.word	0x70014cce		/* Q16ADD xr3, xr3, xr5, xr0, AA, WW */
	.word	0x7001910e		/* Q16ADD xr4, xr4, xr6, xr0, AA, WW */
	.word	0x7001ccce		/* Q16ADD xr3, xr3, xr7, xr0, AA, WW */
	.word	0x7002110e		/* Q16ADD xr4, xr4, xr8, xr0, AA, WW */
	addiu	$4, $4, 8
	.word	0x70a004d5		/* S32SDI xr3, $5, 4 */
	bgtz	$6, .Lblit320_ul
	.word	0x70a00515		/* S32SDI xr4, $5, 4 */
	jr	$31
	nop

/* ==========================================================================
 * void gpu_blend_flat_m0_fast(uint16_t *dst, uint32_t count, uint32_t pix15, uint32_t maskor)
 * Mode 0: ((uDst & 0x7BDE) + (uSrc & 0x7BDE)) >> 1 | maskor
 * 32-bit SWAR: 2 pixels in 3 ALU ops (zero cross-pixel carry!).
 * $4 = dst, $5 = count, $6 = pix15 (0..0x7FFF), $7 = maskor (0 or 0x8000)
 * ========================================================================== */
	.globl	gpu_blend_flat_m0_fast
	.type	gpu_blend_flat_m0_fast, @function
gpu_blend_flat_m0_fast:
	blez	$5, .Lm0_done
	andi	$6, $6, 0x7bde
	/* Align dst to 4 bytes if odd */
	andi	$8, $4, 2
	beqz	$8, .Lm0_aligned
	nop
	lhu	$9, 0($4)
	andi	$9, $9, 0x7bde
	addu	$9, $9, $6
	srl	$9, $9, 1
	or	$9, $9, $7
	sh	$9, 0($4)
	addiu	$4, $4, 2
	addiu	$5, $5, -1
	blez	$5, .Lm0_done
	nop
.Lm0_aligned:
	sll	$10, $6, 16
	or	$10, $10, $6		/* src2_masked */
	sll	$11, $7, 16
	or	$11, $11, $7		/* maskor2 */
	lui	$12, 0x7bde
	ori	$12, $12, 0x7bde	/* 0x7BDE7BDE */
	sltiu	$8, $5, 4
	bnez	$8, .Lm0_pair
	nop
.Lm0_loop4:
	lw	$8, 0($4)
	lw	$9, 4($4)
	and	$8, $8, $12
	and	$9, $9, $12
	addu	$8, $8, $10
	addu	$9, $9, $10
	srl	$8, $8, 1
	srl	$9, $9, 1
	or	$8, $8, $11
	or	$9, $9, $11
	addiu	$5, $5, -4
	sw	$8, 0($4)
	sw	$9, 4($4)
	sltiu	$8, $5, 4
	beqz	$8, .Lm0_loop4
	addiu	$4, $4, 8
.Lm0_pair:
	sltiu	$8, $5, 2
	bnez	$8, .Lm0_tail
	nop
	lw	$8, 0($4)
	and	$8, $8, $12
	addu	$8, $8, $10
	srl	$8, $8, 1
	or	$8, $8, $11
	sw	$8, 0($4)
	addiu	$4, $4, 4
	addiu	$5, $5, -2
.Lm0_tail:
	blez	$5, .Lm0_done
	nop
	lhu	$9, 0($4)
	andi	$9, $9, 0x7bde
	addu	$9, $9, $6
	srl	$9, $9, 1
	or	$9, $9, $7
	sh	$9, 0($4)
.Lm0_done:
	jr	$31
	nop

/* ==========================================================================
 * void gpu_blend_flat_m1_mxu(uint16_t *dst, uint32_t count, uint32_t pix15, uint32_t maskor)
 * Mode 1: min(31, dst_c + src_c) | maskor  (also Mode 3 with pix15=(src>>2)&0x1CE7)
 * 4 pixels/iter in MXU 1.0 (zero Blargg carry chains!).
 * $4 = dst, $5 = count, $6 = pix15 (MSB=0), $7 = maskor (0 or 0x8000)
 * ========================================================================== */
	.globl	gpu_blend_flat_m1_mxu
	.type	gpu_blend_flat_m1_mxu, @function
gpu_blend_flat_m1_mxu:
	blez	$5, .Lm1_done
	andi	$6, $6, 0x7fff
	/* Extract sB, sG, sR and duplicate into [c|c] */
	srl	$8, $6, 10		/* sB */
	sll	$9, $8, 16
	or	$8, $8, $9
	.word	0x700802af		/* S32I2M xr10, $8 */
	srl	$8, $6, 5
	andi	$8, $8, 0x1f		/* sG */
	sll	$9, $8, 16
	or	$8, $8, $9
	.word	0x700802ef		/* S32I2M xr11, $8 */
	andi	$8, $6, 0x1f		/* sR */
	sll	$9, $8, 16
	or	$8, $8, $9
	.word	0x7008032f		/* S32I2M xr12, $8 */
	lui	$8, 0x001f
	ori	$8, $8, 0x001f		/* 0x001F001F */
	.word	0x7008036f		/* S32I2M xr13, $8 */
	sll	$8, $7, 16
	or	$8, $8, $7		/* [maskor | maskor] */
	.word	0x700803af		/* S32I2M xr14, $8 */

	/* Handle odd leading pixel to 4-byte align $4 */
	andi	$8, $4, 2
	beqz	$8, .Lm1_check4
	nop
	lhu	$8, 0($4)
	.word	0x7008006f		/* S32I2M xr1, $8 */
	.word	0x704004f4		/* Q16SLL xr3, xr1, xr0, xr0, 1 */
	.word	0x72c00cf5		/* Q16SLR xr3, xr3, xr0, xr0, 11 */
	.word	0x71800574		/* Q16SLL xr5, xr1, xr0, xr0, 6 */
	.word	0x72c01575		/* Q16SLR xr5, xr5, xr0, xr0, 11 */
	.word	0x72c005f4		/* Q16SLL xr7, xr1, xr0, xr0, 11 */
	.word	0x72c01df5		/* Q16SLR xr7, xr7, xr0, xr0, 11 */
	.word	0x70028cce		/* Q16ADD xr3, xr3, xr10, xr0, AA, WW */
	.word	0x7002d54e		/* Q16ADD xr5, xr5, xr11, xr0, AA, WW */
	.word	0x70031dce		/* Q16ADD xr7, xr7, xr12, xr0, AA, WW */
	.word	0x700f4cc3		/* D16MIN xr3, xr3, xr13 */
	.word	0x700f5543		/* D16MIN xr5, xr5, xr13 */
	.word	0x700f5dc3		/* D16MIN xr7, xr7, xr13 */
	.word	0x72800cf4		/* Q16SLL xr3, xr3, xr0, xr0, 10 */
	.word	0x71401574		/* Q16SLL xr5, xr5, xr0, xr0, 5 */
	.word	0x70014cce		/* Q16ADD xr3, xr3, xr5, xr0, AA, WW */
	.word	0x7001ccce		/* Q16ADD xr3, xr3, xr7, xr0, AA, WW */
	.word	0x70038cce		/* Q16ADD xr3, xr3, xr14, xr0, AA, WW */
	.word	0x700800ee		/* S32M2I xr3, $8 */
	sh	$8, 0($4)
	addiu	$4, $4, 2
	addiu	$5, $5, -1
	blez	$5, .Lm1_done
	nop

.Lm1_check4:
	sltiu	$8, $5, 4
	bnez	$8, .Lm1_tail
	nop
.Lm1_loop4:
	.word	0x70800050		/* S32LDD xr1, $4, 0 */
	.word	0x70800490		/* S32LDD xr2, $4, 4 */
	.word	0x705084f4		/* Q16SLL xr3, xr1, xr2, xr4, 1 */
	.word	0x71988574		/* Q16SLL xr5, xr1, xr2, xr6, 6 */
	.word	0x72e085f4		/* Q16SLL xr7, xr1, xr2, xr8, 11 */
	.word	0x72d10cf5		/* Q16SLR xr3, xr3, xr4, xr4, 11 - B0..3 */
	.word	0x72d99575		/* Q16SLR xr5, xr5, xr6, xr6, 11 - G0..3 */
	.word	0x72e21df5		/* Q16SLR xr7, xr7, xr8, xr8, 11 - R0..3 */
	.word	0x70028cce		/* Q16ADD xr3, xr3, xr10, xr0, AA, WW */
	.word	0x7002910e		/* Q16ADD xr4, xr4, xr10, xr0, AA, WW */
	.word	0x7002d54e		/* Q16ADD xr5, xr5, xr11, xr0, AA, WW */
	.word	0x7002d98e		/* Q16ADD xr6, xr6, xr11, xr0, AA, WW */
	.word	0x70031dce		/* Q16ADD xr7, xr7, xr12, xr0, AA, WW */
	.word	0x7003220e		/* Q16ADD xr8, xr8, xr12, xr0, AA, WW */
	.word	0x700f4cc3		/* D16MIN xr3, xr3, xr13 */
	.word	0x700f5103		/* D16MIN xr4, xr4, xr13 */
	.word	0x700f5543		/* D16MIN xr5, xr5, xr13 */
	.word	0x700f5983		/* D16MIN xr6, xr6, xr13 */
	.word	0x700f5dc3		/* D16MIN xr7, xr7, xr13 */
	.word	0x700f6203		/* D16MIN xr8, xr8, xr13 */
	.word	0x72910cf4		/* Q16SLL xr3, xr3, xr4, xr4, 10 */
	.word	0x71599574		/* Q16SLL xr5, xr5, xr6, xr6, 5 */
	addiu	$5, $5, -4
	.word	0x70014cce		/* Q16ADD xr3, xr3, xr5, xr0, AA, WW */
	.word	0x7001910e		/* Q16ADD xr4, xr4, xr6, xr0, AA, WW */
	.word	0x7001ccce		/* Q16ADD xr3, xr3, xr7, xr0, AA, WW */
	.word	0x7002110e		/* Q16ADD xr4, xr4, xr8, xr0, AA, WW */
	.word	0x70038cce		/* Q16ADD xr3, xr3, xr14, xr0, AA, WW */
	.word	0x7003910e		/* Q16ADD xr4, xr4, xr14, xr0, AA, WW */
	.word	0x708000d1		/* S32STD xr3, $4, 0 */
	.word	0x70800511		/* S32STD xr4, $4, 4 */
	sltiu	$8, $5, 4
	beqz	$8, .Lm1_loop4
	addiu	$4, $4, 8

.Lm1_tail:
	blez	$5, .Lm1_done
	nop
.Lm1_tloop:
	lhu	$8, 0($4)
	.word	0x7008006f		/* S32I2M xr1, $8 */
	.word	0x704004f4		/* Q16SLL xr3, xr1, xr0, xr0, 1 */
	.word	0x72c00cf5		/* Q16SLR xr3, xr3, xr0, xr0, 11 */
	.word	0x71800574		/* Q16SLL xr5, xr1, xr0, xr0, 6 */
	.word	0x72c01575		/* Q16SLR xr5, xr5, xr0, xr0, 11 */
	.word	0x72c005f4		/* Q16SLL xr7, xr1, xr0, xr0, 11 */
	.word	0x72c01df5		/* Q16SLR xr7, xr7, xr0, xr0, 11 */
	.word	0x70028cce		/* Q16ADD xr3, xr3, xr10, xr0, AA, WW */
	.word	0x7002d54e		/* Q16ADD xr5, xr5, xr11, xr0, AA, WW */
	.word	0x70031dce		/* Q16ADD xr7, xr7, xr12, xr0, AA, WW */
	.word	0x700f4cc3		/* D16MIN xr3, xr3, xr13 */
	.word	0x700f5543		/* D16MIN xr5, xr5, xr13 */
	.word	0x700f5dc3		/* D16MIN xr7, xr7, xr13 */
	.word	0x72800cf4		/* Q16SLL xr3, xr3, xr0, xr0, 10 */
	.word	0x71401574		/* Q16SLL xr5, xr5, xr0, xr0, 5 */
	addiu	$5, $5, -1
	.word	0x70014cce		/* Q16ADD xr3, xr3, xr5, xr0, AA, WW */
	.word	0x7001ccce		/* Q16ADD xr3, xr3, xr7, xr0, AA, WW */
	.word	0x70038cce		/* Q16ADD xr3, xr3, xr14, xr0, AA, WW */
	.word	0x700800ee		/* S32M2I xr3, $8 */
	sh	$8, 0($4)
	bgtz	$5, .Lm1_tloop
	addiu	$4, $4, 2
.Lm1_done:
	jr	$31
	nop

/* ==========================================================================
 * void gpu_blend_flat_m2_mxu(uint16_t *dst, uint32_t count, uint32_t pix15, uint32_t maskor)
 * Mode 2: max(0, dst_c - src_c) | maskor
 * 4 pixels/iter in MXU 1.0 (zero Blargg borrow chains!).
 * ========================================================================== */
	.globl	gpu_blend_flat_m2_mxu
	.type	gpu_blend_flat_m2_mxu, @function
gpu_blend_flat_m2_mxu:
	blez	$5, .Lm2_done
	andi	$6, $6, 0x7fff
	srl	$8, $6, 10
	sll	$9, $8, 16
	or	$8, $8, $9
	.word	0x700802af		/* S32I2M xr10, $8 */
	srl	$8, $6, 5
	andi	$8, $8, 0x1f
	sll	$9, $8, 16
	or	$8, $8, $9
	.word	0x700802ef		/* S32I2M xr11, $8 */
	andi	$8, $6, 0x1f
	sll	$9, $8, 16
	or	$8, $8, $9
	.word	0x7008032f		/* S32I2M xr12, $8 */
	sll	$8, $7, 16
	or	$8, $8, $7
	.word	0x700803af		/* S32I2M xr14, $8 */

	andi	$8, $4, 2
	beqz	$8, .Lm2_check4
	nop
	lhu	$8, 0($4)
	.word	0x7008006f		/* S32I2M xr1, $8 */
	.word	0x704004f4		/* Q16SLL xr3, xr1, xr0, xr0, 1 */
	.word	0x72c00cf5		/* Q16SLR xr3, xr3, xr0, xr0, 11 */
	.word	0x71800574		/* Q16SLL xr5, xr1, xr0, xr0, 6 */
	.word	0x72c01575		/* Q16SLR xr5, xr5, xr0, xr0, 11 */
	.word	0x72c005f4		/* Q16SLL xr7, xr1, xr0, xr0, 11 */
	.word	0x72c01df5		/* Q16SLR xr7, xr7, xr0, xr0, 11 */
	.word	0x73028cce		/* Q16ADD xr3, xr3, xr10, xr0, SS, WW */
	.word	0x7302d54e		/* Q16ADD xr5, xr5, xr11, xr0, SS, WW */
	.word	0x73031dce		/* Q16ADD xr7, xr7, xr12, xr0, SS, WW */
	.word	0x70080cc3		/* D16MAX xr3, xr3, xr0 */
	.word	0x70081543		/* D16MAX xr5, xr5, xr0 */
	.word	0x70081dc3		/* D16MAX xr7, xr7, xr0 */
	.word	0x72800cf4		/* Q16SLL xr3, xr3, xr0, xr0, 10 */
	.word	0x71401574		/* Q16SLL xr5, xr5, xr0, xr0, 5 */
	.word	0x70014cce		/* Q16ADD xr3, xr3, xr5, xr0, AA, WW */
	.word	0x7001ccce		/* Q16ADD xr3, xr3, xr7, xr0, AA, WW */
	.word	0x70038cce		/* Q16ADD xr3, xr3, xr14, xr0, AA, WW */
	.word	0x700800ee		/* S32M2I xr3, $8 */
	sh	$8, 0($4)
	addiu	$4, $4, 2
	addiu	$5, $5, -1
	blez	$5, .Lm2_done
	nop

.Lm2_check4:
	sltiu	$8, $5, 4
	bnez	$8, .Lm2_tail
	nop
.Lm2_loop4:
	.word	0x70800050		/* S32LDD xr1, $4, 0 */
	.word	0x70800490		/* S32LDD xr2, $4, 4 */
	.word	0x705084f4		/* Q16SLL xr3, xr1, xr2, xr4, 1 */
	.word	0x71988574		/* Q16SLL xr5, xr1, xr2, xr6, 6 */
	.word	0x72e085f4		/* Q16SLL xr7, xr1, xr2, xr8, 11 */
	.word	0x72d10cf5		/* Q16SLR xr3, xr3, xr4, xr4, 11 */
	.word	0x72d99575		/* Q16SLR xr5, xr5, xr6, xr6, 11 */
	.word	0x72e21df5		/* Q16SLR xr7, xr7, xr8, xr8, 11 */
	.word	0x73028cce		/* Q16ADD xr3, xr3, xr10, xr0, SS, WW */
	.word	0x7302910e		/* Q16ADD xr4, xr4, xr10, xr0, SS, WW */
	.word	0x7302d54e		/* Q16ADD xr5, xr5, xr11, xr0, SS, WW */
	.word	0x7302d98e		/* Q16ADD xr6, xr6, xr11, xr0, SS, WW */
	.word	0x73031dce		/* Q16ADD xr7, xr7, xr12, xr0, SS, WW */
	.word	0x7303220e		/* Q16ADD xr8, xr8, xr12, xr0, SS, WW */
	.word	0x70080cc3		/* D16MAX xr3, xr3, xr0 */
	.word	0x70081103		/* D16MAX xr4, xr4, xr0 */
	.word	0x70081543		/* D16MAX xr5, xr5, xr0 */
	.word	0x70081983		/* D16MAX xr6, xr6, xr0 */
	.word	0x70081dc3		/* D16MAX xr7, xr7, xr0 */
	.word	0x70082203		/* D16MAX xr8, xr8, xr0 */
	.word	0x72910cf4		/* Q16SLL xr3, xr3, xr4, xr4, 10 */
	.word	0x71599574		/* Q16SLL xr5, xr5, xr6, xr6, 5 */
	addiu	$5, $5, -4
	.word	0x70014cce		/* Q16ADD xr3, xr3, xr5, xr0, AA, WW */
	.word	0x7001910e		/* Q16ADD xr4, xr4, xr6, xr0, AA, WW */
	.word	0x7001ccce		/* Q16ADD xr3, xr3, xr7, xr0, AA, WW */
	.word	0x7002110e		/* Q16ADD xr4, xr4, xr8, xr0, AA, WW */
	.word	0x70038cce		/* Q16ADD xr3, xr3, xr14, xr0, AA, WW */
	.word	0x7003910e		/* Q16ADD xr4, xr4, xr14, xr0, AA, WW */
	.word	0x708000d1		/* S32STD xr3, $4, 0 */
	.word	0x70800511		/* S32STD xr4, $4, 4 */
	sltiu	$8, $5, 4
	beqz	$8, .Lm2_loop4
	addiu	$4, $4, 8

.Lm2_tail:
	blez	$5, .Lm2_done
	nop
.Lm2_tloop:
	lhu	$8, 0($4)
	.word	0x7008006f		/* S32I2M xr1, $8 */
	.word	0x704004f4		/* Q16SLL xr3, xr1, xr0, xr0, 1 */
	.word	0x72c00cf5		/* Q16SLR xr3, xr3, xr0, xr0, 11 */
	.word	0x71800574		/* Q16SLL xr5, xr1, xr0, xr0, 6 */
	.word	0x72c01575		/* Q16SLR xr5, xr5, xr0, xr0, 11 */
	.word	0x72c005f4		/* Q16SLL xr7, xr1, xr0, xr0, 11 */
	.word	0x72c01df5		/* Q16SLR xr7, xr7, xr0, xr0, 11 */
	.word	0x73028cce		/* Q16ADD xr3, xr3, xr10, xr0, SS, WW */
	.word	0x7302d54e		/* Q16ADD xr5, xr5, xr11, xr0, SS, WW */
	.word	0x73031dce		/* Q16ADD xr7, xr7, xr12, xr0, SS, WW */
	.word	0x70080cc3		/* D16MAX xr3, xr3, xr0 */
	.word	0x70081543		/* D16MAX xr5, xr5, xr0 */
	.word	0x70081dc3		/* D16MAX xr7, xr7, xr0 */
	.word	0x72800cf4		/* Q16SLL xr3, xr3, xr0, xr0, 10 */
	.word	0x71401574		/* Q16SLL xr5, xr5, xr0, xr0, 5 */
	addiu	$5, $5, -1
	.word	0x70014cce		/* Q16ADD xr3, xr3, xr5, xr0, AA, WW */
	.word	0x7001ccce		/* Q16ADD xr3, xr3, xr7, xr0, AA, WW */
	.word	0x70038cce		/* Q16ADD xr3, xr3, xr14, xr0, AA, WW */
	.word	0x700800ee		/* S32M2I xr3, $8 */
	sh	$8, 0($4)
	bgtz	$5, .Lm2_tloop
	addiu	$4, $4, 2
.Lm2_done:
	jr	$31
	nop

/* ==========================================================================
 * void gpu_poly_gouraud_fast(uint16_t *dst, uint32_t count, uint32_t gCol,
 *                            uint32_t gInc, uint32_t maskor)
 * Untextured Gouraud opaque span, bit-exact to gpuLightingRGB(gCol) | maskor.
 * 2-pixel unrolled with 32-bit aligned sw stores!
 * $4 = dst, $5 = count, $6 = gCol, $7 = gInc, 16($sp) = maskor (0 or 0x8000)
 * ========================================================================== */
	.globl	gpu_poly_gouraud_fast
	.type	gpu_poly_gouraud_fast, @function
gpu_poly_gouraud_fast:
	blez	$5, .Lgou_done
	lw	$15, 16($sp)		/* maskor */
	andi	$8, $4, 2
	beqz	$8, .Lgou_aligned
	nop
	/* Leading odd pixel */
	sll	$8, $6, 5
	andi	$8, $8, 0x7c00
	srl	$9, $6, 11
	andi	$9, $9, 0x03e0
	srl	$10, $6, 27
	or	$8, $8, $9
	or	$8, $8, $10
	or	$8, $8, $15
	sh	$8, 0($4)
	addu	$6, $6, $7
	addiu	$4, $4, 2
	addiu	$5, $5, -1
	blez	$5, .Lgou_done
	nop
.Lgou_aligned:
	sll	$14, $15, 16
	or	$14, $14, $15		/* [maskor | maskor] */
	sltiu	$8, $5, 2
	bnez	$8, .Lgou_tail
	nop
.Lgou_loop2:
	addu	$13, $6, $7		/* gCol1 */
	sll	$8, $6, 5
	andi	$8, $8, 0x7c00
	srl	$9, $6, 11
	andi	$9, $9, 0x03e0
	srl	$10, $6, 27
	or	$8, $8, $9
	or	$8, $8, $10		/* pix0 */
	sll	$11, $13, 5
	andi	$11, $11, 0x7c00
	srl	$12, $13, 11
	andi	$12, $12, 0x03e0
	srl	$9, $13, 27
	or	$11, $11, $12
	or	$11, $11, $9		/* pix1 */
	addu	$6, $13, $7		/* gCol += 2*gInc */
	sll	$11, $11, 16
	or	$8, $8, $11
	or	$8, $8, $14
	addiu	$5, $5, -2
	sw	$8, 0($4)
	sltiu	$9, $5, 2
	beqz	$9, .Lgou_loop2
	addiu	$4, $4, 4
.Lgou_tail:
	blez	$5, .Lgou_done
	nop
	sll	$8, $6, 5
	andi	$8, $8, 0x7c00
	srl	$9, $6, 11
	andi	$9, $9, 0x03e0
	srl	$10, $6, 27
	or	$8, $8, $9
	or	$8, $8, $10
	or	$8, $8, $15
	sh	$8, 0($4)
.Lgou_done:
	jr	$31
	nop

/* ==========================================================================
 * void gpu_poly_gt4_rgb_mxu(uint16_t *dst, uint32_t count, const uint8_t *tba8,
 *                           const uint32_t *uclut, const uint32_t *params)
 *
 * Colored Gouraud-Lit 4bpp Textured Polygon Span Kernel using MXU Q8MUL!
 * Zero LightLUT memory loads! 100% bit-exact to gpuLightingTXTGouraud().
 *
 * $4 = dst
 * $5 = count
 * $6 = tba8
 * $7 = uclut[16]: 0 if transparent; else (stp_or_mask << 24) | (B5<<16) | (G5<<8) | R5
 *      where stp_or_mask = 0x80 if (CF_MASKSET || (raw_clut & 0x8000)) else 0x01
 * 16($sp) -> params[6]:
 *   [0] l_u, [1] l_v, [2] l_u_inc, [3] l_v_inc, [4] l_gCol, [5] l_gInc
 * ========================================================================== */
	.globl	gpu_poly_gt4_rgb_mxu
	.type	gpu_poly_gt4_rgb_mxu, @function
gpu_poly_gt4_rgb_mxu:
	blez	$5, .Lgt4_ret
	lw	$8, 16($sp)		/* params */
	addiu	$sp, $sp, -24
	sw	$16, 0($sp)
	sw	$17, 4($sp)
	sw	$18, 8($sp)
	sw	$19, 12($sp)
	sw	$20, 16($sp)
	sw	$21, 20($sp)

	lw	$16, 0($8)		/* l_u */
	lw	$17, 4($8)		/* l_v */
	lw	$18, 8($8)		/* l_u_inc */
	lw	$19, 12($8)		/* l_v_inc */
	lw	$20, 16($8)		/* l_gCol */
	lw	$21, 20($8)		/* l_gInc */

	lui	$14, 0x001f
	ori	$14, $14, 0x001f	/* 0x001F001F */
	.word	0x700e03ef		/* S32I2M xr15, $14 */
	lui	$15, 0x0007
	ori	$15, $15, 0xf800	/* 0xff << 11 = 0x7F800 */

.Lgt4_loop:
	sll	$8, $17, 1
	and	$8, $8, $15		/* (l_v << 1) & (0xff << 11) */
	srl	$9, $16, 11
	andi	$9, $9, 0x7f		/* (l_u >> 11) & 0x7f */
	addu	$8, $8, $9
	addu	$8, $8, $6
	lbu	$8, 0($8)		/* b0 */
	srl	$9, $16, 8
	andi	$9, $9, 4
	srlv	$8, $8, $9
	andi	$8, $8, 0xf
	sll	$8, $8, 2
	addu	$8, $8, $7
	lw	$8, 0($8)		/* uclut[idx]: [flag | B5 | G5 | R5] */
	beqz	$8, .Lgt4_next
	addu	$16, $16, $18		/* delay slot: l_u += l_u_inc */

	/* Pack gRGB = [0 | gB5 | gG5 | gR5] from l_gCol ($20) */
	.word	0x700800af		/* S32I2M xr2, $8 */
	srl	$9, $20, 27		/* gR5 */
	srl	$10, $20, 16
	andi	$10, $10, 0x1f
	sll	$10, $10, 8
	or	$9, $9, $10
	srl	$10, $20, 5
	andi	$10, $10, 0x1f
	sll	$10, $10, 16
	or	$9, $9, $10
	.word	0x700900ef		/* S32I2M xr3, $9 */

	/* 3-channel hardware multiply + >>4 + clamp(31) with ZERO LightLUT loads! */
	.word	0x7014c938		/* Q8MUL xr4, xr2, xr3, xr5 - xr4=[0|B*gB], xr5=[G*gG|R*gR] */
	.word	0x71155135		/* Q16SLR xr4, xr4, xr5, xr5, 4 */
	.word	0x700fd103		/* D16MIN xr4, xr4, xr15 */
	.word	0x700fd543		/* D16MIN xr5, xr5, xr15 */
	.word	0x7009012e		/* S32M2I xr4, $9 - B' in [4:0] */
	.word	0x700a016e		/* S32M2I xr5, $10 - G' in [20:16], R' in [4:0] */
	srl	$8, $8, 16
	andi	$8, $8, 0x8000		/* STP/maskset bit from bit 31 */
	sll	$9, $9, 10		/* B' << 10 */
	srl	$11, $10, 11
	andi	$11, $11, 0x03e0	/* G' << 5 */
	andi	$10, $10, 0x001f	/* R' */
	or	$9, $9, $11
	or	$9, $9, $10
	or	$9, $9, $8
	sh	$9, 0($4)

.Lgt4_next:
	addu	$17, $17, $19		/* l_v += l_v_inc */
	addu	$20, $20, $21		/* l_gCol += l_gInc */
	addiu	$5, $5, -1
	bgtz	$5, .Lgt4_loop
	addiu	$4, $4, 2

	lw	$16, 0($sp)
	lw	$17, 4($sp)
	lw	$18, 8($sp)
	lw	$19, 12($sp)
	lw	$20, 16($sp)
	lw	$21, 20($sp)
	addiu	$sp, $sp, 24
.Lgt4_ret:
	jr	$31
	nop

/* ==========================================================================
 * void gpu_poly_gt8_rgb_mxu(uint16_t *dst, uint32_t count, const uint8_t *tba8,
 *                           const uint32_t *uclut, const uint32_t *params)
 * Colored Gouraud-Lit 8bpp Textured Polygon Span Kernel using MXU Q8MUL!
 * ========================================================================== */
	.globl	gpu_poly_gt8_rgb_mxu
	.type	gpu_poly_gt8_rgb_mxu, @function
gpu_poly_gt8_rgb_mxu:
	blez	$5, .Lgt8_ret
	lw	$8, 16($sp)
	addiu	$sp, $sp, -24
	sw	$16, 0($sp)
	sw	$17, 4($sp)
	sw	$18, 8($sp)
	sw	$19, 12($sp)
	sw	$20, 16($sp)
	sw	$21, 20($sp)

	lw	$16, 0($8)		/* l_u */
	lw	$17, 4($8)		/* l_v */
	lw	$18, 8($8)		/* l_u_inc */
	lw	$19, 12($8)		/* l_v_inc */
	lw	$20, 16($8)		/* l_gCol */
	lw	$21, 20($8)		/* l_gInc */

	lui	$14, 0x001f
	ori	$14, $14, 0x001f
	.word	0x700e03ef		/* S32I2M xr15, $14 */
	lui	$15, 0x0007
	ori	$15, $15, 0xf800	/* 0xff << 11 */

.Lgt8_loop:
	sll	$8, $17, 1
	and	$8, $8, $15
	srl	$9, $16, 10
	andi	$9, $9, 0xff
	addu	$8, $8, $9
	addu	$8, $8, $6
	lbu	$8, 0($8)		/* 8bpp index */
	sll	$8, $8, 2
	addu	$8, $8, $7
	lw	$8, 0($8)		/* uclut[idx] */
	beqz	$8, .Lgt8_next
	addu	$16, $16, $18		/* delay slot: l_u += l_u_inc */

	.word	0x700800af		/* S32I2M xr2, $8 */
	srl	$9, $20, 27
	srl	$10, $20, 16
	andi	$10, $10, 0x1f
	sll	$10, $10, 8
	or	$9, $9, $10
	srl	$10, $20, 5
	andi	$10, $10, 0x1f
	sll	$10, $10, 16
	or	$9, $9, $10
	.word	0x700900ef		/* S32I2M xr3, $9 */

	.word	0x7014c938		/* Q8MUL xr4, xr2, xr3, xr5 */
	.word	0x71155135		/* Q16SLR xr4, xr4, xr5, xr5, 4 */
	.word	0x700fd103		/* D16MIN xr4, xr4, xr15 */
	.word	0x700fd543		/* D16MIN xr5, xr5, xr15 */
	.word	0x7009012e		/* S32M2I xr4, $9 */
	.word	0x700a016e		/* S32M2I xr5, $10 */
	srl	$8, $8, 16
	andi	$8, $8, 0x8000
	sll	$9, $9, 10
	srl	$11, $10, 11
	andi	$11, $11, 0x03e0
	andi	$10, $10, 0x001f
	or	$9, $9, $11
	or	$9, $9, $10
	or	$9, $9, $8
	sh	$9, 0($4)

.Lgt8_next:
	addu	$17, $17, $19
	addu	$20, $20, $21
	addiu	$5, $5, -1
	bgtz	$5, .Lgt8_loop
	addiu	$4, $4, 2

	lw	$16, 0($sp)
	lw	$17, 4($sp)
	lw	$18, 8($sp)
	lw	$19, 12($sp)
	lw	$20, 16($sp)
	lw	$21, 20($sp)
	addiu	$sp, $sp, 24
.Lgt8_ret:
	jr	$31
	nop

/* ==========================================================================
 * void gpu_blit512_skip_mxu(const uint16_t *src, uint16_t *dst)
 * 512 -> 320 pixel_skip=1 downscale + BGR555 -> RGB565 in MXU 1.0!
 * Keeps indices [0,1, 3,4, 6,8, 9,11, 12,14] of every 16 pixels (32 iters = 320 px).
 * $4 = src (512 BGR555 px, 2-byte aligned), $5 = dst (320 RGB565 px, 4-byte aligned)
 * ========================================================================== */
	.globl	gpu_blit512_skip_mxu
	.type	gpu_blit512_skip_mxu, @function
gpu_blit512_skip_mxu:
	li	$6, 32			/* 32 iters * 10 out px = 320 px */
	addiu	$5, $5, -4		/* pre-dec for S32SDI +4 */
.Lb512_loop:
	pref	0, 64($4)
	/* Pack 10 pixels into 5 x 32-bit pairs:
	 * p01=(0,1), p23=(3,4), p45=(6,8), p67=(9,11), p89=(12,14) */
	lhu	$8,  0($4)
	lhu	$9,  2($4)
	lhu	$10, 6($4)
	lhu	$11, 8($4)
	sll	$9,  $9, 16
	or	$8,  $8, $9		/* (0,1) */
	sll	$11, $11, 16
	or	$10, $10, $11		/* (3,4) */
	.word	0x7008006f		/* S32I2M xr1, $8 */
	.word	0x700a00af		/* S32I2M xr2, $10 */
	.word	0x72d084f4		/* Q16SLL xr3, xr1, xr2, xr4, 11 */
	.word	0x71988574		/* Q16SLL xr5, xr1, xr2, xr6, 6 */
	.word	0x706085f4		/* Q16SLL xr7, xr1, xr2, xr8, 1 */
	.word	0x72d99575		/* Q16SLR xr5, xr5, xr6, xr6, 11 */
	.word	0x72e21df5		/* Q16SLR xr7, xr7, xr8, xr8, 11 */
	.word	0x71999574		/* Q16SLL xr5, xr5, xr6, xr6, 6 */
	.word	0x70014cce		/* Q16ADD xr3, xr3, xr5, xr0, AA, WW */
	.word	0x7001910e		/* Q16ADD xr4, xr4, xr6, xr0, AA, WW */
	.word	0x7001ccce		/* Q16ADD xr3, xr3, xr7, xr0, AA, WW */
	.word	0x7002110e		/* Q16ADD xr4, xr4, xr8, xr0, AA, WW */
	.word	0x70a004d5		/* S32SDI xr3, $5, 4 */
	.word	0x70a00515		/* S32SDI xr4, $5, 4 */

	lhu	$8,  12($4)		/* 6 */
	lhu	$9,  16($4)		/* 8 */
	lhu	$10, 18($4)		/* 9 */
	lhu	$11, 22($4)		/* 11 */
	sll	$9,  $9, 16
	or	$8,  $8, $9		/* (6,8) */
	sll	$11, $11, 16
	or	$10, $10, $11		/* (9,11) */
	.word	0x7008006f		/* S32I2M xr1, $8 */
	.word	0x700a00af		/* S32I2M xr2, $10 */
	.word	0x72d084f4		/* Q16SLL xr3, xr1, xr2, xr4, 11 */
	.word	0x71988574		/* Q16SLL xr5, xr1, xr2, xr6, 6 */
	.word	0x706085f4		/* Q16SLL xr7, xr1, xr2, xr8, 1 */
	.word	0x72d99575		/* Q16SLR xr5, xr5, xr6, xr6, 11 */
	.word	0x72e21df5		/* Q16SLR xr7, xr7, xr8, xr8, 11 */
	.word	0x71999574		/* Q16SLL xr5, xr5, xr6, xr6, 6 */
	.word	0x70014cce		/* Q16ADD xr3, xr3, xr5, xr0, AA, WW */
	.word	0x7001910e		/* Q16ADD xr4, xr4, xr6, xr0, AA, WW */
	.word	0x7001ccce		/* Q16ADD xr3, xr3, xr7, xr0, AA, WW */
	.word	0x7002110e		/* Q16ADD xr4, xr4, xr8, xr0, AA, WW */
	.word	0x70a004d5		/* S32SDI xr3, $5, 4 */
	.word	0x70a00515		/* S32SDI xr4, $5, 4 */

	lhu	$8,  24($4)		/* 12 */
	lhu	$9,  28($4)		/* 14 */
	sll	$9,  $9, 16
	or	$8,  $8, $9		/* (12,14) */
	.word	0x7008006f		/* S32I2M xr1, $8 */
	.word	0x72c004f4		/* Q16SLL xr3, xr1, xr0, xr0, 11 */
	.word	0x71800574		/* Q16SLL xr5, xr1, xr0, xr0, 6 */
	.word	0x704005f4		/* Q16SLL xr7, xr1, xr0, xr0, 1 */
	.word	0x72c01575		/* Q16SLR xr5, xr5, xr0, xr0, 11 */
	.word	0x72c01df5		/* Q16SLR xr7, xr7, xr0, xr0, 11 */
	.word	0x71801574		/* Q16SLL xr5, xr5, xr0, xr0, 6 */
	addiu	$6, $6, -1
	.word	0x70014cce		/* Q16ADD xr3, xr3, xr5, xr0, AA, WW */
	.word	0x7001ccce		/* Q16ADD xr3, xr3, xr7, xr0, AA, WW */
	addiu	$4, $4, 32
	bgtz	$6, .Lb512_loop
	.word	0x70a004d5		/* S32SDI xr3, $5, 4 */
	jr	$31
	nop

/* ==========================================================================
 * void gpu_blit640_skip_mxu(const uint16_t *src, uint16_t *dst)
 * 640 -> 320 pixel_skip=1 downscale + BGR555 -> RGB565 in MXU 1.0!
 * Keeps every even pixel [0,2,4,6,...] (80 iters * 4 out px = 320 px).
 * ========================================================================== */
	.globl	gpu_blit640_skip_mxu
	.type	gpu_blit640_skip_mxu, @function
gpu_blit640_skip_mxu:
	li	$6, 80			/* 80 iters * 4 out px = 320 px */
	addiu	$5, $5, -4
.Lb640_loop:
	pref	0, 64($4)
	lhu	$8,  0($4)
	lhu	$9,  4($4)
	lhu	$10, 8($4)
	lhu	$11, 12($4)
	sll	$9,  $9, 16
	or	$8,  $8, $9
	sll	$11, $11, 16
	or	$10, $10, $11
	.word	0x7008006f		/* S32I2M xr1, $8 */
	.word	0x700a00af		/* S32I2M xr2, $10 */
	.word	0x72d084f4		/* Q16SLL xr3, xr1, xr2, xr4, 11 */
	.word	0x71988574		/* Q16SLL xr5, xr1, xr2, xr6, 6 */
	.word	0x706085f4		/* Q16SLL xr7, xr1, xr2, xr8, 1 */
	.word	0x72d99575		/* Q16SLR xr5, xr5, xr6, xr6, 11 */
	.word	0x72e21df5		/* Q16SLR xr7, xr7, xr8, xr8, 11 */
	.word	0x71999574		/* Q16SLL xr5, xr5, xr6, xr6, 6 */
	addiu	$6, $6, -1
	.word	0x70014cce		/* Q16ADD xr3, xr3, xr5, xr0, AA, WW */
	.word	0x7001910e		/* Q16ADD xr4, xr4, xr6, xr0, AA, WW */
	.word	0x7001ccce		/* Q16ADD xr3, xr3, xr7, xr0, AA, WW */
	.word	0x7002110e		/* Q16ADD xr4, xr4, xr8, xr0, AA, WW */
	addiu	$4, $4, 16
	.word	0x70a004d5		/* S32SDI xr3, $5, 4 */
	bgtz	$6, .Lb640_loop
	.word	0x70a00515		/* S32SDI xr4, $5, 4 */
	jr	$31
	nop
