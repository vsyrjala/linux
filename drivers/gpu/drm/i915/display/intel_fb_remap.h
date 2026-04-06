/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef __INTEL_FB_REMAP_H__
#define __INTEL_FB_REMAP_H__

#include <linux/types.h>

#include "i915_gtt_view_types.h"

intel_remap_func intel_fb_remap_func(bool rotate);

#endif /* __INTEL_FB_REMAP_H__ */
