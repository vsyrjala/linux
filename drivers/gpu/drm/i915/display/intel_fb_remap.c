/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#include <drm/drm_fourcc.h>

#include "intel_display_utils.h"
#include "intel_fb_remap.h"

static void remap_rotate_270(unsigned int *x, unsigned int *y,
			     unsigned int w, unsigned int h)
{
	int dx = *x, dy = *y;

	*y = w - dx - 1;
	*x = dy;
}

static unsigned int remap_tiled_r0(unsigned int offset,
				   unsigned int x, unsigned int y,
				   unsigned int w, unsigned int h,
				   unsigned int src_stride)
{
	return offset + y * src_stride + x;
}

static unsigned int remap_tiled_r270(unsigned int offset,
				     unsigned int x, unsigned int y,
				     unsigned int w, unsigned int h,
				     unsigned int src_stride)
{
	remap_rotate_270(&x, &y, w, h);

	return remap_tiled_r0(offset, x, y, w, h, src_stride);
}

/*
 * Each 64KiB Ys tile is made up of 4x4 Yf tiles,
 * stored in the following order:
 * | 0| 2| 8|10|
 * | 1| 3| 9|11|
 * | 4| 6|12|14|
 * | 5| 7|13|15|
 */
static unsigned int remap_ys_tiled_r0(unsigned int offset,
				      unsigned int x, unsigned int y,
				      unsigned int w, unsigned int h,
				      unsigned int stride)
{
	bool x0, x1, y0, y1;

	/* full 64KiB tile rows */
	offset += (y & ~0b11) * stride;

	/* full 64KiB tiles */
	offset += (x & ~0b11) * 4;

	x0 = y & 0b01;
	x1 = x & 0b01;
	y0 = y & 0b10;
	y1 = x & 0b10;

	x = (x1 << 1) | x0;
	y = (y1 << 1) | y0;

	return offset + y * 4 + x;
}

static unsigned int remap_ys_tiled_r270(unsigned int offset,
					unsigned int x, unsigned int y,
					unsigned int w, unsigned int h,
					unsigned int stride)
{
	remap_rotate_270(&x, &y, w, h);

	return remap_ys_tiled_r0(offset, x, y, w, h, stride);
}

static unsigned int remap_tile64(unsigned int offset,
				 unsigned int x, unsigned int y,
				 unsigned int w, unsigned int h,
				 unsigned int stride,
				 unsigned int tw, unsigned int th)
{
	/* full 64KiB tile rows */
	offset += (y & ~(th - 1)) * stride;

	/* full 64KiB tiles */
	offset += (x & ~(tw - 1)) * th;

	y &= th - 1;
	x &= tw - 1;

	return offset + y * tw + x;
}

/*
 * Each 64KiB Tile64 is made up of 2x8 4KiB Tile4s,
 * stored in row major order. Use for 8bpp formats.
 */
static unsigned int remap_64_tiled_8_r0(unsigned int offset,
					unsigned int x, unsigned int y,
					unsigned int w, unsigned int h,
					unsigned int stride)
{
	return remap_tile64(offset, x, y, w, h, stride, 2, 8);
}

static unsigned int remap_64_tiled_8_r270(unsigned int offset,
					  unsigned int x, unsigned int y,
					  unsigned int w, unsigned int h,
					  unsigned int stride)
{
	remap_rotate_270(&x, &y, w, h);

	return remap_64_tiled_8_r0(offset, x, y, w, h, stride);
}

/*
 * Each 64KiB Tile64 is made up of 4x4 4KiB Tile4s,
 * stored in row major order. Use for 16bpp and 32bpp formats.
 */
static unsigned int remap_64_tiled_16_32_r0(unsigned int offset,
					    unsigned int x, unsigned int y,
					    unsigned int w, unsigned int h,
					    unsigned int stride)
{
	return remap_tile64(offset, x, y, w, h, stride, 4, 4);
}

static unsigned int remap_64_tiled_16_32_r270(unsigned int offset,
					      unsigned int x, unsigned int y,
					      unsigned int w, unsigned int h,
					      unsigned int stride)
{
	remap_rotate_270(&x, &y, w, h);

	return remap_64_tiled_16_32_r0(offset, x, y, w, h, stride);
}

/*
 * Each 64KiB Tile64 is made up of 8x2 4KiB Tile4s,
 * stored in row major order. Use for 64bpp and 128bpp formats.
 */
static unsigned int remap_64_tiled_64_128_r0(unsigned int offset,
					     unsigned int x, unsigned int y,
					     unsigned int w, unsigned int h,
					     unsigned int stride)
{
	return remap_tile64(offset, x, y, w, h, stride, 8, 2);
}

static unsigned int remap_64_tiled_64_128_r270(unsigned int offset,
					       unsigned int x, unsigned int y,
					       unsigned int w, unsigned int h,
					       unsigned int stride)
{
	remap_rotate_270(&x, &y, w, h);

	return remap_64_tiled_64_128_r0(offset, x, y, w, h, stride);
}

intel_remap_func intel_fb_remap_func(u64 modifier, unsigned int cpp, bool rotate)
{
	switch (modifier) {
	case I915_FORMAT_MOD_64_TILED:
		switch (cpp) {
		case 1:
			if (rotate)
				return remap_64_tiled_8_r270;
			else
				return remap_64_tiled_8_r0;
		case 2:
		case 4:
			if (rotate)
				return remap_64_tiled_16_32_r270;
			else
				return remap_64_tiled_16_32_r0;
		case 8:
		case 16:
			if (rotate)
				return remap_64_tiled_64_128_r270;
			else
				return remap_64_tiled_64_128_r0;
		default:
			MISSING_CASE(cpp);
			return NULL;
		}
	case I915_FORMAT_MOD_Ys_TILED:
		if (rotate)
			return remap_ys_tiled_r270;
		else
			return remap_ys_tiled_r0;
	default:
		if (rotate)
			return remap_tiled_r270;
		else if (1)
			return remap_tiled_r0;
		else
			return NULL;
	}
}
