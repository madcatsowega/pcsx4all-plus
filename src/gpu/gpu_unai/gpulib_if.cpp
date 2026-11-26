/***************************************************************************
*   Copyright (C) 2010 PCSX4ALL Team                                      *
*   Copyright (C) 2010 Unai                                               *
*   Copyright (C) 2011 notaz                                              *
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

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/gpulib/gpu.h"
#include "port.h"
#include "gpu_unai.h"
#include "aux/pcsx_prof.h"
#ifdef RS97
#include "aux/pcsx_aux.h"
#include "/home/sean/retrofw-aux/firmware/pcsx_hybrid_proto.h"
#endif

#define GPU_INLINE static inline __attribute__((always_inline))

// GPU fixed point math
#include "gpu_fixedpoint.h"

// Inner loop driver instantiation file
#include "gpu_inner.h"

// GPU internal image drawing functions
#include "gpu_raster_image.h"

// GPU internal line drawing functions
#include "gpu_raster_line.h"

// GPU internal polygon drawing functions
#include "gpu_raster_polygon.h"

// GPU internal sprite drawing functions
#include "gpu_raster_sprite.h"

// GPU command buffer execution/store
#include "gpu_command.h"

/////////////////////////////////////////////////////////////////////////////

int renderer_init(void)
{
  memset((void*)&gpu_unai, 0, sizeof(gpu_unai));
  gpu_unai.vram = (u16*)gpu.vram;

  // Original standalone gpu_unai initialized TextureWindow[]. I added the
  //  same behavior here, since it seems unsafe to leave [2],[3] unset when
  //  using HLE and Rearmed gpu_neon sets this similarly on init. -senquack
  gpu_unai.TextureWindow[0] = 0;
  gpu_unai.TextureWindow[1] = 0;
  gpu_unai.TextureWindow[2] = 255;
  gpu_unai.TextureWindow[3] = 255;
  //senquack - new vars must be updated whenever texture window is changed:
  //           (used for polygon-drawing in gpu_inner.h, gpu_raster_polygon.h)
  const u32 fb = FIXED_BITS;  // # of fractional fixed-pt bits of u4/v4
  gpu_unai.u_msk = (((u32)gpu_unai.TextureWindow[2]) << fb) | ((1 << fb) - 1);
  gpu_unai.v_msk = (((u32)gpu_unai.TextureWindow[3]) << fb) | ((1 << fb) - 1);

  // Configuration options
  gpu_unai.config = gpu_unai_config_ext;
  //senquack - disabled, not sure this is needed and would require modifying
  // sprite-span functions, perhaps unnecessarily. No Abe Oddysey hack was
  // present in latest PCSX4ALL sources we were using.
  //gpu_unai.config.enableAbbeyHack = gpu_unai_config_ext.abe_hack;
  gpu_unai.ilace_mask = gpu_unai.config.ilace_force;

#ifdef GPU_UNAI_USE_INT_DIV_MULTINV
  // s_invTable (indexed directly by uDen in gpu_fixedpoint.h: s_invTable[uDen])
  s_invTable[0] = 0x7fffffff;
  for(int i=1;i<(1<<TABLE_BITS);++i)
  {
    double v = 1.0 / double(i);
#ifdef GPU_TABLE_10_BITS
    v *= double(0xffffffff>>1);
#else
    v *= double(0x80000000);
#endif
    s_invTable[i]=s32(v);
  }
#endif

  SetupLightLUT();
  SetupDitheringConstants();

  return 0;
}

void renderer_finish(void)
{
}

void renderer_notify_res_change(void)
{
  if (PixelSkipEnabled()) {
    // Set blit_mask for high horizontal resolutions. This allows skipping
    //  rendering pixels that would never get displayed on low-resolution
    //  platforms that use simple pixel-dropping scaler.

    switch (gpu.screen.hres)
    {
      case 512: gpu_unai.blit_mask = 0xa4; break; // GPU_BlitWWSWWSWS
      case 640: gpu_unai.blit_mask = 0xaa; break; // GPU_BlitWS
      default:  gpu_unai.blit_mask = 0;    break;
    }
  } else {
    gpu_unai.blit_mask = 0;
  }

  if (LineSkipEnabled()) {
    // Set rendering line-skip (only render every other line in high-res
    //  480 vertical mode, or, optionally, force it for all video modes)

    if (gpu.screen.vres == 480) {
      if (gpu_unai.config.ilace_force) {
        gpu_unai.ilace_mask = 3; // Only need 1/4 of lines
      } else {
        gpu_unai.ilace_mask = 1; // Only need 1/2 of lines
      }
    } else {
      // Vert resolution changed from 480 to lower one
      gpu_unai.ilace_mask = gpu_unai.config.ilace_force;
    }
  } else {
    gpu_unai.ilace_mask = 0;
  }

  /*
  printf("res change hres: %d   vres: %d   depth: %d   ilace_mask: %d\n",
      gpu.screen.hres, gpu.screen.vres, gpu.status.rgb24 ? 24 : 15,
      gpu_unai.ilace_mask);
  */
}

// Handles GP0 draw settings commands 0xE1...0xE6
static void gpuGP0Cmd_0xEx(gpu_unai_t &gpu_unai, u32 cmd_word)
{
  // Assume incoming GP0 command is 0xE1..0xE6, convert to 1..6
  u8 num = (cmd_word >> 24) & 7;
  gpu.ex_regs[num] = cmd_word; // Update gpulib register
  switch (num) {
    case 1: {
      // GP0(E1h) - Draw Mode setting (aka "Texpage")
      u32 cur_texpage = gpu_unai.GPU_GP1 & 0x7FF;
      u32 new_texpage = cmd_word & 0x7FF;
      if (cur_texpage != new_texpage) {
        gpu_unai.GPU_GP1 = (gpu_unai.GPU_GP1 & ~0x7FF) | new_texpage;
        gpuSetTexture(gpu_unai.GPU_GP1);
      }
    } break;

    case 2: {
      // GP0(E2h) - Texture Window setting
      if (cmd_word != gpu_unai.TextureWindowCur) {
        static const u8 TextureMask[32] = {
          255, 7, 15, 7, 31, 7, 15, 7, 63, 7, 15, 7, 31, 7, 15, 7,
          127, 7, 15, 7, 31, 7, 15, 7, 63, 7, 15, 7, 31, 7, 15, 7
        };
        gpu_unai.TextureWindowCur = cmd_word;
        gpu_unai.TextureWindow[0] = ((cmd_word >> 10) & 0x1F) << 3;
        gpu_unai.TextureWindow[1] = ((cmd_word >> 15) & 0x1F) << 3;
        gpu_unai.TextureWindow[2] = TextureMask[(cmd_word >> 0) & 0x1F];
        gpu_unai.TextureWindow[3] = TextureMask[(cmd_word >> 5) & 0x1F];
        gpu_unai.TextureWindow[0] &= ~gpu_unai.TextureWindow[2];
        gpu_unai.TextureWindow[1] &= ~gpu_unai.TextureWindow[3];

        // Inner loop vars must be updated whenever texture window is changed:
        const u32 fb = FIXED_BITS;  // # of fractional fixed-pt bits of u4/v4
        gpu_unai.u_msk = (((u32)gpu_unai.TextureWindow[2]) << fb) | ((1 << fb) - 1);
        gpu_unai.v_msk = (((u32)gpu_unai.TextureWindow[3]) << fb) | ((1 << fb) - 1);

        gpuSetTexture(gpu_unai.GPU_GP1);
      }
    } break;

    case 3: {
      // GP0(E3h) - Set Drawing Area top left (X1,Y1)
      gpu_unai.DrawingArea[0] = cmd_word         & 0x3FF;
      gpu_unai.DrawingArea[1] = (cmd_word >> 10) & 0x3FF;
    } break;

    case 4: {
      // GP0(E4h) - Set Drawing Area bottom right (X2,Y2)
      gpu_unai.DrawingArea[2] = (cmd_word         & 0x3FF) + 1;
      gpu_unai.DrawingArea[3] = ((cmd_word >> 10) & 0x3FF) + 1;
    } break;

    case 5: {
      // GP0(E5h) - Set Drawing Offset (X,Y)
      gpu_unai.DrawingOffset[0] = ((s32)cmd_word<<(32-11))>>(32-11);
      gpu_unai.DrawingOffset[1] = ((s32)cmd_word<<(32-22))>>(32-11);
    } break;

    case 6: {
      // GP0(E6h) - Mask Bit Setting
      gpu_unai.Masking  = (cmd_word & 0x2) <<  1;
      gpu_unai.PixelMSB = (cmd_word & 0x1) <<  8;
    } break;
  }
}

extern const unsigned char cmd_lengths[256];

static int do_cmd_list_sw(unsigned int *list, int list_len, int *last_cmd)
{
  unsigned int cmd = 0, len, i;
  unsigned int *list_start = list;
  unsigned int *list_end = list + list_len;

  // Invalidate cached pre-lit monochrome CLUT rows on each DMA batch entry
  // so any prior GP0(0xA0) VRAM CLUT upload is guaranteed coherent.
  gpuInvalidateMonoCLUTCache();

  //TODO: set ilace_mask when resolution changes instead of every time,
  // eliminate #ifdef below.
  gpu_unai.ilace_mask = gpu_unai.config.ilace_force;

#ifdef HAVE_PRE_ARMV7 /* XXX */
  gpu_unai.ilace_mask |= gpu.status.interlace;
#endif

  for (; list < list_end; list += 1 + len)
  {
    cmd = *list >> 24;
    len = cmd_lengths[cmd];
    if (list + 1 + len > list_end) {
      cmd = -1;
      break;
    }

    #define PRIM cmd
    // Zero-copy packet view into command list for read-only commands
    // (polygons, single lines, variable tiles/sprites, clears, moves, 0xEx).
    // Mutating commands (line strips & fixed-size tiles/sprites) copy into
    // gpu_unai.PacketBuffer below.
    PtrUnion packet = { .ptr = (void*)list };

    if (__builtin_expect(g_pcsx_prof.enabled, 0)) {
      if (cmd >= 0x20 && cmd <= 0x3F) {
        if (cmd & 2) g_pcsx_prof.prim_blend++;
        if (cmd & 0x10) {
          if (cmd & 4) g_pcsx_prof.prim_gt++; else g_pcsx_prof.prim_g++;
        } else {
          if (cmd & 4) g_pcsx_prof.prim_ft++; else g_pcsx_prof.prim_f++;
        }
        if (cmd & 4) {
          u32 tm = (cmd & 0x10) ? ((packet.U4[5] >> 23) & 3) : ((packet.U4[4] >> 23) & 3);
          if (tm == 0) g_pcsx_prof.prim_tex4++;
          else if (tm == 1) g_pcsx_prof.prim_tex8++;
          else g_pcsx_prof.prim_tex16++;
        }
      } else if (cmd >= 0x60 && cmd <= 0x7F) {
        if (cmd & 2) g_pcsx_prof.prim_blend++;
        if (cmd & 4) g_pcsx_prof.prim_spr++; else g_pcsx_prof.prim_tile++;
      }
    }

    switch (cmd)
    {
      case 0x02:
        gpuClearImage(packet);
        break;

      case 0x20:
      case 0x21:
      case 0x22:
      case 0x23: {          // Monochrome 3-pt poly
        PP driver = gpuPolySpanDrivers[
          (gpu_unai.blit_mask?1024:0) |
          Blending_Mode |
          gpu_unai.Masking | Blending | gpu_unai.PixelMSB
        ];
        gpuDrawPolyF(packet, driver, false);
      } break;

      case 0x24:
      case 0x25:
      case 0x26:
      case 0x27: {          // Textured 3-pt poly
        gpuSetCLUT   (packet.U4[2] >> 16);
        gpuSetTexture(packet.U4[4] >> 16);

        u32 driver_idx =
          (gpu_unai.blit_mask?1024:0) |
          Dithering |
          Blending_Mode | gpu_unai.TEXT_MODE |
          gpu_unai.Masking | Blending | gpu_unai.PixelMSB;

        if (!FastLightingEnabled()) {
          driver_idx |= Lighting;
        } else {
          if (!((packet.U1[0]>0x5F) && (packet.U1[1]>0x5F) && (packet.U1[2]>0x5F)))
            driver_idx |= Lighting;
        }

        PP driver = gpuPolySpanDrivers[driver_idx];
        gpuDrawPolyFT(packet, driver, false);
      } break;

      case 0x28:
      case 0x29:
      case 0x2A:
      case 0x2B: {          // Monochrome 4-pt poly
        PP driver = gpuPolySpanDrivers[
          (gpu_unai.blit_mask?1024:0) |
          Blending_Mode |
          gpu_unai.Masking | Blending | gpu_unai.PixelMSB
        ];
        gpuDrawPolyF(packet, driver, true); // is_quad = true
      } break;

      case 0x2C:
      case 0x2D:
      case 0x2E:
      case 0x2F: {          // Textured 4-pt poly
        gpuSetCLUT   (packet.U4[2] >> 16);
        gpuSetTexture(packet.U4[4] >> 16);

        u32 driver_idx =
          (gpu_unai.blit_mask?1024:0) |
          Dithering |
          Blending_Mode | gpu_unai.TEXT_MODE |
          gpu_unai.Masking | Blending | gpu_unai.PixelMSB;

        if (!FastLightingEnabled()) {
          driver_idx |= Lighting;
        } else {
          if (!((packet.U1[0]>0x5F) && (packet.U1[1]>0x5F) && (packet.U1[2]>0x5F)))
            driver_idx |= Lighting;
        }

        PP driver = gpuPolySpanDrivers[driver_idx];
        gpuDrawPolyFT(packet, driver, true); // is_quad = true
      } break;

      case 0x30:
      case 0x31:
      case 0x32:
      case 0x33: {          // Gouraud-shaded 3-pt poly
        u32 c0 = packet.U4[0], c1 = packet.U4[2], c2 = packet.U4[4];
        if (!Dithering && (((c0 ^ c1) | (c0 ^ c2)) & 0xF8F8F8u) == 0) {
          u32 fp[4] = { c0, packet.U4[1], packet.U4[3], packet.U4[5] };
          PtrUnion fpu = { .ptr = fp };
          PP fdrv = gpuPolySpanDrivers[
            (gpu_unai.blit_mask?1024:0) |
            Blending_Mode |
            gpu_unai.Masking | Blending | gpu_unai.PixelMSB
          ];
          gpuDrawPolyF(fpu, fdrv, false);
          break;
        }
        //NOTE: The '129' here is CF_GOURAUD | CF_LIGHT, however
        // this is an untextured poly, so CF_LIGHT (texture blend)
        // shouldn't apply. Until the original array of template
        // instantiation ptrs is fixed, we're stuck with this. (TODO)
        PP driver = gpuPolySpanDrivers[
          (gpu_unai.blit_mask?1024:0) |
          Dithering |
          Blending_Mode |
          gpu_unai.Masking | Blending | 129 | gpu_unai.PixelMSB
        ];
        gpuDrawPolyG(packet, driver, false);
      } break;

      case 0x34:
      case 0x35:
      case 0x36:
      case 0x37: {          // Gouraud-shaded, textured 3-pt poly
        gpuSetCLUT    (packet.U4[2] >> 16);
        gpuSetTexture (packet.U4[5] >> 16);
        u32 base_idx =
          (gpu_unai.blit_mask?1024:0) |
          Dithering |
          Blending_Mode | gpu_unai.TEXT_MODE |
          gpu_unai.Masking | Blending | gpu_unai.PixelMSB;
        u32 c0 = packet.U4[0], c1 = packet.U4[3], c2 = packet.U4[6];
        bool drop_light = !Lighting;
        if (!drop_light && FastLightingEnabled()) {
          if ((c0 & 0xff) > 0x5F && ((c0 >> 8) & 0xff) > 0x5F && ((c0 >> 16) & 0xff) > 0x5F &&
              (c1 & 0xff) > 0x5F && ((c1 >> 8) & 0xff) > 0x5F && ((c1 >> 16) & 0xff) > 0x5F &&
              (c2 & 0xff) > 0x5F && ((c2 >> 8) & 0xff) > 0x5F && ((c2 >> 16) & 0xff) > 0x5F)
            drop_light = true;
        }
        if (!drop_light && !Dithering &&
            (c0 & 0xF8F8F8u) == 0x808080u &&
            (c1 & 0xF8F8F8u) == 0x808080u &&
            (c2 & 0xF8F8F8u) == 0x808080u) {
          drop_light = true;
        }
        if (drop_light) {
          u32 fp[7] = { c0 | 0x01000000u, packet.U4[1], packet.U4[2],
                        packet.U4[4], packet.U4[5], packet.U4[7], packet.U4[8] };
          PtrUnion fpu = { .ptr = fp };
          if (__builtin_expect(g_pcsx_prof.enabled, 0)) g_pcsx_prof.prim_gt_promo++;
          gpuDrawPolyFT(fpu, gpuPolySpanDrivers[base_idx], false);
          break;
        }
        u32 cmask = Dithering ? 0xFFFFFFu : 0xF8F8F8u;
        if ((((c0 ^ c1) | (c0 ^ c2)) & cmask) == 0) {
          u32 fp[7] = { c0, packet.U4[1], packet.U4[2],
                        packet.U4[4], packet.U4[5], packet.U4[7], packet.U4[8] };
          PtrUnion fpu = { .ptr = fp };
          if (__builtin_expect(g_pcsx_prof.enabled, 0)) g_pcsx_prof.prim_gt_promo++;
          gpuDrawPolyFT(fpu, gpuPolySpanDrivers[base_idx | 1], false);
          break;
        }
        PP driver = gpuPolySpanDrivers[base_idx | 129];
        gpuDrawPolyGT(packet, driver, false);
      } break;

      case 0x38:
      case 0x39:
      case 0x3A:
      case 0x3B: {          // Gouraud-shaded 4-pt poly
        u32 c0 = packet.U4[0], c1 = packet.U4[2], c2 = packet.U4[4], c3 = packet.U4[6];
        if (!Dithering && (((c0 ^ c1) | (c0 ^ c2) | (c0 ^ c3)) & 0xF8F8F8u) == 0) {
          u32 fp[5] = { c0, packet.U4[1], packet.U4[3], packet.U4[5], packet.U4[7] };
          PtrUnion fpu = { .ptr = fp };
          PP fdrv = gpuPolySpanDrivers[
            (gpu_unai.blit_mask?1024:0) |
            Blending_Mode |
            gpu_unai.Masking | Blending | gpu_unai.PixelMSB
          ];
          gpuDrawPolyF(fpu, fdrv, true);
          break;
        }
        // See notes regarding '129' for 0x30..0x33 further above -senquack
        PP driver = gpuPolySpanDrivers[
          (gpu_unai.blit_mask?1024:0) |
          Dithering |
          Blending_Mode |
          gpu_unai.Masking | Blending | 129 | gpu_unai.PixelMSB
        ];
        gpuDrawPolyG(packet, driver, true); // is_quad = true
      } break;

      case 0x3C:
      case 0x3D:
      case 0x3E:
      case 0x3F: {          // Gouraud-shaded, textured 4-pt poly
        gpuSetCLUT    (packet.U4[2] >> 16);
        gpuSetTexture (packet.U4[5] >> 16);
        u32 base_idx =
          (gpu_unai.blit_mask?1024:0) |
          Dithering |
          Blending_Mode | gpu_unai.TEXT_MODE |
          gpu_unai.Masking | Blending | gpu_unai.PixelMSB;
        u32 c0 = packet.U4[0], c1 = packet.U4[3], c2 = packet.U4[6], c3 = packet.U4[9];
        bool drop_light = !Lighting;
        if (!drop_light && FastLightingEnabled()) {
          if ((c0 & 0xff) > 0x5F && ((c0 >> 8) & 0xff) > 0x5F && ((c0 >> 16) & 0xff) > 0x5F &&
              (c1 & 0xff) > 0x5F && ((c1 >> 8) & 0xff) > 0x5F && ((c1 >> 16) & 0xff) > 0x5F &&
              (c2 & 0xff) > 0x5F && ((c2 >> 8) & 0xff) > 0x5F && ((c2 >> 16) & 0xff) > 0x5F &&
              (c3 & 0xff) > 0x5F && ((c3 >> 8) & 0xff) > 0x5F && ((c3 >> 16) & 0xff) > 0x5F)
            drop_light = true;
        }
        if (!drop_light && !Dithering &&
            (c0 & 0xF8F8F8u) == 0x808080u &&
            (c1 & 0xF8F8F8u) == 0x808080u &&
            (c2 & 0xF8F8F8u) == 0x808080u &&
            (c3 & 0xF8F8F8u) == 0x808080u) {
          drop_light = true;
        }
        if (drop_light) {
          u32 fp[9] = { c0 | 0x01000000u, packet.U4[1], packet.U4[2],
                        packet.U4[4], packet.U4[5], packet.U4[7], packet.U4[8],
                        packet.U4[10], packet.U4[11] };
          PtrUnion fpu = { .ptr = fp };
          if (__builtin_expect(g_pcsx_prof.enabled, 0)) g_pcsx_prof.prim_gt_promo++;
          gpuDrawPolyFT(fpu, gpuPolySpanDrivers[base_idx], true);
          break;
        }
        u32 cmask = Dithering ? 0xFFFFFFu : 0xF8F8F8u;
        if ((((c0 ^ c1) | (c0 ^ c2) | (c0 ^ c3)) & cmask) == 0) {
          u32 fp[9] = { c0, packet.U4[1], packet.U4[2],
                        packet.U4[4], packet.U4[5], packet.U4[7], packet.U4[8],
                        packet.U4[10], packet.U4[11] };
          PtrUnion fpu = { .ptr = fp };
          if (__builtin_expect(g_pcsx_prof.enabled, 0)) g_pcsx_prof.prim_gt_promo++;
          gpuDrawPolyFT(fpu, gpuPolySpanDrivers[base_idx | 1], true);
          break;
        }
        PP driver = gpuPolySpanDrivers[base_idx | 129];
        gpuDrawPolyGT(packet, driver, true); // is_quad = true
      } break;

      case 0x40:
      case 0x41:
      case 0x42:
      case 0x43: {          // Monochrome line
        // Shift index right by one, as untextured prims don't use lighting
        u32 driver_idx = (Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1;
        PSD driver = gpuPixelSpanDrivers[driver_idx];
        gpuDrawLineF(packet, driver);
      } break;

      case 0x48 ... 0x4F: { // Monochrome line strip
        for (i = 0; i <= len; i++) gpu_unai.PacketBuffer.U4[i] = list[i];
        packet.ptr = (void*)&gpu_unai.PacketBuffer;
        u32 num_vertexes = 1;
        u32 *list_position = &(list[2]);

        // Shift index right by one, as untextured prims don't use lighting
        u32 driver_idx = (Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1;
        PSD driver = gpuPixelSpanDrivers[driver_idx];
        gpuDrawLineF(packet, driver);

        while(1)
        {
          gpu_unai.PacketBuffer.U4[1] = gpu_unai.PacketBuffer.U4[2];
          gpu_unai.PacketBuffer.U4[2] = *list_position++;
          gpuDrawLineF(packet, driver);

          num_vertexes++;
          if(list_position >= list_end) {
            cmd = -1;
            goto breakloop;
          }
          if((*list_position & 0xf000f000) == 0x50005000)
            break;
        }

        len += (num_vertexes - 2);
      } break;

      case 0x50:
      case 0x51:
      case 0x52:
      case 0x53: {          // Gouraud-shaded line
        // Shift index right by one, as untextured prims don't use lighting
        u32 driver_idx = (Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1;
        // Index MSB selects Gouraud-shaded PixelSpanDriver:
        driver_idx |= (1 << 5);
        PSD driver = gpuPixelSpanDrivers[driver_idx];
        gpuDrawLineG(packet, driver);
      } break;

      case 0x58 ... 0x5F: { // Gouraud-shaded line strip
        for (i = 0; i <= len; i++) gpu_unai.PacketBuffer.U4[i] = list[i];
        packet.ptr = (void*)&gpu_unai.PacketBuffer;
        u32 num_vertexes = 1;
        u32 *list_position = &(list[2]);

        // Shift index right by one, as untextured prims don't use lighting
        u32 driver_idx = (Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1;
        // Index MSB selects Gouraud-shaded PixelSpanDriver:
        driver_idx |= (1 << 5);
        PSD driver = gpuPixelSpanDrivers[driver_idx];
        gpuDrawLineG(packet, driver);

        while(1)
        {
          gpu_unai.PacketBuffer.U4[0] = gpu_unai.PacketBuffer.U4[2];
          gpu_unai.PacketBuffer.U4[1] = gpu_unai.PacketBuffer.U4[3];
          gpu_unai.PacketBuffer.U4[2] = *list_position++;
          gpu_unai.PacketBuffer.U4[3] = *list_position++;
          gpuDrawLineG(packet, driver);

          num_vertexes++;
          if(list_position >= list_end) {
            cmd = -1;
            goto breakloop;
          }
          if((*list_position & 0xf000f000) == 0x50005000)
            break;
        }

        len += (num_vertexes - 2) * 2;
      } break;

      case 0x60:
      case 0x61:
      case 0x62:
      case 0x63: {          // Monochrome rectangle (variable size)
        PT driver = gpuTileSpanDrivers[(Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1];
        gpuDrawT(packet, driver);
      } break;

      case 0x64:
      case 0x65:
      case 0x66:
      case 0x67: {          // Textured rectangle (variable size)
        gpuSetCLUT    (packet.U4[2] >> 16);
        u32 driver_idx = Blending_Mode | gpu_unai.TEXT_MODE | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>1);

        // Strip lower 3 bits of each color and determine if lighting should be used:
        if ((packet.U4[0] & 0xF8F8F8) != 0x808080)
          driver_idx |= Lighting;
        PS driver = gpuSpriteSpanDrivers[driver_idx];
        gpuDrawS(packet, driver);
      } break;

      case 0x68:
      case 0x69:
      case 0x6A:
      case 0x6B: {          // Monochrome rectangle (1x1 dot)
        gpu_unai.PacketBuffer.U4[0] = list[0];
        gpu_unai.PacketBuffer.U4[1] = list[1];
        gpu_unai.PacketBuffer.U4[2] = 0x00010001;
        packet.ptr = (void*)&gpu_unai.PacketBuffer;
        PT driver = gpuTileSpanDrivers[(Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1];
        gpuDrawT(packet, driver);
      } break;

      case 0x70:
      case 0x71:
      case 0x72:
      case 0x73: {          // Monochrome rectangle (8x8)
        gpu_unai.PacketBuffer.U4[0] = list[0];
        gpu_unai.PacketBuffer.U4[1] = list[1];
        gpu_unai.PacketBuffer.U4[2] = 0x00080008;
        packet.ptr = (void*)&gpu_unai.PacketBuffer;
        PT driver = gpuTileSpanDrivers[(Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1];
        gpuDrawT(packet, driver);
      } break;

      case 0x74:
      case 0x75:
      case 0x76:
      case 0x77: {          // Textured rectangle (8x8)
        gpu_unai.PacketBuffer.U4[0] = list[0];
        gpu_unai.PacketBuffer.U4[1] = list[1];
        gpu_unai.PacketBuffer.U4[2] = list[2];
        gpu_unai.PacketBuffer.U4[3] = 0x00080008;
        packet.ptr = (void*)&gpu_unai.PacketBuffer;
        gpuSetCLUT    (gpu_unai.PacketBuffer.U4[2] >> 16);
        u32 driver_idx = Blending_Mode | gpu_unai.TEXT_MODE | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>1);

        // Strip lower 3 bits of each color and determine if lighting should be used:
        if ((gpu_unai.PacketBuffer.U4[0] & 0xF8F8F8) != 0x808080)
          driver_idx |= Lighting;
        PS driver = gpuSpriteSpanDrivers[driver_idx];
        gpuDrawS(packet, driver);
      } break;

      case 0x78:
      case 0x79:
      case 0x7A:
      case 0x7B: {          // Monochrome rectangle (16x16)
        gpu_unai.PacketBuffer.U4[0] = list[0];
        gpu_unai.PacketBuffer.U4[1] = list[1];
        gpu_unai.PacketBuffer.U4[2] = 0x00100010;
        packet.ptr = (void*)&gpu_unai.PacketBuffer;
        PT driver = gpuTileSpanDrivers[(Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1];
        gpuDrawT(packet, driver);
      } break;

      case 0x7C:
      case 0x7D:
#ifdef __arm__
        if ((gpu_unai.GPU_GP1 & 0x180) == 0 && (gpu_unai.Masking | gpu_unai.PixelMSB) == 0)
        {
          gpuSetCLUT    (packet.U4[2] >> 16);
          gpuDrawS16(packet);
          break;
        }
        // fallthrough
#endif
      case 0x7E:
      case 0x7F: {          // Textured rectangle (16x16)
        gpu_unai.PacketBuffer.U4[0] = list[0];
        gpu_unai.PacketBuffer.U4[1] = list[1];
        gpu_unai.PacketBuffer.U4[2] = list[2];
        gpu_unai.PacketBuffer.U4[3] = 0x00100010;
        packet.ptr = (void*)&gpu_unai.PacketBuffer;
        gpuSetCLUT    (gpu_unai.PacketBuffer.U4[2] >> 16);
        u32 driver_idx = Blending_Mode | gpu_unai.TEXT_MODE | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>1);
        // Strip lower 3 bits of each color and determine if lighting should be used:
        if ((gpu_unai.PacketBuffer.U4[0] & 0xF8F8F8) != 0x808080)
          driver_idx |= Lighting;
        PS driver = gpuSpriteSpanDrivers[driver_idx];
        gpuDrawS(packet, driver);
      } break;

      case 0x80:          //  vid -> vid
        gpuMoveImage(packet);
        break;

#ifdef TEST
      case 0xA0:          //  sys -> vid
      {
        u32 load_width = list[2] & 0xffff;
        u32 load_height = list[2] >> 16;
        u32 load_size = load_width * load_height;

        len += load_size / 2;
      } break;

      case 0xC0:
        break;
#else
      case 0xA0:          //  sys ->vid
      case 0xC0:          //  vid -> sys
        // Handled by gpulib
        goto breakloop;
#endif
      case 0xE1 ... 0xE6: { // Draw settings
        gpuGP0Cmd_0xEx(gpu_unai, packet.U4[0]);
      } break;
    }
  }

breakloop:
  gpu.ex_regs[1] &= ~0x1ff;
  gpu.ex_regs[1] |= gpu_unai.GPU_GP1 & 0x1ff;

  *last_cmd = cmd;
  return list - list_start;
}

#ifdef RS97
/* ============================================================================
 * Half-Screen AUX GPU Split Dispatcher (Core 0 Top Band || Core 1 Bottom Band)
 * ============================================================================ */

static inline int aux_build_dl_batch(unsigned int *list, int list_len,
                                     int y_split, int band_y1,
                                     uint32_t *dl, uint32_t dl_cap,
                                     uint32_t *out_dl_words, uint32_t *out_aux_prims)
{
  unsigned int *cur = list;
  unsigned int *end = list + list_len;
  uint32_t dl_w = 0;
  uint32_t aux_prims = 0;

  /* Shadow GP0 state so we compute exact Y-bounds & flags without touching gpu_unai */
  s32 off_y = gpu_unai.DrawingOffset[1];
  u32 gp1   = gpu_unai.GPU_GP1;
  u8  masking = gpu_unai.Masking ? 1 : 0;
  u8  msb     = gpu_unai.PixelMSB ? 1 : 0;
  u8  has_bmask = gpu_unai.blit_mask ? 1 : 0;
  u8  do_blend_en = BlendingEnabled() ? 1 : 0;
  u8  do_light_en = LightingEnabled() ? 1 : 0;
  u8  do_fast_lt  = FastLightingEnabled() ? 1 : 0;

  while (cur < end) {
    u32 w0 = *cur;
    u32 cmd = w0 >> 24;
    u32 len = cmd_lengths[cmd];
    if (cur + 1 + len > end)
      break;

    /* Break batch before VRAM I/O, MoveImage, ClearImage, or DrawArea changes */
    if (cmd == 0x02 || cmd == 0x80 || cmd == 0xA0 || cmd == 0xC0 || cmd == 0xE3 || cmd == 0xE4)
      break;

    if (cmd >= 0xE1 && cmd <= 0xE6) {
      if (dl_w + 2U >= dl_cap) break;
      dl[dl_w++] = w0;
      if (cmd == 0xE1) {
        gp1 = (gp1 & ~0x7FFU) | (w0 & 0x7FFU);
      } else if (cmd == 0xE5) {
        off_y = GPU_EXPANDSIGN(w0 >> 11);
      } else if (cmd == 0xE6) {
        masking = (w0 & 2U) ? 1 : 0;
        msb     = (w0 & 1U) ? 1 : 0;
      }
      cur += 1 + len;
      continue;
    }

    if (cmd >= 0x20 && cmd <= 0x3F) {
      u32 is_tex  = (cmd & 0x04U) ? 1U : 0U;
      u32 is_gour = (cmd & 0x10U) ? 1U : 0U;
      u32 is_quad = (cmd & 0x08U) ? 1U : 0U;
      u32 nv = is_quad ? 4U : 3U;
      u32 vstride = 1U + is_tex + is_gour;

      s32 ymin = 10000, ymax = -10000;
      for (u32 i = 0; i < nv; i++) {
        s32 vy = GPU_EXPANDSIGN(cur[1 + i * vstride] >> 16) + off_y;
        if (vy < ymin) ymin = vy;
        if (vy > ymax) ymax = vy;
      }
      if (is_tex) {
        u32 tp = cur[is_gour ? 5 : 4] >> 16;
        u32 new_gp1 = (gp1 & ~0x1FFU) | (tp & 0x1FFU);
        if (new_gp1 != gp1) {
          gp1 = new_gp1;
          /* Keep AUX tpage in sync across skipped top-half polys & per-band skips! */
          if (dl_w + 2U < dl_cap)
            dl[dl_w++] = 0xE1000000U | (gp1 & 0x7FFU);
        }
      }

      /* Strict Y-band filter: only queue to AUX if polygon touches [y_split .. band_y1) */
      if (ymax > y_split && ymin < band_y1 && (ymax - ymin) < CHKMAX_Y) {
        u32 nwords = 1U + len;
        if (dl_w + 2U + nwords + 2U >= dl_cap) break;

        u32 pf = 0;
        if ((cmd & 2U) && do_blend_en) {
          pf |= AUX_GPU_PF_BLEND | (((gp1 >> 5) & 3U) << AUX_GPU_PF_BMODE_SH);
        }
        if (is_quad)   pf |= AUX_GPU_PF_QUAD;
        if (has_bmask) pf |= AUX_GPU_PF_BLITMASK;
        if (masking)   pf |= AUX_GPU_PF_MASKING;
        if (msb)       pf |= AUX_GPU_PF_PIXELMSB;

        u32 out_cmd = cmd;
        u32 promo_pkt[12];
        const u32 *src_pkt = cur;

        if (is_tex && !is_gour) {
          /* Flat textured (FT) */
          if (!(cmd & 1U) && do_light_en) {
            if (!do_fast_lt || !(((cur[0] & 0xFFU) > 0x5FU) && (((cur[0] >> 8) & 0xFFU) > 0x5FU) && (((cur[0] >> 16) & 0xFFU) > 0x5FU)))
              pf |= AUX_GPU_PF_LIGHT;
          }
        } else if (!is_tex && is_gour) {
          /* G -> F promotion when all vertex colors match */
          u32 c0 = cur[0], c1 = cur[2], c2 = cur[4];
          u32 diff = (c0 ^ c1) | (c0 ^ c2);
          if (is_quad) diff |= (c0 ^ cur[6]);
          if ((diff & 0xF8F8F8U) == 0) {
            out_cmd = cmd & ~0x10U;
            promo_pkt[0] = c0;
            promo_pkt[1] = cur[1];
            promo_pkt[2] = cur[3];
            promo_pkt[3] = cur[5];
            if (is_quad) { promo_pkt[4] = cur[7]; nwords = 5; } else { nwords = 4; }
            src_pkt = promo_pkt;
          }
        } else if (is_tex && is_gour) {
          /* GT -> FT promotion (exact same rules as Main CPU gpulib_if.cpp!) */
          u32 c0 = cur[0], c1 = cur[3], c2 = cur[6], c3 = is_quad ? cur[9] : c0;
          bool drop_light = !((!(cmd & 1U)) && do_light_en);
          if (!drop_light && do_fast_lt) {
            if ((c0 & 0xff) > 0x5F && ((c0 >> 8) & 0xff) > 0x5F && ((c0 >> 16) & 0xff) > 0x5F &&
                (c1 & 0xff) > 0x5F && ((c1 >> 8) & 0xff) > 0x5F && ((c1 >> 16) & 0xff) > 0x5F &&
                (c2 & 0xff) > 0x5F && ((c2 >> 8) & 0xff) > 0x5F && ((c2 >> 16) & 0xff) > 0x5F &&
                (c3 & 0xff) > 0x5F && ((c3 >> 8) & 0xff) > 0x5F && ((c3 >> 16) & 0xff) > 0x5F)
              drop_light = true;
          }
          if (!drop_light && (c0 & 0xF8F8F8U) == 0x808080U &&
              (c1 & 0xF8F8F8U) == 0x808080U && (c2 & 0xF8F8F8U) == 0x808080U &&
              (c3 & 0xF8F8F8U) == 0x808080U)
            drop_light = true;

          if (drop_light || ((((c0 ^ c1) | (c0 ^ c2) | (c0 ^ c3)) & 0xF8F8F8U) == 0)) {
            out_cmd = (cmd & ~0x10U) | (drop_light ? 1U : 0U);
            if (!drop_light) pf |= AUX_GPU_PF_LIGHT;
            promo_pkt[0] = drop_light ? (c0 | 0x01000000U) : c0;
            promo_pkt[1] = cur[1]; promo_pkt[2] = cur[2];
            promo_pkt[3] = cur[4]; promo_pkt[4] = cur[5];
            promo_pkt[5] = cur[7]; promo_pkt[6] = cur[8];
            if (is_quad) { promo_pkt[7] = cur[10]; promo_pkt[8] = cur[11]; nwords = 9; } else { nwords = 7; }
            src_pkt = promo_pkt;
          }
        }

        if (ymin < 0) ymin = 0;
        if (ymax > 511) ymax = 511;
        dl[dl_w++] = 0x80000000U | (nwords << 16) | (pf << 8) | out_cmd;
        dl[dl_w++] = ((u32)ymin << 16) | (u32)ymax;
        for (u32 k = 0; k < nwords; k++)
          dl[dl_w++] = src_pkt[k];
        aux_prims++;
      }
      cur += 1 + len;
      continue;
    }

    if (cmd >= 0x60 && cmd <= 0x7F) {
      u32 is_spr = (cmd & 4U) ? 1U : 0U;
      u32 sz_type = (cmd >> 3) & 3U; /* 0=var, 1=1x1, 2=8x8, 3=16x16 */
      u32 wh_word;
      if (sz_type == 0)      wh_word = cur[is_spr ? 3 : 2];
      else if (sz_type == 1) wh_word = 0x00010001U;
      else if (sz_type == 2) wh_word = 0x00080008U;
      else                   wh_word = 0x00100010U;

      s32 y0 = GPU_EXPANDSIGN((s16)(cur[1] >> 16) + off_y);
      s32 h  = (s32)((wh_word >> 16) & 0x1FFU);
      s32 y1 = y0 + h;

      if (y1 > y_split && y0 < band_y1 && h > 0) {
        u32 nwords = is_spr ? 4U : 3U;
        if (dl_w + 2U + nwords + 2U >= dl_cap) break;

        u32 pf = 0;
        if ((cmd & 2U) && do_blend_en)
          pf |= AUX_GPU_PF_BLEND | (((gp1 >> 5) & 3U) << AUX_GPU_PF_BMODE_SH);
        if (masking) pf |= AUX_GPU_PF_MASKING;
        if (msb)     pf |= AUX_GPU_PF_PIXELMSB;
        if (is_spr && !(cmd & 1U) && do_light_en && ((cur[0] & 0xF8F8F8U) != 0x808080U))
          pf |= AUX_GPU_PF_LIGHT;

        s32 ymin = (y0 < 0) ? 0 : y0;
        s32 ymax = (y1 > 511) ? 511 : y1;
        u32 out_cmd = is_spr ? (0x64U | (cmd & 3U)) : (0x60U | (cmd & 3U));
        dl[dl_w++] = 0x80000000U | (nwords << 16) | (pf << 8) | out_cmd;
        dl[dl_w++] = ((u32)ymin << 16) | (u32)ymax;
        dl[dl_w++] = cur[0];
        dl[dl_w++] = cur[1];
        if (is_spr) {
          dl[dl_w++] = cur[2];
          dl[dl_w++] = wh_word;
        } else {
          dl[dl_w++] = wh_word;
        }
        aux_prims++;
      }
      cur += 1 + len;
      continue;
    }

    if ((cmd >= 0x40 && cmd <= 0x43) || (cmd >= 0x50 && cmd <= 0x53)) {
      u32 is_gour = (cmd & 0x10U) ? 1U : 0U;
      s32 y0 = GPU_EXPANDSIGN(cur[1] >> 16) + off_y;
      s32 y1 = GPU_EXPANDSIGN(cur[is_gour ? 3 : 2] >> 16) + off_y;
      s32 ymin = (y0 < y1) ? y0 : y1;
      s32 ymax = ((y0 > y1) ? y0 : y1) + 1;
      if (ymax > y_split && ymin < band_y1) {
        u32 nwords = is_gour ? 4U : 3U;
        if (dl_w + 2U + nwords + 2U >= dl_cap) break;
        u32 pf = 0;
        if ((cmd & 2U) && do_blend_en)
          pf |= AUX_GPU_PF_BLEND | (((gp1 >> 5) & 3U) << AUX_GPU_PF_BMODE_SH);
        if (masking) pf |= AUX_GPU_PF_MASKING;
        if (msb)     pf |= AUX_GPU_PF_PIXELMSB;
        if (ymin < 0) ymin = 0;
        if (ymax > 511) ymax = 511;
        dl[dl_w++] = 0x80000000U | (nwords << 16) | (pf << 8) | cmd;
        dl[dl_w++] = ((u32)ymin << 16) | (u32)ymax;
        for (u32 k = 0; k < nwords; k++) dl[dl_w++] = cur[k];
        aux_prims++;
      }
      cur += 1 + len;
      continue;
    }

    /* Line strips (0x48..0x4F, 0x58..0x5F) or unknown commands: stop batch cleanly */
    break;
  }

  if (dl_w < dl_cap)
    dl[dl_w++] = 0U;
  *out_dl_words  = dl_w;
  *out_aux_prims = aux_prims;
  return (int)(cur - list);
}

static void aux_gpu_verify_batch(unsigned int *batch, int batch_words,
                                 const struct pcsx_aux_gpu_params *p)
{
  static int verify_done = 0;
  static int batch_seen = 0;
  int target = pcsx_aux_gpu_verify_mode();
  if (target < 2) target = 8;
  if (verify_done) return;
  if (++batch_seen < target) return; /* Wait for target gameplay batch */
  verify_done = 1;

  u32 x0 = p->strip_x0, x1 = p->strip_x1;
  u32 y0 = p->band_y0,  y1 = p->band_y1;
  u32 w = x1 - x0, h = y1 - y0;
  u16 *before = (u16 *)malloc(w * h * 2);
  u16 *aux_px = (u16 *)malloc(w * h * 2);
  if (!before || !aux_px) { free(before); free(aux_px); return; }

  /* 1. Snapshot initial bottom VRAM */
  for (u32 y = 0; y < h; y++)
    memcpy(before + y * w, gpu_unai.vram + FRAME_OFFSET(x0, y0 + y), w * 2);

  /* 2. Run AUX GPU on bottom band and wait */
  pcsx_aux_gpu_submit(p);
  pcsx_aux_gpu_flush();
  for (u32 y = 0; y < h; y++)
    memcpy(aux_px + y * w, gpu_unai.vram + FRAME_OFFSET(x0, y0 + y), w * 2);

  /* 3. Restore initial bottom VRAM and run Main CPU gpu_unai on the EXACT SAME bottom band! */
  for (u32 y = 0; y < h; y++)
    memcpy(gpu_unai.vram + FRAME_OFFSET(x0, y0 + y), before + y * w, w * 2);

  gpu_unai_t saved = gpu_unai;
  gpu_unai.DrawingArea[1] = (u16)y0;
  gpu_unai.DrawingArea[3] = (u16)y1;
  int dummy = 0;
  do_cmd_list_sw(batch, batch_words, &dummy);

  /* 4. Compare every single pixel! */
  u32 total = w * h, exact = 0, close1 = 0, diff = 0;
  u32 chg_aux = 0, chg_sw = 0, aux_only = 0, sw_only = 0, both_diff = 0;
  int shown = 0;
  for (u32 y = 0; y < h; y++) {
    u16 *sw_row = gpu_unai.vram + FRAME_OFFSET(x0, y0 + y);
    u16 *ax_row = aux_px + y * w;
    for (u32 x = 0; x < w; x++) {
      u16 a = ax_row[x], s = sw_row[x], b = before[y * w + x];
      if (a != b) chg_aux++;
      if (s != b) chg_sw++;
      if (a == s) {
        exact++;
      } else {
        if (a == b && s != b) sw_only++;
        else if (s == b && a != b) aux_only++;
        else both_diff++;
        int dr = abs((int)(a & 31) - (int)(s & 31));
        int dg = abs((int)((a >> 5) & 31) - (int)((s >> 5) & 31));
        int db = abs((int)((a >> 10) & 31) - (int)((s >> 10) & 31));
        if (dr <= 1 && dg <= 1 && db <= 1) close1++;
        else {
          diff++;
          if (shown < 8) {
            printf("  [VERIFY DIFF] (%u,%u) aux=0x%04x sw=0x%04x (before=0x%04x)\n",
                   x0 + x, y0 + y, a, s, b);
            shown++;
          }
        }
      }
    }
  }
  printf("[AUX-GPU-VERIFY] rect=(%u..%u, %u..%u) dl_words=%u off=(%d,%d) gp1=0x%x bm=0x%x | total=%u exact=%u (%.2f%%) close1=%u diff=%u | chg_aux=%u chg_sw=%u sw_only=%u aux_only=%u both_diff=%u\n",
         x0, x1, y0, y1, p->dl_words, p->init_off_x, p->init_off_y, p->init_gp1, p->blit_mask,
         total, exact, 100.0 * exact / (double)total, close1, diff,
         chg_aux, chg_sw, sw_only, aux_only, both_diff);
  fflush(stdout);

  /* Restore gpu_unai state & initial VRAM so normal split executes cleanly */
  gpu_unai = saved;
  for (u32 y = 0; y < h; y++)
    memcpy(gpu_unai.vram + FRAME_OFFSET(x0, y0 + y), before + y * w, w * 2);
  free(before);
  free(aux_px);
}
#endif

int do_cmd_list(unsigned int *list, int list_len, int *last_cmd)
{
#ifdef RS97
  if (pcsx_aux_gpu_ready() && list_len >= 16 && !DitheringEnabled()) {
    unsigned int *cur = list;
    unsigned int *end = list + list_len;
    int split_pct = pcsx_aux_gpu_split_pct();

    while (cur < end) {
      /* Check if remaining list is large enough & current DrawingArea is splittable */
      int rem = (int)(end - cur);
      u32 cmd0 = *cur >> 24;
      int dx0 = gpu_unai.DrawingArea[0];
      int dy0 = gpu_unai.DrawingArea[1];
      int dx1 = gpu_unai.DrawingArea[2];
      int dy1 = gpu_unai.DrawingArea[3];
      int dw = dx1 - dx0, dh = dy1 - dy0;

      if (rem >= 24 && dw >= 64 && dw <= 640 && dh >= 64 &&
          cmd0 != 0x02 && cmd0 != 0x80 && cmd0 != 0xA0 && cmd0 != 0xC0 &&
          cmd0 != 0xE3 && cmd0 != 0xE4) {
        uint32_t dl_cap = 0;
        uint32_t *dl = pcsx_aux_gpu_dl_ptr(&dl_cap);
        int y_split = dy0 + ((dh * split_pct) / 100);
        uint32_t dl_words = 0, aux_prims = 0;
        int consumed = aux_build_dl_batch(cur, rem, y_split, dy1, dl, dl_cap,
                                          &dl_words, &aux_prims);
        if (consumed > 0 && aux_prims >= 4U) {
          struct pcsx_aux_gpu_params p;
          p.dl_words     = dl_words;
          p.strip_x0     = (uint16_t)(dx0 & ~1);
          p.strip_x1     = (uint16_t)((dx1 + 1) & ~1);
          if (p.strip_x1 > 1024U) p.strip_x1 = 1024U;
          p.band_y0      = (uint16_t)y_split;
          p.band_y1      = (uint16_t)dy1;
          p.init_gp1     = gpu_unai.GPU_GP1;
          p.init_texwin  = 0xE2000000U | (gpu_unai.TextureWindowCur & 0x00FFFFFFU);
          p.init_off_x   = gpu_unai.DrawingOffset[0];
          p.init_off_y   = gpu_unai.DrawingOffset[1];
          p.init_draw_x0 = gpu_unai.DrawingArea[0];
          p.init_draw_x1 = gpu_unai.DrawingArea[2];
          p.init_draw_y0 = gpu_unai.DrawingArea[1];
          p.init_draw_y1 = gpu_unai.DrawingArea[3];
          p.blit_mask    = gpu_unai.blit_mask;
          p.ilace_mask   = gpu_unai.ilace_mask;
          p.init_masking = gpu_unai.Masking ? 1 : 0;
          p.init_msb     = gpu_unai.PixelMSB ? 1 : 0;

          if (__builtin_expect(pcsx_aux_gpu_verify_mode(), 0))
            aux_gpu_verify_batch(cur, consumed, &p);

          if (pcsx_aux_gpu_submit(&p)) {
            /* Main CPU renders top [dy0 .. y_split) IN PARALLEL with AUX rendering [y_split .. dy1)! */
            u16 orig_y1 = gpu_unai.DrawingArea[3];
            gpu_unai.DrawingArea[3] = (u16)y_split;
            int sub_last = 0;
            do_cmd_list_sw(cur, consumed, &sub_last);
            gpu_unai.DrawingArea[3] = orig_y1;
            cur += consumed;
            *last_cmd = sub_last;
            continue;
          }
        }
      }

      /* Unsplit single command (e.g. 0x02 ClearImage, 0x80 MoveImage, 0xE3/E4, or tiny tail) */
      u32 len = cmd_lengths[cmd0];
      if (cur + 1 + len > end) {
        *last_cmd = -1;
        return (int)(cur - list);
      }
      if (cmd0 == 0xA0 || cmd0 == 0xC0) {
        *last_cmd = (int)cmd0;
        return (int)(cur - list);
      }
      /* Flush in-flight AUX GPU before any unsplit draw/clear/move that could touch bottom half */
      if (cmd0 != 0xE1 && cmd0 != 0xE2 && cmd0 != 0xE5 && cmd0 != 0xE6)
        pcsx_aux_gpu_flush();

      /* Handle line strips by passing remaining tail to SW */
      if ((cmd0 >= 0x48 && cmd0 <= 0x4F) || (cmd0 >= 0x58 && cmd0 <= 0x5F)) {
        pcsx_aux_gpu_flush();
        int sw_done = do_cmd_list_sw(cur, rem, last_cmd);
        cur += sw_done;
        return (int)(cur - list);
      }

      int sub_last = 0;
      int sw_done = do_cmd_list_sw(cur, (int)(1 + len), &sub_last);
      cur += sw_done;
      *last_cmd = sub_last;
      if (sw_done == 0) break;
    }
    return (int)(cur - list);
  }
  /* Small non-splittable call: ensure any prior AUX GPU batch is flushed before Main CPU draws */
  pcsx_aux_gpu_flush();
#endif
  return do_cmd_list_sw(list, list_len, last_cmd);
}

void renderer_sync_ecmds(uint32_t *ecmds)
{
  int dummy;
  do_cmd_list(&ecmds[1], 6, &dummy);
}

void renderer_update_caches(int x, int y, int w, int h)
{
}

void renderer_flush_queues(void)
{
#ifdef RS97
  pcsx_aux_gpu_flush();
#endif
}

void renderer_set_interlace(int enable, int is_odd)
{
}

// Handle any gpulib settings applicable to gpu_unai:
void renderer_set_config(const gpulib_config_t *config)
{
  gpu_unai.vram = (u16*)gpu.vram;
}

// vim:shiftwidth=2:expandtab
