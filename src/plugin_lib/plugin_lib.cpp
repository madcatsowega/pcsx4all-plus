/***************************************************************************
 * (C) notaz, 2010-2011                                                    *
 * (C) PCSX4ALL team 2016                                                  *
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

/*
 * Plugin library to assist with frameskip, controls, etc.
 * Largely taken/based on Notaz's PCSX Rearmed.
 *
 * Added Dec 2016 by senquack (Daniel Silsby)
 *
 */

#include <unistd.h>
#include <time.h>

#include "psxcommon.h"
#include "plugin_lib.h"
#include "perfmon.h"
#include "plugins.h"
#include "aux/pcsx_aux.h"

#ifdef USE_GPULIB
#include "gpu/gpulib/gpu.h"
#endif

static void pl_frameskip_prepare(void);
static void pl_stats_update(void);

#define MAX_LAG_FRAMES 6

static inline int64_t pl_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
}

#define tvdiff(tv, tv_old) \
	((tv.tv_sec - tv_old.tv_sec) * 1000000 + tv.tv_usec - tv_old.tv_usec)

struct pl_data_t pl_data;

void pl_clear_screen()
{
	u16 *dst = SCREEN;
	memset((void*)dst, 0, 320*240*2);
}

void pl_clear_borders()
{
	gettimeofday(&pl_data.tv_last_clear, 0);
	pl_clear_screen();

	pl_data.clear_ctr = 4;
}

static void pl_frameskip_prepare(void)
{
	pl_data.fskip_advice = false;
	pl_data.frameskip = Config.FrameSkip;
	pl_data.is_pal = (Config.PsxType == PSXTYPE_PAL);
	pl_data.frame_interval = pl_data.is_pal ? 20000 : 16667;
	pl_data.frame_interval1024 = pl_data.is_pal ? 20000*1024 : 17066667;

	struct timeval now;
	gettimeofday(&pl_data.tv_last_clear, 0);
	gettimeofday(&now, 0);
	pl_data.vsync_usec_time = now.tv_usec;
	while (pl_data.vsync_usec_time >= pl_data.frame_interval)
		pl_data.vsync_usec_time -= pl_data.frame_interval;
	pl_data.expect_us1024 = pl_now_us() << 10;

#ifdef USE_GPULIB
	gpulib_frameskip_prepare();
#endif
}

/* called on every vsync */
void pl_frame_limit(void)
{
	struct timeval now;
	int64_t now_us, diff;

	gettimeofday(&now, 0);
	now_us = pl_now_us();

	GPU_getScreenInfo(&pl_data.sinfo);

	if (pl_data.clear_ctr > 0) {
		int cdiff = tvdiff(now, pl_data.tv_last_clear);
		if (cdiff > 166667) {
			pl_data.clear_ctr--;
			pl_data.tv_last_clear = now;
			pl_clear_screen();
		}

	}

	// Update performance monitor
	bool new_stats = pmonUpdate(&now);
	if (new_stats) {
		pmonGetStats(&pl_data.fps_cur, &pl_data.cpu_cur);
		pl_stats_update();
	}

	// If cfg settings change, catch it here
	if (pl_data.frameskip != Config.FrameSkip ||
	    pl_data.is_pal != (Config.PsxType == PSXTYPE_PAL))
	{
		pl_frameskip_prepare();
		now_us = pl_now_us();
	}

	// Advance expected monotonic VBlank deadline in usec*1024 units
	pl_data.expect_us1024 += (int64_t)pl_data.frame_interval1024;
	diff = (pl_data.expect_us1024 >> 10) - now_us;

	// Large positive jump (menu/resume) or severe stall (>6 frames = ~100ms): re-anchor cleanly
	if (diff > (int64_t)MAX_LAG_FRAMES * pl_data.frame_interval) {
		pl_data.expect_us1024 = now_us << 10;
		diff = 0;
		pl_data.fskip_advice = false;
	} else if (diff < -(int64_t)MAX_LAG_FRAMES * pl_data.frame_interval) {
		// Soft-clamp lag to -2 frames instead of resetting to 0 so skip cadence stays smooth
		// without periodic stutter/surge waves on sub-fullspeed scenes.
		int64_t clamp_lag = (int64_t)2 * pl_data.frame_interval;
		pl_data.expect_us1024 = (now_us - clamp_lag) << 10;
		diff = -clamp_lag;
	}

	// Hybrid precision frame pacer:
	//   - Small 1/2-frame lead cushion (~8.3ms) absorbs micro-jitter with low latency.
	//   - Coarse usleep() leaves a 10ms margin so Linux 2.6 jiffies NEVER overshoot the deadline.
	//   - Final <=10ms finishes with tight monotonic polling (never enters cpu_idle()!).
	if (Config.FrameLimit) {
		int64_t cushion = (int64_t)(pl_data.frame_interval >> 1);
		int64_t target_us = (pl_data.expect_us1024 >> 10) - cushion;
		int64_t sleep_us = target_us - now_us;
		if (sleep_us > 0) {
			if (sleep_us > 11000) {
#ifdef RS97
				pcsx_aux_flush();
#endif
				usleep((useconds_t)(sleep_us - 10000));
			}
			while (pl_now_us() < target_us) {
#ifdef RS97
				pcsx_aux_reap_ready();
#endif
			}
			now_us = pl_now_us();
			diff = (pl_data.expect_us1024 >> 10) - now_us;
		}
	}

	// Spaced 1-for-1 auto-frameskip trigger:
	// Set advice once >=1 full frame behind; decide_frameskip() calls pl_frameskip_consume()
	// immediately after skipping 1 frame so 55-57 FPS skips single spaced frames, never bursts!
	if (diff < -(int64_t)pl_data.frame_interval) {
		pl_data.fskip_advice = true;
	} else if (diff >= -(int64_t)(pl_data.frame_interval >> 1)) {
		pl_data.fskip_advice = false;
	}

	// recompilation is not that fast and may cause frame skip on
	// loading screens and such, resulting in flicker or glitches
	if (pl_data.dynarec_compiled) {
		if (pl_data.dynarec_active_vsyncs < 32)
			pl_data.fskip_advice = false;
		pl_data.dynarec_active_vsyncs++;
	} else {
		pl_data.dynarec_active_vsyncs = 0;
	}
	pl_data.dynarec_compiled = false;
}

void pl_init(void)
{
	pl_reset();

#ifdef USE_GPULIB
	gpulib_set_config(&gpulib_config);
#endif
}

void pl_reset(void)
{
	pl_data.clear_ctr = 0;
	pl_data.fps_cur = pl_data.cpu_cur = 0;
	pl_data.dynarec_compiled = false;
	pl_data.dynarec_active_vsyncs = 0;
	pl_frameskip_prepare();
	strcpy(pl_data.stats_msg, "");
	// sprintf(pl_data.stats_msg, "000x000x00 CPU=000%% FPS=000/00");
	pmonReset(); // Reset performance monitor (FPS,CPU usage,etc)
}

// Called when emu paused and frontend is active
void pl_pause(void)
{
	pmonPause();
}

// Called when leaving frontend back to emu
void pl_resume(void)
{
	pmonResume();
	pl_frameskip_prepare();
	GPU_requestScreenRedraw(); // GPU plugin should redraw screen
}

static void pl_stats_update(void)
{
	// TODO: show skipped frames in stats message
	sprintf(pl_data.stats_msg, "%ux%ux%s CPU=%u%% FPS=%u/%u",
			pl_data.sinfo.hres,
			pl_data.sinfo.vres,
			pl_data.sinfo.depth24 ? "24" : "15",
			(unsigned int)(pl_data.cpu_cur + 0.5f),
			(unsigned int)(pl_data.fps_cur + 0.5f),
			pl_data.sinfo.pal ? 50 : 60);
}
