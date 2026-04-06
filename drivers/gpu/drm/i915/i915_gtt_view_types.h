/* SPDX-License-Identifier: MIT */
/* Copyright © 2025 Intel Corporation */

#ifndef __I915_GTT_VIEW_TYPES_H__
#define __I915_GTT_VIEW_TYPES_H__

#include <linux/types.h>

struct intel_remapped_plane_info {
	/* in gtt pages */
	u32 offset:31;
	u32 linear:1;
	union {
		/* in gtt pages for !linear */
		struct {
			u16 width;
			u16 height;
			u16 src_stride;
			u16 dst_stride;
		};

		/* in gtt pages for linear */
		u32 size;
	};
} __packed;

struct intel_partial_info {
	u64 offset;
	unsigned int size;
} __packed;

/*
 * Custom remap function for tiled color planes. Linear color planes
 * are not remapped by the function. Input x,y,w,h are in destination
 * coordinate space. The remap function returns the corresponding
 * source tile offset. Units are 4KiB tiles.
 */
typedef unsigned int (*intel_remap_func)(unsigned int offset,
					 unsigned int x, unsigned int y,
					 unsigned int w, unsigned int h,
					 unsigned int src_stride);

struct intel_remapped_info {
	struct intel_remapped_plane_info plane[4];
	/* TODO we might need a per-color plane remap func eg. for tile64+NV12 */
	intel_remap_func remap;
	/* in gtt pages */
	u32 plane_alignment;
	bool rotated;
} __packed;

enum i915_gtt_view_type {
	I915_GTT_VIEW_NORMAL = 0,
	I915_GTT_VIEW_PARTIAL = sizeof(struct intel_partial_info),
	I915_GTT_VIEW_REMAPPED = sizeof(struct intel_remapped_info),
};

struct i915_gtt_view {
	enum i915_gtt_view_type type;
	union {
		/* Members need to contain no holes/padding */
		struct intel_partial_info partial;
		struct intel_remapped_info remapped;
	};
};

static inline bool i915_gtt_view_is_normal(const struct i915_gtt_view *view)
{
	return view->type == I915_GTT_VIEW_NORMAL;
}

static inline bool i915_gtt_view_is_remapped(const struct i915_gtt_view *view)
{
	return view->type == I915_GTT_VIEW_REMAPPED && !view->remapped.rotated;
}

static inline bool i915_gtt_view_is_rotated(const struct i915_gtt_view *view)
{
	return view->type == I915_GTT_VIEW_REMAPPED && view->remapped.rotated;
}

#endif /* __I915_GTT_VIEW_TYPES_H__ */
