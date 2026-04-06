/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

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

intel_remap_func intel_fb_remap_func(bool rotate)
{
	if (rotate)
		return remap_tiled_r270;
	else if (0)
		return remap_tiled_r0;
	else
		return NULL;
}
