/*
 * Ingenic JZ4760B MXU SIMD kernels for spu_pcsxrearmed.
 *
 * All MXU opcodes use verified .word encodings (checked against Ingenic
 * mipsel-linux-as) so GCC 7.3+ binutils assembles cleanly without MXU gas.
 * Requires kernel MXU context-switch support (auto-detected via /proc/cpuinfo).
 */
	.text
	.set	noreorder

/* void jz_mxu_enable(void) */
	.globl	jz_mxu_enable
	.type	jz_mxu_enable, @function
jz_mxu_enable:
	li	$2, 1
	.word	0x7002042f		/* S32I2M xr16, $2 (enable MXU) */
	nop				/* >=3-cycle JZ4760B XBurst hazard */
	nop
	nop
	nop
	jr	$31
	nop

/* void mix_chan_mxu(int *SSumLR, const int *ChanBuf, int count, unsigned int lv_rv)
 * a0 ($4) = SSumLR
 * a1 ($5) = ChanBuf
 * a2 ($6) = count
 * a3 ($7) = (lv << 16) | (rv & 0xffff)
 */
	.globl	mix_chan_mxu
	.type	mix_chan_mxu, @function
mix_chan_mxu:
	blez	$6, 4f
	addiu	$5, $5, -4		/* pre-decrement for S32LDI +4 */
	.word	0x7007006f		/* S32I2M xr1, $7 */
	slti	$8, $6, 2
	bnez	$8, 2f
	nop
1:
	.word	0x70a00494		/* S32LDI xr2,  $5, 4  (sval0) */
	.word	0x70a005d4		/* S32LDI xr7,  $5, 4  (sval1) */
	.word	0x70800150		/* S32LDD xr5,  $4, 0  (L0) */
	.word	0x70800590		/* S32LDD xr6,  $4, 4  (R0) */
	.word	0x705048c8		/* D16MUL xr3,  xr2, xr1, xr4, LW */
	.word	0x70645e08		/* D16MUL xr8,  xr7, xr1, xr9, LW */
	.word	0x70800a90		/* S32LDD xr10, $4, 8  (L1) */
	.word	0x70800ed0		/* S32LDD xr11, $4, 12 (R1) */
	.word	0x73910cf3		/* D32SAR xr3,  xr3, xr4, xr4, 14 */
	.word	0x73a66233		/* D32SAR xr8,  xr8, xr9, xr9, 14 */
	addiu	$6, $6, -2
	.word	0x7000d558		/* D32ADD xr5,  xr5,  xr3, xr0, AA */
	.word	0x70011998		/* D32ADD xr6,  xr6,  xr4, xr0, AA */
	.word	0x70022a98		/* D32ADD xr10, xr10, xr8, xr0, AA */
	.word	0x70026ed8		/* D32ADD xr11, xr11, xr9, xr0, AA */
	.word	0x70800151		/* S32STD xr5,  $4, 0 */
	.word	0x70800591		/* S32STD xr6,  $4, 4 */
	.word	0x70800a91		/* S32STD xr10, $4, 8 */
	.word	0x70800ed1		/* S32STD xr11, $4, 12 */
	slti	$8, $6, 2
	beqz	$8, 1b
	addiu	$4, $4, 16
2:
	blez	$6, 4f
	nop
	.word	0x70a00494		/* S32LDI xr2, $5, 4 */
	.word	0x70800150		/* S32LDD xr5, $4, 0 */
	.word	0x70800590		/* S32LDD xr6, $4, 4 */
	.word	0x705048c8		/* D16MUL xr3, xr2, xr1, xr4, LW */
	.word	0x73910cf3		/* D32SAR xr3, xr3, xr4, xr4, 14 */
	.word	0x7000d558		/* D32ADD xr5, xr5, xr3, xr0, AA */
	.word	0x70011998		/* D32ADD xr6, xr6, xr4, xr0, AA */
	.word	0x70800151		/* S32STD xr5, $4, 0 */
	.word	0x70800591		/* S32STD xr6, $4, 4 */
4:
	jr	$31
	nop

/* void mix_chan_rvb_mxu(int *SSumLR, const int *ChanBuf, int count, unsigned int lv_rv, int *rvb)
 * a0 ($4) = SSumLR
 * a1 ($5) = ChanBuf
 * a2 ($6) = count
 * a3 ($7) = (lv << 16) | (rv & 0xffff)
 * 16($sp) -> $8 = rvb
 */
	.globl	mix_chan_rvb_mxu
	.type	mix_chan_rvb_mxu, @function
mix_chan_rvb_mxu:
	blez	$6, 2f
	lw	$8, 16($sp)
	addiu	$5, $5, -4		/* pre-decrement for S32LDI +4 */
	.word	0x7007006f		/* S32I2M xr1, $7 */
1:
	.word	0x70a00494		/* S32LDI xr2,  $5, 4 */
	.word	0x70800150		/* S32LDD xr5,  $4, 0 */
	.word	0x70800590		/* S32LDD xr6,  $4, 4 */
	.word	0x705048c8		/* D16MUL xr3,  xr2, xr1, xr4, LW */
	.word	0x71000310		/* S32LDD xr12, $8, 0 */
	.word	0x71000750		/* S32LDD xr13, $8, 4 */
	.word	0x73910cf3		/* D32SAR xr3,  xr3, xr4, xr4, 14 */
	addiu	$6, $6, -1
	.word	0x7000d558		/* D32ADD xr5,  xr5,  xr3, xr0, AA */
	.word	0x70011998		/* D32ADD xr6,  xr6,  xr4, xr0, AA */
	.word	0x7000f318		/* D32ADD xr12, xr12, xr3, xr0, AA */
	.word	0x70013758		/* D32ADD xr13, xr13, xr4, xr0, AA */
	.word	0x70800151		/* S32STD xr5,  $4, 0 */
	.word	0x70800591		/* S32STD xr6,  $4, 4 */
	.word	0x71000311		/* S32STD xr12, $8, 0 */
	.word	0x71000751		/* S32STD xr13, $8, 4 */
	addiu	$4, $4, 8
	bgtz	$6, 1b
	addiu	$8, $8, 8
2:
	jr	$31
	nop

/* void finish_samples_mxu(int *SSumLR, short *pS, int ns_to)
 * a0 ($4) = SSumLR
 * a1 ($5) = pS (must be 4-byte aligned)
 * a2 ($6) = ns_to (number of stereo pairs)
 */
	.globl	finish_samples_mxu
	.type	finish_samples_mxu, @function
finish_samples_mxu:
	blez	$6, 4f
	addiu	$5, $5, -4		/* pre-decrement for S32SDI +4 */
	li	$8, 32767
	li	$9, -32768
	.word	0x700803af		/* S32I2M xr14, $8 */
	.word	0x700903ef		/* S32I2M xr15, $9 */
	slti	$8, $6, 2
	bnez	$8, 2f
	nop
1:
	.word	0x70800050		/* S32LDD xr1, $4, 0 */
	.word	0x70800490		/* S32LDD xr2, $4, 4 */
	.word	0x70800950		/* S32LDD xr5, $4, 8 */
	.word	0x70800d90		/* S32LDD xr6, $4, 12 */
	.word	0x70800011		/* S32STD xr0, $4, 0 */
	.word	0x70800411		/* S32STD xr0, $4, 4 */
	.word	0x70800811		/* S32STD xr0, $4, 8 */
	.word	0x70800c11		/* S32STD xr0, $4, 12 */
	.word	0x70078443		/* S32MIN xr1, xr1, xr14 */
	.word	0x70078883		/* S32MIN xr2, xr2, xr14 */
	.word	0x70079543		/* S32MIN xr5, xr5, xr14 */
	.word	0x70079983		/* S32MIN xr6, xr6, xr14 */
	.word	0x7003c443		/* S32MAX xr1, xr1, xr15 */
	.word	0x7003c883		/* S32MAX xr2, xr2, xr15 */
	.word	0x7003d543		/* S32MAX xr5, xr5, xr15 */
	.word	0x7003d983		/* S32MAX xr6, xr6, xr15 */
	addiu	$6, $6, -2
	.word	0x731048fd		/* S32SFL xr3, xr2, xr1, xr4, ptn3 */
	.word	0x732159fd		/* S32SFL xr7, xr6, xr5, xr8, ptn3 */
	.word	0x70a00515		/* S32SDI xr4, $5, 4 */
	.word	0x70a00615		/* S32SDI xr8, $5, 4 */
	slti	$8, $6, 2
	beqz	$8, 1b
	addiu	$4, $4, 16
2:
	blez	$6, 4f
	nop
	.word	0x70800050		/* S32LDD xr1, $4, 0 */
	.word	0x70800490		/* S32LDD xr2, $4, 4 */
	.word	0x70800011		/* S32STD xr0, $4, 0 */
	.word	0x70800411		/* S32STD xr0, $4, 4 */
	.word	0x70078443		/* S32MIN xr1, xr1, xr14 */
	.word	0x70078883		/* S32MIN xr2, xr2, xr14 */
	.word	0x7003c443		/* S32MAX xr1, xr1, xr15 */
	.word	0x7003c883		/* S32MAX xr2, xr2, xr15 */
	.word	0x731048fd		/* S32SFL xr3, xr2, xr1, xr4, ptn3 */
	.word	0x70a00515		/* S32SDI xr4, $5, 4 */
4:
	jr	$31
	nop
