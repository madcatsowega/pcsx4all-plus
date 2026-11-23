/***************************************************************************
*   Copyright (C) 2010 PCSX4ALL Team                                      *
*   Copyright (C) 2010 Unai                                               *
*   Copyright (C) 2016 Senquack (dansilsby <AT> gmail <DOT> com)          *
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
*   This program is distributed in the hope that it will be useful,       *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
*   GNU General Public License for more details.                          *
*                                                                         *
*   You should have received a copy of the GNU General Public License     *
*   along with this program; if not, write to the                         *
*   Free Software Foundation, Inc.,                                       *
*   51 Franklin Street, Fifth Floor, Boston, MA 02111-1307 USA.           *
***************************************************************************/

///////////////////////////////////////////////////////////////////////////////
// Inner loop driver instantiation file

///////////////////////////////////////////////////////////////////////////////
//  Option Masks (CF template paramter)
#define  CF_LIGHT     ((CF>> 0)&1) // Lighting
#define  CF_BLEND     ((CF>> 1)&1) // Blending
#define  CF_MASKCHECK ((CF>> 2)&1) // Mask bit check
#define  CF_BLENDMODE ((CF>> 3)&3) // Blend mode   0..3
#define  CF_TEXTMODE  ((CF>> 5)&3) // Texture mode 1..3 (0: texturing disabled)
#define  CF_GOURAUD   ((CF>> 7)&1) // Gouraud shading
#define  CF_MASKSET   ((CF>> 8)&1) // Mask bit set
#define  CF_DITHER    ((CF>> 9)&1) // Dithering
#define  CF_BLITMASK  ((CF>>10)&1) // blit_mask check (skip rendering pixels
                                   //  that wouldn't end up displayed on
                                   //  low-res screen using simple downscaler)

#ifdef __arm__
#ifndef ENABLE_GPU_ARMV7
/* ARMv5 */
#include "gpu_inner_blend_arm5.h"
#else
/* ARMv7 optimized */
#include "gpu_inner_blend_arm7.h"
#endif
#else
#include "gpu_inner_blend.h"
#endif

#include "gpu_inner_quantization.h"
#include "gpu_inner_light.h"

// If defined, Gouraud colors are fixed-point 5.11, otherwise they are 8.16
// This is only for debugging/verification of low-precision colors in C.
// Low-precision Gouraud is intended for use by SIMD-optimized inner drivers
// which get/use Gouraud colors in SIMD registers.
//#define GPU_GOURAUD_LOW_PRECISION

// How many bits of fixed-point precision GouraudColor uses
#ifdef GPU_GOURAUD_LOW_PRECISION
#define GPU_GOURAUD_FIXED_BITS 11
#else
#define GPU_GOURAUD_FIXED_BITS 16
#endif

// Used to pass Gouraud colors to gpuPixelSpanFn() (lines)
struct GouraudColor {
#ifdef GPU_GOURAUD_LOW_PRECISION
	u16 r, g, b;
	s16 r_incr, g_incr, b_incr;
#else
	u32 r, g, b;
	s32 r_incr, g_incr, b_incr;
#endif
};

static inline u16 gpuGouraudColor15bpp(u32 r, u32 g, u32 b)
{
	r >>= GPU_GOURAUD_FIXED_BITS;
	g >>= GPU_GOURAUD_FIXED_BITS;
	b >>= GPU_GOURAUD_FIXED_BITS;

#ifndef GPU_GOURAUD_LOW_PRECISION
	// High-precision Gouraud colors are 8-bit + fractional
	r >>= 3;  g >>= 3;  b >>= 3;
#endif

	return r | (g << 5) | (b << 10);
}

///////////////////////////////////////////////////////////////////////////////
//  GPU Pixel span operations generator gpuPixelSpanFn<>
//  Oct 2016: Created/adapted from old gpuPixelFn by senquack:
//  Original gpuPixelFn was used to draw lines one pixel at a time. I wrote
//  new line algorithms that draw lines using horizontal/vertical/diagonal
//  spans of pixels, necessitating new pixel-drawing function that could
//  not only render spans of pixels, but gouraud-shade them as well.
//  This speeds up line rendering and would allow tile-rendering (untextured
//  rectangles) to use the same set of functions. Since tiles are always
//  monochrome, they simply wouldn't use the extra set of 32 gouraud-shaded
//  gpuPixelSpanFn functions (TODO?).
//
// NOTE: While the PS1 framebuffer is 16 bit, we use 8-bit pointers here,
//       so that pDst can be incremented directly by 'incr' parameter
//       without having to shift it before use.
template<int CF>
static u8* gpuPixelSpanFn(u8* pDst, uintptr_t data, ptrdiff_t incr, size_t len)
{
	// Blend func can save an operation if it knows uSrc MSB is
	//  unset. For untextured prims, this is always true.
	const bool skip_uSrc_mask = true;

	u16 col;
	struct GouraudColor * gcPtr;
	u32 r, g, b;
	s32 r_incr, g_incr, b_incr;

	if (CF_GOURAUD) {
		gcPtr = (GouraudColor*)data;
		r = gcPtr->r;  r_incr = gcPtr->r_incr;
		g = gcPtr->g;  g_incr = gcPtr->g_incr;
		b = gcPtr->b;  b_incr = gcPtr->b_incr;
	} else {
		col = (u16)data;
	}

	do {
		if (!CF_GOURAUD)
		{   // NO GOURAUD
			if (!CF_MASKCHECK && !CF_BLEND) {
				if (CF_MASKSET) { *(u16*)pDst = col | 0x8000; }
				else            { *(u16*)pDst = col;          }
			} else if (CF_MASKCHECK && !CF_BLEND) {
				if (!(*(u16*)pDst & 0x8000)) {
					if (CF_MASKSET) { *(u16*)pDst = col | 0x8000; }
					else            { *(u16*)pDst = col;          }
				}
			} else {
				u16 uDst = *(u16*)pDst;
				if (CF_MASKCHECK) { if (uDst & 0x8000) goto endpixel; }

				u16 uSrc = col;

				if (CF_BLEND)
					uSrc = gpuBlending<CF_BLENDMODE, skip_uSrc_mask>(uSrc, uDst);

				if (CF_MASKSET) { *(u16*)pDst = uSrc | 0x8000; }
				else            { *(u16*)pDst = uSrc;          }
			}

		} else
		{   // GOURAUD

			if (!CF_MASKCHECK && !CF_BLEND) {
				col = gpuGouraudColor15bpp(r, g, b);
				if (CF_MASKSET) { *(u16*)pDst = col | 0x8000; }
				else            { *(u16*)pDst = col;          }
			} else if (CF_MASKCHECK && !CF_BLEND) {
				col = gpuGouraudColor15bpp(r, g, b);
				if (!(*(u16*)pDst & 0x8000)) {
					if (CF_MASKSET) { *(u16*)pDst = col | 0x8000; }
					else            { *(u16*)pDst = col;          }
				}
			} else {
				u16 uDst = *(u16*)pDst;
				if (CF_MASKCHECK) { if (uDst & 0x8000) goto endpixel; }
				col = gpuGouraudColor15bpp(r, g, b);

				u16 uSrc = col;

				// Blend func can save an operation if it knows uSrc MSB is
				//  unset. For untextured prims, this is always true.
				const bool skip_uSrc_mask = true;

				if (CF_BLEND)
					uSrc = gpuBlending<CF_BLENDMODE, skip_uSrc_mask>(uSrc, uDst);

				if (CF_MASKSET) { *(u16*)pDst = uSrc | 0x8000; }
				else            { *(u16*)pDst = uSrc;          }
			}
		}

endpixel:
		if (CF_GOURAUD) {
			r += r_incr;
			g += g_incr;
			b += b_incr;
		}
		pDst += incr;
	} while (len-- > 1);

	// Note from senquack: Normally, I'd prefer to write a 'do {} while (--len)'
	//  loop, or even a for() loop, however, on MIPS platforms anything but the
	//  'do {} while (len-- > 1)' tends to generate very unoptimal asm, with
	//  many unneeded MULs/ADDs/branches at the ends of these functions.
	//  If you change the loop structure above, be sure to compare the quality
	//  of the generated code!!

	if (CF_GOURAUD) {
		gcPtr->r = r;
		gcPtr->g = g;
		gcPtr->b = b;
	}
	return pDst;
}

static u8* PixelSpanNULL(u8* pDst, uintptr_t data, ptrdiff_t incr, size_t len)
{
	#ifdef ENABLE_GPU_LOG_SUPPORT
		fprintf(stdout,"PixelSpanNULL()\n");
	#endif
	return pDst;
}

///////////////////////////////////////////////////////////////////////////////
//  PixelSpan (lines) innerloops driver
typedef u8* (*PSD)(u8* dst, uintptr_t data, ptrdiff_t incr, size_t len);

const PSD gpuPixelSpanDrivers[64] =
{ 
	// Array index | 'CF' template field | Field value
	// ------------+---------------------+----------------
	// Bit 0       | CF_BLEND            | off (0), on (1)
	// Bit 1       | CF_MASKCHECK        | off (0), on (1)
	// Bit 3:2     | CF_BLENDMODE        | 0..3
	// Bit 4       | CF_MASKSET          | off (0), on (1)
	// Bit 5       | CF_GOURAUD          | off (0), on (1)
	//
	// NULL entries are ones for which blending is disabled and blend-mode
	//  field is non-zero, which is obviously invalid.

	// Flat-shaded
	gpuPixelSpanFn<0x00<<1>,         gpuPixelSpanFn<0x01<<1>,         gpuPixelSpanFn<0x02<<1>,         gpuPixelSpanFn<0x03<<1>,
	PixelSpanNULL,                   gpuPixelSpanFn<0x05<<1>,         PixelSpanNULL,                   gpuPixelSpanFn<0x07<<1>,
	PixelSpanNULL,                   gpuPixelSpanFn<0x09<<1>,         PixelSpanNULL,                   gpuPixelSpanFn<0x0B<<1>,
	PixelSpanNULL,                   gpuPixelSpanFn<0x0D<<1>,         PixelSpanNULL,                   gpuPixelSpanFn<0x0F<<1>,

	// Flat-shaded + PixelMSB (CF_MASKSET)
	gpuPixelSpanFn<(0x00<<1)|0x100>, gpuPixelSpanFn<(0x01<<1)|0x100>, gpuPixelSpanFn<(0x02<<1)|0x100>, gpuPixelSpanFn<(0x03<<1)|0x100>,
	PixelSpanNULL,                   gpuPixelSpanFn<(0x05<<1)|0x100>, PixelSpanNULL,                   gpuPixelSpanFn<(0x07<<1)|0x100>,
	PixelSpanNULL,                   gpuPixelSpanFn<(0x09<<1)|0x100>, PixelSpanNULL,                   gpuPixelSpanFn<(0x0B<<1)|0x100>,
	PixelSpanNULL,                   gpuPixelSpanFn<(0x0D<<1)|0x100>, PixelSpanNULL,                   gpuPixelSpanFn<(0x0F<<1)|0x100>,

	// Gouraud-shaded (CF_GOURAUD)
	gpuPixelSpanFn<(0x00<<1)|0x80>,  gpuPixelSpanFn<(0x01<<1)|0x80>,  gpuPixelSpanFn<(0x02<<1)|0x80>,  gpuPixelSpanFn<(0x03<<1)|0x80>,
	PixelSpanNULL,                   gpuPixelSpanFn<(0x05<<1)|0x80>,  PixelSpanNULL,                   gpuPixelSpanFn<(0x07<<1)|0x80>,
	PixelSpanNULL,                   gpuPixelSpanFn<(0x09<<1)|0x80>,  PixelSpanNULL,                   gpuPixelSpanFn<(0x0B<<1)|0x80>,
	PixelSpanNULL,                   gpuPixelSpanFn<(0x0D<<1)|0x80>,  PixelSpanNULL,                   gpuPixelSpanFn<(0x0F<<1)|0x80>,

	// Gouraud-shaded (CF_GOURAUD) + PixelMSB (CF_MASKSET)
	gpuPixelSpanFn<(0x00<<1)|0x180>, gpuPixelSpanFn<(0x01<<1)|0x180>, gpuPixelSpanFn<(0x02<<1)|0x180>, gpuPixelSpanFn<(0x03<<1)|0x180>,
	PixelSpanNULL,                   gpuPixelSpanFn<(0x05<<1)|0x180>, PixelSpanNULL,                   gpuPixelSpanFn<(0x07<<1)|0x180>,
	PixelSpanNULL,                   gpuPixelSpanFn<(0x09<<1)|0x180>, PixelSpanNULL,                   gpuPixelSpanFn<(0x0B<<1)|0x180>,
	PixelSpanNULL,                   gpuPixelSpanFn<(0x0D<<1)|0x180>, PixelSpanNULL,                   gpuPixelSpanFn<(0x0F<<1)|0x180>
};

///////////////////////////////////////////////////////////////////////////////
//  Fast 32-bit aligned VRAM span fill with MIPS pref 30 (cached PS1 VRAM only)
static inline void gpuFillSpan16(u16 * __restrict__ pDst, u32 count, u16 data)
{
	if (count >= 4) {
		if ((uintptr_t)pDst & 2) {
			*pDst++ = data;
			count--;
		}
		u32 data32 = ((u32)data << 16) | data;
		u32 * __restrict__ pDst32 = (u32 *)pDst;
#ifdef __mips__
		if (count >= 32) {
			while ((uintptr_t)pDst32 & 31) {
				*pDst32++ = data32;
				count -= 2;
			}
			while (count >= 16) {
				__asm__ __volatile__("pref 30, 0(%0)" : : "r"(pDst32));
				pDst32[0] = data32; pDst32[1] = data32;
				pDst32[2] = data32; pDst32[3] = data32;
				pDst32[4] = data32; pDst32[5] = data32;
				pDst32[6] = data32; pDst32[7] = data32;
				pDst32 += 8;
				count -= 16;
			}
		}
#endif
		while (count >= 8) {
			pDst32[0] = data32; pDst32[1] = data32;
			pDst32[2] = data32; pDst32[3] = data32;
			pDst32 += 4;
			count -= 8;
		}
		while (count >= 2) {
			*pDst32++ = data32;
			count -= 2;
		}
		pDst = (u16 *)pDst32;
	}
	while (count--) {
		*pDst++ = data;
	}
}

///////////////////////////////////////////////////////////////////////////////
//  GPU Tiles innerloops generator

template<int CF>
static void gpuTileSpanFn(u16 *pDst, u32 count, u16 data)
{
	if (!CF_MASKCHECK && !CF_BLEND) {
		if (CF_MASKSET) { data = data | 0x8000; }
		gpuFillSpan16(pDst, count, data);
	} else if (CF_MASKCHECK && !CF_BLEND) {
		if (CF_MASKSET) { data = data | 0x8000; }
		do { if (!(*pDst&0x8000)) { *pDst = data; } pDst++; } while (--count);
	} else
	{
		// Blend func can save an operation if it knows uSrc MSB is
		//  unset. For untextured prims, this is always true.
		const bool skip_uSrc_mask = true;

		u16 uSrc, uDst;
		do
		{
			if (CF_MASKCHECK || CF_BLEND) { uDst = *pDst; }
			if (CF_MASKCHECK) { if (uDst&0x8000) goto endtile; }

			uSrc = data;

			if (CF_BLEND)
				uSrc = gpuBlending<CF_BLENDMODE, skip_uSrc_mask>(uSrc, uDst);

			if (CF_MASKSET) { *pDst = uSrc | 0x8000; }
			else            { *pDst = uSrc;          }

			//senquack - Did not apply "Silent Hill" mask-bit fix to here.
			// It is hard to tell from scarce documentation available and
			//  lack of comments in code, but I believe the tile-span
			//  functions here should not bother to preserve any source MSB,
			//  as they are not drawing from a texture.
endtile:
			pDst++;
		}
		while (--count);
	}
}

static void TileNULL(u16 *pDst, u32 count, u16 data)
{
	#ifdef ENABLE_GPU_LOG_SUPPORT
		fprintf(stdout,"TileNULL()\n");
	#endif
}

///////////////////////////////////////////////////////////////////////////////
//  Tiles innerloops driver
typedef void (*PT)(u16 *pDst, u32 count, u16 data);

// Template instantiation helper macros
#define TI(cf) gpuTileSpanFn<(cf)>
#define TN     TileNULL
#define TIBLOCK(ub) \
	TI((ub)|0x00), TI((ub)|0x02), TI((ub)|0x04), TI((ub)|0x06), \
	TN,            TI((ub)|0x0a), TN,            TI((ub)|0x0e), \
	TN,            TI((ub)|0x12), TN,            TI((ub)|0x16), \
	TN,            TI((ub)|0x1a), TN,            TI((ub)|0x1e)

const PT gpuTileSpanDrivers[32] = {
	TIBLOCK(0<<8), TIBLOCK(1<<8)
};

#undef TI
#undef TN
#undef TIBLOCK


///////////////////////////////////////////////////////////////////////////////
//  GPU Sprites innerloops generator

template<int CF>
static void gpuSpriteSpanFn(u16 *pDst, u32 count, u8* pTxt, u32 u0)
{
	// Blend func can save an operation if it knows uSrc MSB is unset.
	//  Untextured prims can always skip (source color always comes with MSB=0).
	//  For textured prims, lighting funcs always return it unset. (bonus!)
	const bool skip_uSrc_mask = (!CF_TEXTMODE) || CF_LIGHT;

	u16 uSrc, uDst, srcMSB;
	u32 u0_mask = gpu_unai.TextureWindow[2];

	u8 r5, g5, b5;
	if (CF_LIGHT) {
		r5 = gpu_unai.r5;
		g5 = gpu_unai.g5;
		b5 = gpu_unai.b5;
	}

	if (CF_TEXTMODE==3) {
		// Texture is accessed byte-wise, so adjust mask if 16bpp
		u0_mask <<= 1;
	}

	const u16 *CBA_; if (CF_TEXTMODE!=3) CBA_ = gpu_unai.CBA;

	// Fast path for 4bpp flat-lit sprites using pre-lit 16-entry CLUT
	if (CF_TEXTMODE == 1 && CF_LIGHT) {
		const u32 * __restrict__ litCLUT = gpu_unai.LitCLUT;
		if (!CF_MASKCHECK && !CF_BLEND) {
			do {
				u32 u_cur = u0 & u0_mask;
				u8 rgb = pTxt[u_cur >> 1];
				u32 p = litCLUT[(rgb >> ((u_cur & 1) << 2)) & 0xf];
				if (p) {
					if (CF_MASKSET) *pDst = (u16)(p | 0x8000);
					else            *pDst = (u16)p;
				}
				u0++;
				pDst++;
			} while (--count);
			return;
		}
		do {
			uDst = *pDst;
			if (CF_MASKCHECK && (uDst & 0x8000)) { goto endsprite_lit4; }
			{
				u32 u_cur = u0 & u0_mask;
				u8 rgb = pTxt[u_cur >> 1];
				u32 p = litCLUT[(rgb >> ((u_cur & 1) << 2)) & 0xf];
				if (!p) goto endsprite_lit4;
				uSrc = (u16)p;
				srcMSB = uSrc & 0x8000;
				if (CF_BLEND && srcMSB)
					uSrc = gpuBlending<CF_BLENDMODE, false>(uSrc, uDst);
				if (CF_MASKSET) *pDst = uSrc | 0x8000;
				else            *pDst = uSrc | srcMSB;
			}
endsprite_lit4:
			u0++;
			pDst++;
		} while (--count);
		return;
	}

	do
	{
		if (CF_MASKCHECK || CF_BLEND) { uDst = *pDst; }
		if (CF_MASKCHECK) if (uDst&0x8000) { goto endsprite; }

		if (CF_TEXTMODE==1) {  //  4bpp (CLUT)
			u8 rgb = pTxt[(u0 & u0_mask)>>1];
			uSrc = CBA_[(rgb>>((u0&1)<<2))&0xf];
		}
		if (CF_TEXTMODE==2) {  //  8bpp (CLUT)
			uSrc = CBA_[pTxt[u0 & u0_mask]];
		}
		if (CF_TEXTMODE==3) {  // 16bpp
			uSrc = *(u16*)(&pTxt[u0 & u0_mask]);
		}

		if (!uSrc) goto endsprite;

		//senquack - save source MSB, as blending or lighting macros will not
		//           (Silent Hill gray rectangles mask bit bug)
		if (CF_BLEND || CF_LIGHT) srcMSB = uSrc & 0x8000;
		
		if (CF_LIGHT)
			uSrc = gpuLightingTXT(uSrc, r5, g5, b5);

		if (CF_BLEND && srcMSB)
			uSrc = gpuBlending<CF_BLENDMODE, skip_uSrc_mask>(uSrc, uDst);

		if (CF_MASKSET)                { *pDst = uSrc | 0x8000; }
		else if (CF_BLEND || CF_LIGHT) { *pDst = uSrc | srcMSB; }
		else                           { *pDst = uSrc;          }

endsprite:
		u0 += (CF_TEXTMODE==3) ? 2 : 1;
		pDst++;
	}
	while (--count);
}

static void SpriteNULL(u16 *pDst, u32 count, u8* pTxt, u32 u0)
{
	#ifdef ENABLE_GPU_LOG_SUPPORT
		fprintf(stdout,"SpriteNULL()\n");
	#endif
}

///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
//  Sprite innerloops driver
typedef void (*PS)(u16 *pDst, u32 count, u8* pTxt, u32 u0);

// Template instantiation helper macros
#define TI(cf) gpuSpriteSpanFn<(cf)>
#define TN     SpriteNULL
#define TIBLOCK(ub) \
	TN,            TN,            TN,            TN,            TN,            TN,            TN,            TN,            \
	TN,            TN,            TN,            TN,            TN,            TN,            TN,            TN,            \
	TN,            TN,            TN,            TN,            TN,            TN,            TN,            TN,            \
	TN,            TN,            TN,            TN,            TN,            TN,            TN,            TN,            \
	TI((ub)|0x20), TI((ub)|0x21), TI((ub)|0x22), TI((ub)|0x23), TI((ub)|0x24), TI((ub)|0x25), TI((ub)|0x26), TI((ub)|0x27), \
	TN,            TN,            TI((ub)|0x2a), TI((ub)|0x2b), TN,            TN,            TI((ub)|0x2e), TI((ub)|0x2f), \
	TN,            TN,            TI((ub)|0x32), TI((ub)|0x33), TN,            TN,            TI((ub)|0x36), TI((ub)|0x37), \
	TN,            TN,            TI((ub)|0x3a), TI((ub)|0x3b), TN,            TN,            TI((ub)|0x3e), TI((ub)|0x3f), \
	TI((ub)|0x40), TI((ub)|0x41), TI((ub)|0x42), TI((ub)|0x43), TI((ub)|0x44), TI((ub)|0x45), TI((ub)|0x46), TI((ub)|0x47), \
	TN,            TN,            TI((ub)|0x4a), TI((ub)|0x4b), TN,            TN,            TI((ub)|0x4e), TI((ub)|0x4f), \
	TN,            TN,            TI((ub)|0x52), TI((ub)|0x53), TN,            TN,            TI((ub)|0x56), TI((ub)|0x57), \
	TN,            TN,            TI((ub)|0x5a), TI((ub)|0x5b), TN,            TN,            TI((ub)|0x5e), TI((ub)|0x5f), \
	TI((ub)|0x60), TI((ub)|0x61), TI((ub)|0x62), TI((ub)|0x63), TI((ub)|0x64), TI((ub)|0x65), TI((ub)|0x66), TI((ub)|0x67), \
	TN,            TN,            TI((ub)|0x6a), TI((ub)|0x6b), TN,            TN,            TI((ub)|0x6e), TI((ub)|0x6f), \
	TN,            TN,            TI((ub)|0x72), TI((ub)|0x73), TN,            TN,            TI((ub)|0x76), TI((ub)|0x77), \
	TN,            TN,            TI((ub)|0x7a), TI((ub)|0x7b), TN,            TN,            TI((ub)|0x7e), TI((ub)|0x7f)

const PS gpuSpriteSpanDrivers[256] = {
	TIBLOCK(0<<8), TIBLOCK(1<<8)
};

#undef TI
#undef TN
#undef TIBLOCK

///////////////////////////////////////////////////////////////////////////////
//  GPU Polygon innerloops generator

//senquack - Newer version with following changes:
//           * Adapted to work with new poly routings in gpu_raster_polygon.h
//             adapted from DrHell GPU. They are less glitchy and use 22.10
//             fixed-point instead of original UNAI's 16.16.
//           * Texture coordinates are no longer packed together into one
//             unsigned int. This seems to lose too much accuracy (they each
//             end up being only 8.7 fixed-point that way) and pixel-droupouts
//             were noticeable both with original code and current DrHell
//             adaptations. An example would be the sky in NFS3. Now, they are
//             stored in separate ints, using separate masks.
//           * Function is no longer INLINE, as it was always called
//             through a function pointer.
//           * Function now ensures the mask bit of source texture is preserved
//             across calls to blending functions (Silent Hill rectangles fix)
//           * November 2016: Large refactoring of blending/lighting when
//             JohnnyF added dithering. See gpu_inner_quantization.h and
//             relevant blend/light headers.
// (see README_senquack.txt)
template<int CF>
static void gpuPolySpanFn(const gpu_unai_t & __restrict__ gpu_unai, u16 * __restrict__ pDst, u32 count)
{
	// Blend func can save an operation if it knows uSrc MSB is unset.
	//  Untextured prims can always skip this (src color MSB is always 0).
	//  For textured prims, lighting funcs always return it unset. (bonus!)
	const bool skip_uSrc_mask = (!CF_TEXTMODE) || CF_LIGHT;

	u32 bMsk; if (CF_BLITMASK) bMsk = gpu_unai.blit_mask;

	if (!CF_TEXTMODE)
	{
		if (!CF_GOURAUD)
		{
			// UNTEXTURED, NO GOURAUD
			const u16 pix15 = gpu_unai.PixelData;
			if (!CF_MASKCHECK && !CF_BLEND) {
				gpuFillSpan16(pDst, count, CF_MASKSET ? (pix15 | 0x8000) : pix15);
				return;
			}
			do {
				u16 uSrc, uDst;

				// NOTE: Don't enable CF_BLITMASK  pixel skipping (speed hack)
				//  on untextured polys. It seems to do more harm than good: see
				//  gravestone text at end of Medieval intro sequence. -senquack
				//if (CF_BLITMASK) { if ((bMsk>>((((uintptr_t)pDst)>>1)&7))&1) { goto endpolynotextnogou; } }

				if (CF_BLEND || CF_MASKCHECK) uDst = *pDst;
				if (CF_MASKCHECK) { if (uDst&0x8000) { goto endpolynotextnogou; } }

				uSrc = pix15;

				if (CF_BLEND)
					uSrc = gpuBlending<CF_BLENDMODE, skip_uSrc_mask>(uSrc, uDst);

				if (CF_MASKSET) { *pDst = uSrc | 0x8000; }
				else            { *pDst = uSrc;          }

endpolynotextnogou:
				pDst++;
			} while(--count);
		}
		else
		{
			// UNTEXTURED, GOURAUD
			u32 l_gCol = gpu_unai.gCol;
			u32 l_gInc = gpu_unai.gInc;

			do {
				u16 uDst, uSrc;

				// See note in above loop regarding CF_BLITMASK
				//if (CF_BLITMASK) { if ((bMsk>>((((uintptr_t)pDst)>>1)&7))&1) goto endpolynotextgou; }

				if (CF_BLEND || CF_MASKCHECK) uDst = *pDst;
				if (CF_MASKCHECK) { if (uDst&0x8000) goto endpolynotextgou; }

				if (CF_DITHER) {
					// GOURAUD, DITHER

					u32 uSrc24 = gpuLightingRGB24(l_gCol);
					if (CF_BLEND)
						uSrc24 = gpuBlending24<CF_BLENDMODE>(uSrc24, uDst);
					uSrc = gpuColorQuantization24<CF_DITHER>(uSrc24, pDst);
				} else {
					// GOURAUD, NO DITHER

					uSrc = gpuLightingRGB(l_gCol);

					if (CF_BLEND)
						uSrc = gpuBlending<CF_BLENDMODE, skip_uSrc_mask>(uSrc, uDst);
				}

				if (CF_MASKSET) { *pDst = uSrc | 0x8000; }
				else            { *pDst = uSrc;          }

endpolynotextgou:
				pDst++;
				l_gCol += l_gInc;
			}
			while (--count);
		}
	}
	else
	{
		// TEXTURED

		u16 uDst, uSrc, srcMSB;

		//senquack - note: original UNAI code had gpu_unai.{u4/v4} packed into
		// one 32-bit unsigned int, but this proved to lose too much accuracy
		// (pixel drouputs noticeable in NFS3 sky), so now are separate vars.
		u32 l_u_msk = gpu_unai.u_msk;     u32 l_v_msk = gpu_unai.v_msk;
		u32 l_u = gpu_unai.u & l_u_msk;   u32 l_v = gpu_unai.v & l_v_msk;
		s32 l_u_inc = gpu_unai.u_inc;     s32 l_v_inc = gpu_unai.v_inc;

		const u16* __restrict__ TBA_ = gpu_unai.TBA;
		const u16* __restrict__ CBA_; if (CF_TEXTMODE!=3) CBA_ = gpu_unai.CBA;

		u8 r5, g5, b5;
		u8 r8, g8, b8;

		u32 l_gInc, l_gCol;

		if (CF_LIGHT) {
			if (CF_GOURAUD) {
				l_gInc = gpu_unai.gInc;
				l_gCol = gpu_unai.gCol;
			} else {
				if (CF_DITHER) {
					r8 = gpu_unai.r8;
					g8 = gpu_unai.g8;
					b8 = gpu_unai.b8;
				} else {
					r5 = gpu_unai.r5;
					g5 = gpu_unai.g5;
					b5 = gpu_unai.b5;
				}
			}
		}

		// ================================================================
		// Fast path 1: 4bpp flat-lit undithered using pre-lit 16-entry CLUT
		// (1.41x verified on JZ4760B, bit-exact including darkened-to-black)
		// ================================================================
		if (CF_TEXTMODE == 1 && CF_LIGHT && !CF_GOURAUD && !CF_DITHER) {
			const u32 * __restrict__ litCLUT = gpu_unai.LitCLUT;
			const u8 * __restrict__ tba8 = (const u8 *)TBA_;

			if (!CF_BLITMASK && !CF_MASKCHECK && !CF_BLEND && ((l_u_msk & l_v_msk) == 0x3ffff)) {
				while (count >= 2) {
					u32 u1 = l_u + l_u_inc;
					u32 v1 = l_v + l_v_inc;
					u8 b0 = tba8[((l_v << 1) & (0xff << 11)) + ((l_u >> 11) & 0x7f)];
					u8 b1 = tba8[((v1  << 1) & (0xff << 11)) + ((u1  >> 11) & 0x7f)];
					u32 p0 = litCLUT[(b0 >> ((l_u >> 8) & 4)) & 0xf];
					u32 p1 = litCLUT[(b1 >> ((u1  >> 8) & 4)) & 0xf];
					if (p0) pDst[0] = CF_MASKSET ? (u16)(p0 | 0x8000) : (u16)p0;
					if (p1) pDst[1] = CF_MASKSET ? (u16)(p1 | 0x8000) : (u16)p1;
					pDst += 2;
					l_u = u1 + l_u_inc;
					l_v = v1 + l_v_inc;
					count -= 2;
				}
				if (count) {
					u8 b0 = tba8[((l_v << 1) & (0xff << 11)) + ((l_u >> 11) & 0x7f)];
					u32 p0 = litCLUT[(b0 >> ((l_u >> 8) & 4)) & 0xf];
					if (p0) *pDst = CF_MASKSET ? (u16)(p0 | 0x8000) : (u16)p0;
				}
				return;
			}

			do {
				if (CF_BLITMASK) { if ((bMsk>>((((uintptr_t)pDst)>>1)&7))&1) goto endpoly_lit4; }
				if (CF_MASKCHECK || CF_BLEND) { uDst = *pDst; }
				if (CF_MASKCHECK) if (uDst&0x8000) { goto endpoly_lit4; }
				{
					u32 tu = (l_u >> 10);
					u32 tv = (l_v << 1) & (0xff << 11);
					u8 rgb = tba8[tv + (tu >> 1)];
					u32 p = litCLUT[(rgb >> ((tu & 1) << 2)) & 0xf];
					if (!p) goto endpoly_lit4;
					uSrc = (u16)p;
					if (CF_BLEND) {
						srcMSB = uSrc & 0x8000;
						if (srcMSB) uSrc = gpuBlending<CF_BLENDMODE, false>(uSrc, uDst);
						if (CF_MASKSET) *pDst = uSrc | 0x8000;
						else            *pDst = uSrc | srcMSB;
					} else {
						if (CF_MASKSET) *pDst = uSrc | 0x8000;
						else            *pDst = uSrc;
					}
				}
endpoly_lit4:
				pDst++;
				l_u = (l_u + l_u_inc) & l_u_msk;
				l_v = (l_v + l_v_inc) & l_v_msk;
			} while (--count);
			return;
		}

		// ================================================================
		// Fast path 2: 4bpp unlit (1x spill-free, heavily used under fast_lighting=1)
		// ================================================================
		if (CF_TEXTMODE == 1 && !CF_LIGHT && !CF_BLITMASK && !CF_MASKCHECK && !CF_BLEND && ((l_u_msk & l_v_msk) == 0x3ffff)) {
			const u8 * __restrict__ tba8 = (const u8 *)TBA_;
			do {
				u8 b0 = tba8[((l_v << 1) & (0xff << 11)) + ((l_u >> 11) & 0x7f)];
				u16 c0 = CBA_[(b0 >> ((l_u >> 8) & 4)) & 0xf];
				if (c0) *pDst = CF_MASKSET ? (c0 | 0x8000) : c0;
				pDst++;
				l_u += l_u_inc;
				l_v += l_v_inc;
			} while (--count);
			return;
		}

		// ================================================================
		// Fast path 3: 4bpp Gouraud-lit undithered (1x spill-free + 1 KB LUT)
		// ================================================================
		if (CF_TEXTMODE == 1 && CF_LIGHT && CF_GOURAUD && !CF_DITHER && !CF_BLITMASK && !CF_MASKCHECK && !CF_BLEND && ((l_u_msk & l_v_msk) == 0x3ffff)) {
			const u8 * __restrict__ tba8 = (const u8 *)TBA_;
			const u8 * __restrict__ lut  = gpu_unai.LightLUT;
			do {
				u8 b0 = tba8[((l_v << 1) & (0xff << 11)) + ((l_u >> 11) & 0x7f)];
				u16 c0 = CBA_[(b0 >> ((l_u >> 8) & 4)) & 0xf];
				if (c0) {
					u16 lc = ((u16)lut[((c0 & 0x7C00) >> 5) | ((l_gCol >>  5) & 0x1F)] << 10) |
					         ((u16)lut[ (c0 & 0x03E0)       | ((l_gCol >> 16) & 0x1F)] <<  5) |
					         ((u16)lut[((c0 & 0x001F) << 5) |  (l_gCol >> 27)        ]      );
					*pDst = CF_MASKSET ? (lc | 0x8000) : (lc | (c0 & 0x8000));
				}
				pDst++;
				l_u += l_u_inc;
				l_v += l_v_inc;
				l_gCol += l_gInc;
			} while (--count);
			return;
		}

		// ================================================================
		// Fast path 4: 8bpp unlit (1x spill-free)
		// ================================================================
		if (CF_TEXTMODE == 2 && !CF_LIGHT && !CF_BLITMASK && !CF_MASKCHECK && !CF_BLEND && ((l_u_msk & l_v_msk) == 0x3ffff)) {
			const u8 * __restrict__ tba8 = (const u8 *)TBA_;
			do {
				u16 c0 = CBA_[tba8[((l_v << 1) & (0xff << 11)) + ((l_u >> 10) & 0xff)]];
				if (c0) *pDst = CF_MASKSET ? (c0 | 0x8000) : c0;
				pDst++;
				l_u += l_u_inc;
				l_v += l_v_inc;
			} while (--count);
			return;
		}

		// ================================================================
		// Fast path 5: 8bpp flat-lit undithered (1x spill-free + pre-offset LUTs)
		// ================================================================
		if (CF_TEXTMODE == 2 && CF_LIGHT && !CF_GOURAUD && !CF_DITHER && !CF_BLITMASK && !CF_MASKCHECK && !CF_BLEND && ((l_u_msk & l_v_msk) == 0x3ffff)) {
			const u8 * __restrict__ tba8 = (const u8 *)TBA_;
			const u8 * __restrict__ lutR = gpu_unai.LightLUT + r5;
			const u8 * __restrict__ lutG = gpu_unai.LightLUT + g5;
			const u8 * __restrict__ lutB = gpu_unai.LightLUT + b5;
			do {
				u16 c0 = CBA_[tba8[((l_v << 1) & (0xff << 11)) + ((l_u >> 10) & 0xff)]];
				if (c0) {
					u16 lc = ((u16)lutB[(c0 & 0x7C00) >> 5] << 10) |
					         ((u16)lutG[ (c0 & 0x03E0)    ] <<  5) |
					         ((u16)lutR[(c0 & 0x001F) << 5]      );
					*pDst = CF_MASKSET ? (lc | 0x8000) : (lc | (c0 & 0x8000));
				}
				pDst++;
				l_u += l_u_inc;
				l_v += l_v_inc;
			} while (--count);
			return;
		}

		// ================================================================
		// Fast path 6: 16bpp unlit (1x spill-free)
		// ================================================================
		if (CF_TEXTMODE == 3 && !CF_LIGHT && !CF_BLITMASK && !CF_MASKCHECK && !CF_BLEND && ((l_u_msk & l_v_msk) == 0x3ffff)) {
			do {
				u16 c0 = TBA_[(l_v & (0xff << 10)) + ((l_u >> 10) & 0xff)];
				if (c0) *pDst = CF_MASKSET ? (c0 | 0x8000) : c0;
				pDst++;
				l_u += l_u_inc;
				l_v += l_v_inc;
			} while (--count);
			return;
		}

		do
		{
			if (CF_BLITMASK) { if ((bMsk>>((((uintptr_t)pDst)>>1)&7))&1) goto endpolytext; }
			if (CF_MASKCHECK || CF_BLEND) { uDst = *pDst; }
			if (CF_MASKCHECK) if (uDst&0x8000) { goto endpolytext; }

			//senquack - adapted to work with new 22.10 fixed point routines:
			//           (UNAI originally used 16.16)
			if (CF_TEXTMODE==1) {  //  4bpp (CLUT)
				u32 tu=(l_u>>10);
				u32 tv=(l_v<<1)&(0xff<<11);
				u8 rgb=((u8*)TBA_)[tv+(tu>>1)];
				uSrc=CBA_[(rgb>>((tu&1)<<2))&0xf];
				if (!uSrc) goto endpolytext;
			}
			if (CF_TEXTMODE==2) {  //  8bpp (CLUT)
				uSrc = CBA_[(((u8*)TBA_)[(l_u>>10)+((l_v<<1)&(0xff<<11))])];
				if (!uSrc) goto endpolytext;
			}
			if (CF_TEXTMODE==3) {  // 16bpp
				uSrc = TBA_[(l_u>>10)+((l_v)&(0xff<<10))];
				if (!uSrc) goto endpolytext;
			}

			// Save source MSB, as blending or lighting will not (Silent Hill)
			if (CF_BLEND || CF_LIGHT) srcMSB = uSrc & 0x8000;

			// When textured, only dither when LIGHT (texture blend) is enabled
			// LIGHT &&  BLEND => dither
			// LIGHT && !BLEND => dither
			//!LIGHT &&  BLEND => no dither
			//!LIGHT && !BLEND => no dither

			if (CF_DITHER && CF_LIGHT) {
				u32 uSrc24;
				if ( CF_GOURAUD)
					uSrc24 = gpuLightingTXT24Gouraud(uSrc, l_gCol);
				if (!CF_GOURAUD)
					uSrc24 = gpuLightingTXT24(uSrc, r8, g8, b8);

				if (CF_BLEND && srcMSB)
					uSrc24 = gpuBlending24<CF_BLENDMODE>(uSrc24, uDst);

				uSrc = gpuColorQuantization24<CF_DITHER>(uSrc24, pDst);
			} else
			{
				if (CF_LIGHT) {
					if ( CF_GOURAUD)
						uSrc = gpuLightingTXTGouraud(uSrc, l_gCol);
					if (!CF_GOURAUD)
						uSrc = gpuLightingTXT(uSrc, r5, g5, b5);
				}

				if (CF_BLEND && srcMSB)
					uSrc = gpuBlending<CF_BLENDMODE, skip_uSrc_mask>(uSrc, uDst);
			}

			if (CF_MASKSET)                { *pDst = uSrc | 0x8000; }
			else if (CF_BLEND || CF_LIGHT) { *pDst = uSrc | srcMSB; }
			else                           { *pDst = uSrc;          }
endpolytext:
			pDst++;
			l_u = (l_u + l_u_inc) & l_u_msk;
			l_v = (l_v + l_v_inc) & l_v_msk;
			if (CF_LIGHT && CF_GOURAUD) l_gCol += l_gInc;
		}
		while (--count);
	}
}

static void PolyNULL(const gpu_unai_t &gpu_unai, u16 *pDst, u32 count)
{
	#ifdef ENABLE_GPU_LOG_SUPPORT
		fprintf(stdout,"PolyNULL()\n");
	#endif
}

///////////////////////////////////////////////////////////////////////////////
//  Specialized 4bpp Monochrome Gouraud & Flat Span Loops (CF = 0xA1 = 161)
//  Uses 1 KB per-CLUT MonoCLUT16 table (zero LightLUT loads, zero spills).
//  Hardware verified 1.41x (opaque) / 1.37x (masked) on JZ4760B, bit-exact.
///////////////////////////////////////////////////////////////////////////////
static void gpuPolySpanMono4Opaque(u16 * __restrict__ pDst, u32 count,
                                   const u8 * __restrict__ tba8,
                                   const u16 * __restrict__ mono,
                                   u32 l_u, u32 l_v, u32 l_c,
                                   s32 l_u_inc, s32 l_v_inc, s32 l_c_inc)
{
	do {
		u8 b0 = tba8[((l_v << 1) & (0xff << 11)) + ((l_u >> 11) & 0x7f)];
		*pDst++ = mono[((l_c >> 9) & 0x1F0) | ((b0 >> ((l_u >> 8) & 4)) & 0xf)];
		l_u += l_u_inc;
		l_v += l_v_inc;
		l_c += l_c_inc;
	} while (--count);
}

static void gpuPolySpanMono4Masked(u16 * __restrict__ pDst, u32 count,
                                   const u8 * __restrict__ tba8,
                                   const u16 * __restrict__ mono,
                                   u32 omask,
                                   u32 l_u, u32 l_v, u32 l_c,
                                   s32 l_u_inc, s32 l_v_inc, s32 l_c_inc)
{
	do {
		u8 b0 = tba8[((l_v << 1) & (0xff << 11)) + ((l_u >> 11) & 0x7f)];
		u32 idx = (b0 >> ((l_u >> 8) & 4)) & 0xf;
		if ((omask >> idx) & 1)
			*pDst = mono[((l_c >> 9) & 0x1F0) | idx];
		pDst++;
		l_u += l_u_inc;
		l_v += l_v_inc;
		l_c += l_c_inc;
	} while (--count);
}

static void gpuPolySpanFlat4Opaque(u16 * __restrict__ pDst, u32 count,
                                   const u8 * __restrict__ tba8,
                                   const u16 * __restrict__ row,
                                   u32 l_u, u32 l_v,
                                   s32 l_u_inc, s32 l_v_inc)
{
	do {
		u8 b0 = tba8[((l_v << 1) & (0xff << 11)) + ((l_u >> 11) & 0x7f)];
		*pDst++ = row[(b0 >> ((l_u >> 8) & 4)) & 0xf];
		l_u += l_u_inc;
		l_v += l_v_inc;
	} while (--count);
}

static void gpuPolySpanFlat4Masked(u16 * __restrict__ pDst, u32 count,
                                   const u8 * __restrict__ tba8,
                                   const u16 * __restrict__ row,
                                   u32 omask,
                                   u32 l_u, u32 l_v,
                                   s32 l_u_inc, s32 l_v_inc)
{
	do {
		u8 b0 = tba8[((l_v << 1) & (0xff << 11)) + ((l_u >> 11) & 0x7f)];
		u32 idx = (b0 >> ((l_u >> 8) & 4)) & 0xf;
		if ((omask >> idx) & 1)
			*pDst = row[idx];
		pDst++;
		l_u += l_u_inc;
		l_v += l_v_inc;
	} while (--count);
}

///////////////////////////////////////////////////////////////////////////////
//  Polygon innerloops driver
typedef void (*PP)(const gpu_unai_t &gpu_unai, u16 *pDst, u32 count);

// Template instantiation helper macros
#define TI(cf) gpuPolySpanFn<(cf)>
#define TN     PolyNULL
#define TIBLOCK(ub) \
	TI((ub)|0x00), TI((ub)|0x01), TI((ub)|0x02), TI((ub)|0x03), TI((ub)|0x04), TI((ub)|0x05), TI((ub)|0x06), TI((ub)|0x07), \
	TN,            TN,            TI((ub)|0x0a), TI((ub)|0x0b), TN,            TN,            TI((ub)|0x0e), TI((ub)|0x0f), \
	TN,            TN,            TI((ub)|0x12), TI((ub)|0x13), TN,            TN,            TI((ub)|0x16), TI((ub)|0x17), \
	TN,            TN,            TI((ub)|0x1a), TI((ub)|0x1b), TN,            TN,            TI((ub)|0x1e), TI((ub)|0x1f), \
	TI((ub)|0x20), TI((ub)|0x21), TI((ub)|0x22), TI((ub)|0x23), TI((ub)|0x24), TI((ub)|0x25), TI((ub)|0x26), TI((ub)|0x27), \
	TN,            TN,            TI((ub)|0x2a), TI((ub)|0x2b), TN,            TN,            TI((ub)|0x2e), TI((ub)|0x2f), \
	TN,            TN,            TI((ub)|0x32), TI((ub)|0x33), TN,            TN,            TI((ub)|0x36), TI((ub)|0x37), \
	TN,            TN,            TI((ub)|0x3a), TI((ub)|0x3b), TN,            TN,            TI((ub)|0x3e), TI((ub)|0x3f), \
	TI((ub)|0x40), TI((ub)|0x41), TI((ub)|0x42), TI((ub)|0x43), TI((ub)|0x44), TI((ub)|0x45), TI((ub)|0x46), TI((ub)|0x47), \
	TN,            TN,            TI((ub)|0x4a), TI((ub)|0x4b), TN,            TN,            TI((ub)|0x4e), TI((ub)|0x4f), \
	TN,            TN,            TI((ub)|0x52), TI((ub)|0x53), TN,            TN,            TI((ub)|0x56), TI((ub)|0x57), \
	TN,            TN,            TI((ub)|0x5a), TI((ub)|0x5b), TN,            TN,            TI((ub)|0x5e), TI((ub)|0x5f), \
	TI((ub)|0x60), TI((ub)|0x61), TI((ub)|0x62), TI((ub)|0x63), TI((ub)|0x64), TI((ub)|0x65), TI((ub)|0x66), TI((ub)|0x67), \
	TN,            TN,            TI((ub)|0x6a), TI((ub)|0x6b), TN,            TN,            TI((ub)|0x6e), TI((ub)|0x6f), \
	TN,            TN,            TI((ub)|0x72), TI((ub)|0x73), TN,            TN,            TI((ub)|0x76), TI((ub)|0x77), \
	TN,            TN,            TI((ub)|0x7a), TI((ub)|0x7b), TN,            TN,            TI((ub)|0x7e), TI((ub)|0x7f), \
	TN,            TI((ub)|0x81), TN,            TI((ub)|0x83), TN,            TI((ub)|0x85), TN,            TI((ub)|0x87), \
	TN,            TN,            TN,            TI((ub)|0x8b), TN,            TN,            TN,            TI((ub)|0x8f), \
	TN,            TN,            TN,            TI((ub)|0x93), TN,            TN,            TN,            TI((ub)|0x97), \
	TN,            TN,            TN,            TI((ub)|0x9b), TN,            TN,            TN,            TI((ub)|0x9f), \
	TN,            TI((ub)|0xa1), TN,            TI((ub)|0xa3), TN,            TI((ub)|0xa5), TN,            TI((ub)|0xa7), \
	TN,            TN,            TN,            TI((ub)|0xab), TN,            TN,            TN,            TI((ub)|0xaf), \
	TN,            TN,            TN,            TI((ub)|0xb3), TN,            TN,            TN,            TI((ub)|0xb7), \
	TN,            TN,            TN,            TI((ub)|0xbb), TN,            TN,            TN,            TI((ub)|0xbf), \
	TN,            TI((ub)|0xc1), TN,            TI((ub)|0xc3), TN,            TI((ub)|0xc5), TN,            TI((ub)|0xc7), \
	TN,            TN,            TN,            TI((ub)|0xcb), TN,            TN,            TN,            TI((ub)|0xcf), \
	TN,            TN,            TN,            TI((ub)|0xd3), TN,            TN,            TN,            TI((ub)|0xd7), \
	TN,            TN,            TN,            TI((ub)|0xdb), TN,            TN,            TN,            TI((ub)|0xdf), \
	TN,            TI((ub)|0xe1), TN,            TI((ub)|0xe3), TN,            TI((ub)|0xe5), TN,            TI((ub)|0xe7), \
	TN,            TN,            TN,            TI((ub)|0xeb), TN,            TN,            TN,            TI((ub)|0xef), \
	TN,            TN,            TN,            TI((ub)|0xf3), TN,            TN,            TN,            TI((ub)|0xf7), \
	TN,            TN,            TN,            TI((ub)|0xfb), TN,            TN,            TN,            TI((ub)|0xff)

const PP gpuPolySpanDrivers[2048] = {
	TIBLOCK(0<<8), TIBLOCK(1<<8), TIBLOCK(2<<8), TIBLOCK(3<<8),
	TIBLOCK(4<<8), TIBLOCK(5<<8), TIBLOCK(6<<8), TIBLOCK(7<<8)
};

#undef TI
#undef TN
#undef TIBLOCK
