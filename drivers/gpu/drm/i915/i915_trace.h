#if !defined(_I915_TRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _I915_TRACE_H_

#include <linux/stringify.h>
#include <linux/types.h>
#include <linux/tracepoint.h>

#include <drm/drmP.h>
#include "i915_drv.h"
#include "intel_drv.h"
#include "intel_ringbuffer.h"

#undef TRACE_SYSTEM
#define TRACE_SYSTEM i915
#define TRACE_SYSTEM_STRING __stringify(TRACE_SYSTEM)
#define TRACE_INCLUDE_FILE i915_trace

/* watermark */

TRACE_EVENT(i915_wm_plane_params,
	    TP_PROTO(const char *name, const struct intel_plane_wm_parameters *old, const struct intel_plane_wm_parameters *new),
	    TP_ARGS(name, old, new),
	    TP_STRUCT__entry(
			     __field(const char *, name)
			     __field(bool, old_enabled)
			     __field(bool, new_enabled)
			     __field(bool, old_scaled)
			     __field(bool, new_scaled)
			     ),

	    TP_fast_assign(
			   __entry->name = name;
			   __entry->old_enabled = old->enabled;
			   __entry->new_enabled = new->enabled;
			   __entry->old_scaled = old->scaled;
			   __entry->new_scaled = new->scaled;
			   ),

	    TP_printk("plane=%s, enabled=%d->%d, scaled=%d->%d\n",
		      __entry->name,
		      __entry->old_enabled, __entry->new_enabled,
		      __entry->old_scaled, __entry->new_scaled)
);

TRACE_EVENT_CONDITION(i915_wm_pipe_wm,
	    TP_PROTO(enum pipe pipe, const struct intel_pipe_wm *pipe_wm, bool trace),
	    TP_ARGS(pipe, pipe_wm, trace),

	    TP_CONDITION(trace),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(uint32_t, linetime)
			     __field(bool, fbc_wm_enabled)
			     __field(bool, pipe_enabled)
			     __field(bool, primary_enabled)
			     __field(bool, sprites_enabled)
			     __field(bool, sprites_scaled)
			     ),

	    TP_fast_assign(
			   __entry->pipe = pipe;
			   __entry->linetime = pipe_wm->linetime;
			   __entry->fbc_wm_enabled = pipe_wm->fbc_wm_enabled;
			   __entry->pipe_enabled = pipe_wm->pipe_enabled;
			   __entry->primary_enabled = pipe_wm->primary_enabled;
			   __entry->sprites_enabled = pipe_wm->sprites_enabled;
			   __entry->sprites_scaled = pipe_wm->sprites_scaled;
			   ),

	    TP_printk("pipe=%c linetime=%u, fbc=%s, pipe en=%s, pri en=%s, spr en=%s, spr scaled=%s",
		      pipe_name(__entry->pipe), __entry->linetime,
		      __entry->fbc_wm_enabled ? "yes" : "no",
		      __entry->pipe_enabled ? "yes" : "no",
		      __entry->primary_enabled ? "yes" : "no",
		      __entry->sprites_enabled ? "yes" : "no",
		      __entry->sprites_scaled ? "yes" : "no")
);


TRACE_EVENT_CONDITION(i915_wm_level,
	    TP_PROTO(enum pipe pipe, int level, const struct intel_pipe_wm *pipe_wm, bool trace),
	    TP_ARGS(pipe, level, pipe_wm, trace),

	    TP_CONDITION(trace),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(int, level)
			     __field(bool, enable)
			     __field(uint32_t, pri_val)
			     __field(uint32_t, spr_val)
			     __field(uint32_t, cur_val)
			     __field(uint32_t, fbc_val)
			     ),

	    TP_fast_assign(
			   __entry->pipe = pipe;
			   __entry->level = level;
			   __entry->enable = pipe_wm->wm[level].enable;
			   __entry->pri_val = pipe_wm->wm[level].pri_val;
			   __entry->spr_val = pipe_wm->wm[level].spr_val;
			   __entry->cur_val = pipe_wm->wm[level].cur_val;
			   __entry->fbc_val = pipe_wm->wm[level].fbc_val;
			   ),

	    TP_printk("pipe=%c level=%u, en=%s, fbc=%u, pri=%u, cur=%u, spr=%u",
		      pipe_name(__entry->pipe), __entry->level,
		      __entry->enable ? "yes" : "no", __entry->fbc_val,
		      __entry->pri_val, __entry->cur_val,
		      __entry->spr_val)
);

TRACE_EVENT(i915_wm_setup_pipe,
	    TP_PROTO(enum pipe pipe, u32 vbl_count, u32 pending_vbl_count),
	    TP_ARGS(pipe, vbl_count, pending_vbl_count),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, vbl_count)
			     __field(u32, pending_vbl_count)
			     ),

	    TP_fast_assign(
			   __entry->pipe = pipe;
			   __entry->vbl_count = vbl_count;
			   __entry->pending_vbl_count = pending_vbl_count;
			   ),

	    TP_printk("pipe=%c vbl_count=%u pending_vbl_count=%u", pipe_name(__entry->pipe),
		      __entry->vbl_count, __entry->pending_vbl_count)
);

TRACE_EVENT(i915_wm_refresh_watermarks,
	    TP_PROTO(enum pipe pipe, u32 vbl_count, u32 pending_vbl_count),
	    TP_ARGS(pipe, vbl_count, pending_vbl_count),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, vbl_count)
			     __field(u32, pending_vbl_count)
			     ),

	    TP_fast_assign(
			   __entry->pipe = pipe;
			   __entry->vbl_count = vbl_count;
			   __entry->pending_vbl_count = pending_vbl_count;
			   ),

	    TP_printk("pipe=%c vbl_count=%u pending_vbl_count=%u", pipe_name(__entry->pipe),
		      __entry->vbl_count, __entry->pending_vbl_count)
);

TRACE_EVENT(i915_wm_work_schedule,
	    TP_PROTO(enum pipe pipe, u32 vbl_count, u32 pending_vbl_count),
	    TP_ARGS(pipe, vbl_count, pending_vbl_count),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, vbl_count)
			     __field(u32, pending_vbl_count)
			     ),

	    TP_fast_assign(
			   __entry->pipe = pipe;
			   __entry->vbl_count = vbl_count;
			   __entry->pending_vbl_count = pending_vbl_count;
			   ),

	    TP_printk("pipe=%c vbl_count=%u pending_vbl_count=%u", pipe_name(__entry->pipe),
		      __entry->vbl_count, __entry->pending_vbl_count)
);

TRACE_EVENT(i915_wm_vblank,
	    TP_PROTO(enum pipe pipe, u32 vbl_count, u32 pending_vbl_count),
	    TP_ARGS(pipe, vbl_count, pending_vbl_count),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, vbl_count)
			     __field(u32, pending_vbl_count)
			     ),

	    TP_fast_assign(
			   __entry->pipe = pipe;
			   __entry->vbl_count = vbl_count;
			   __entry->pending_vbl_count = pending_vbl_count;
			   ),

	    TP_printk("pipe=%c vbl_count=%u pending_vbl_count=%u", pipe_name(__entry->pipe),
		      __entry->vbl_count, __entry->pending_vbl_count)
);

TRACE_EVENT(i915_wm_work_start,
	    TP_PROTO(int dummy),
	    TP_ARGS(dummy),

	    TP_STRUCT__entry(
			     __field(int, dummy)
			     ),

	    TP_fast_assign(
			   __entry->dummy = dummy;
			   ),

	    TP_printk("%d", __entry->dummy)
);

TRACE_EVENT(i915_wm_work_end,
	    TP_PROTO(bool changed),
	    TP_ARGS(changed),

	    TP_STRUCT__entry(
			     __field(bool, changed)
			     ),

	    TP_fast_assign(
			   __entry->changed = changed;
			   ),

	    TP_printk("changed=%s", __entry->changed ? "yes" : "no")
);

TRACE_EVENT(i915_wm_update_start,
	    TP_PROTO(enum pipe pipe),
	    TP_ARGS(pipe),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     ),

	    TP_fast_assign(
			   __entry->pipe = pipe;
			   ),

	    TP_printk("pipe %c", pipe_name(__entry->pipe))
);

TRACE_EVENT(i915_wm_update_end,
	    TP_PROTO(enum pipe pipe, bool changed),
	    TP_ARGS(pipe, changed),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(bool, changed)
			     ),

	    TP_fast_assign(
			   __entry->pipe = pipe;
			   __entry->changed = changed;
			   ),

	    TP_printk("pipe %c, changed=%s",
		      pipe_name(__entry->pipe), __entry->changed ? "yes" : "no")
);

TRACE_EVENT_CONDITION(i915_wm_disable_lp,
	    TP_PROTO(bool changed),
	    TP_ARGS(changed),

	    TP_CONDITION(changed),

	    TP_STRUCT__entry(
			     __field(bool, changed)
			     ),

	    TP_fast_assign(
			   __entry->changed = changed;
			   ),

	    TP_printk("changed=%s", __entry->changed ? "yes" : "no")
);

TRACE_EVENT_CONDITION(i915_wm_misc,
	TP_PROTO(const struct ilk_wm_values *hw, bool trace),
	TP_ARGS(hw, trace),

	TP_CONDITION(trace),

	TP_STRUCT__entry(
		__field(bool, enable_fbc_wm)
		__field(enum intel_ddb_partitioning, partitioning)
		),

	TP_fast_assign(
		__entry->enable_fbc_wm = hw->enable_fbc_wm;
		__entry->partitioning = hw->partitioning;
		),

	TP_printk("fbc=%s, ddb partitioning=%s",
		__entry->enable_fbc_wm ? "yes" : "no",
		__entry->partitioning == INTEL_DDB_PART_5_6 ? "5/6" : "1/2")
);

TRACE_EVENT_CONDITION(i915_wm_pipe,
	TP_PROTO(struct drm_device *dev, enum pipe pipe, const struct ilk_wm_values *hw, bool trace),
	TP_ARGS(dev, pipe, hw, trace),

	TP_CONDITION(pipe < INTEL_INFO(dev)->num_pipes && trace),

	TP_STRUCT__entry(
		__field(enum pipe, pipe)
		__field(uint32_t, wm_pipe)
		),

	TP_fast_assign(
		__entry->pipe = pipe;
		__entry->wm_pipe = hw->wm_pipe[pipe];
		),

	TP_printk("pipe %c: pri=%u, spr=%u, cur=%u",
		pipe_name(__entry->pipe),
		(__entry->wm_pipe & WM0_PIPE_PLANE_MASK) >> WM0_PIPE_PLANE_SHIFT,
		(__entry->wm_pipe & WM0_PIPE_SPRITE_MASK) >> WM0_PIPE_SPRITE_SHIFT,
		__entry->wm_pipe & WM0_PIPE_CURSOR_MASK)
);

TRACE_EVENT_CONDITION(i915_wm_linetime,
	TP_PROTO(struct drm_device *dev, enum pipe pipe, const struct ilk_wm_values *hw, bool trace),
	TP_ARGS(dev, pipe, hw, trace),

	TP_CONDITION(IS_HASWELL(dev) && pipe < INTEL_INFO(dev)->num_pipes && trace),

	TP_STRUCT__entry(
		__field(enum pipe, pipe)
		__field(uint32_t, wm_linetime)
		),

	TP_fast_assign(
		__entry->pipe = pipe;
		__entry->wm_linetime = hw->wm_linetime[pipe];
		),

	TP_printk("pipe %c: linetime=%u, ips linetime=%u",
		pipe_name(__entry->pipe),
		__entry->wm_linetime & PIPE_WM_LINETIME_MASK,
		(__entry->wm_linetime & PIPE_WM_LINETIME_IPS_LINETIME_MASK) >> 16)
);


TRACE_EVENT_CONDITION(i915_wm_lp1_ilk,
	TP_PROTO(struct drm_device *dev, const struct ilk_wm_values *hw, bool trace),
	TP_ARGS(dev, hw, trace),

	TP_CONDITION(INTEL_INFO(dev)->gen <= 6 && trace),

	TP_STRUCT__entry(
		__field(uint32_t, wm_lp)
		__field(uint32_t, wm_lp_spr)
		),

	TP_fast_assign(
		__entry->wm_lp = hw->wm_lp[0];
		__entry->wm_lp_spr = hw->wm_lp_spr[0];
		),

	TP_printk("LP1: en=%s, lat=%u, fbc=%u, pri=%u, cur=%u, spr=%u, spr en=%s",
		__entry->wm_lp & WM1_LP_SR_EN ? "yes" : "no",
		(__entry->wm_lp & WM1_LP_LATENCY_MASK) >> WM1_LP_LATENCY_SHIFT,
		(__entry->wm_lp & WM1_LP_FBC_MASK) >> WM1_LP_FBC_SHIFT,
		(__entry->wm_lp & WM1_LP_SR_MASK) >> WM1_LP_SR_SHIFT,
		__entry->wm_lp & WM1_LP_CURSOR_MASK,
		__entry->wm_lp_spr & ~WM1S_LP_EN,
		__entry->wm_lp_spr & WM1S_LP_EN ? "yes" : "no")
);

TRACE_EVENT_CONDITION(i915_wm_lp_ilk,
	TP_PROTO(struct drm_device *dev, int lp, const struct ilk_wm_values *hw, bool trace),
	TP_ARGS(dev, lp, hw, trace),

	TP_CONDITION(INTEL_INFO(dev)->gen <= 6 && trace),

	TP_STRUCT__entry(
		__field(int, lp)
		__field(uint32_t, wm_lp)
		),

	TP_fast_assign(
		__entry->lp = lp;
		__entry->wm_lp = hw->wm_lp[lp - 1];
		),

	TP_printk("LP%d: en=%s, lat=%u, fbc=%u, pri=%u, cur=%u",
		__entry->lp,
		__entry->wm_lp & WM1_LP_SR_EN ? "yes" : "no",
		(__entry->wm_lp & WM1_LP_LATENCY_MASK) >> WM1_LP_LATENCY_SHIFT,
		(__entry->wm_lp & WM1_LP_FBC_MASK) >> WM1_LP_FBC_SHIFT,
		(__entry->wm_lp & WM1_LP_SR_MASK) >> WM1_LP_SR_SHIFT,
		__entry->wm_lp & WM1_LP_CURSOR_MASK)
);

TRACE_EVENT_CONDITION(i915_wm_lp_ivb,
	TP_PROTO(struct drm_device *dev, int lp, const struct ilk_wm_values *hw, bool trace),
	TP_ARGS(dev, lp, hw, trace),

	TP_CONDITION(INTEL_INFO(dev)->gen >= 7 && trace),

	TP_STRUCT__entry(
		__field(int, lp)
		__field(uint32_t, wm_lp)
		__field(uint32_t, wm_lp_spr)
		),

	TP_fast_assign(
		__entry->lp = lp;
		__entry->wm_lp = hw->wm_lp[lp - 1];
		__entry->wm_lp_spr = hw->wm_lp_spr[lp - 1];
		),

	TP_printk("LP%d: en=%s, lat=%u, fbc=%u, pri=%u, cur=%u, spr=%u",
		__entry->lp,
		__entry->wm_lp & WM1_LP_SR_EN ? "yes" : "no",
		(__entry->wm_lp & WM1_LP_LATENCY_MASK) >> WM1_LP_LATENCY_SHIFT,
		(__entry->wm_lp & WM1_LP_FBC_MASK) >> WM1_LP_FBC_SHIFT,
		(__entry->wm_lp & WM1_LP_SR_MASK) >> WM1_LP_SR_SHIFT,
		__entry->wm_lp & WM1_LP_CURSOR_MASK,
		__entry->wm_lp_spr)
);

/* pipe updates */

TRACE_EVENT(i915_pipe_update_start,
	    TP_PROTO(struct intel_crtc *crtc, u32 min, u32 max),
	    TP_ARGS(crtc, min, max),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     __field(u32, min)
			     __field(u32, max)
			     ),

	    TP_fast_assign(
			   __entry->pipe = crtc->pipe;
			   __entry->frame = crtc->base.dev->driver->get_vblank_counter(crtc->base.dev,
										       crtc->pipe);
			   __entry->scanline = intel_get_crtc_scanline(crtc);
			   __entry->min = min;
			   __entry->max = max;
			   ),

	    TP_printk("pipe %c, frame=%u, scanline=%u, min=%u, max=%u",
		      pipe_name(__entry->pipe), __entry->frame,
		       __entry->scanline, __entry->min, __entry->max)
);

TRACE_EVENT(i915_pipe_update_vblank_evaded,
	    TP_PROTO(struct intel_crtc *crtc, u32 min, u32 max, u32 frame),
	    TP_ARGS(crtc, min, max, frame),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     __field(u32, min)
			     __field(u32, max)
			     ),

	    TP_fast_assign(
			   __entry->pipe = crtc->pipe;
			   __entry->frame = frame;
			   __entry->scanline = intel_get_crtc_scanline(crtc);
			   __entry->min = min;
			   __entry->max = max;
			   ),

	    TP_printk("pipe %c, frame=%u, scanline=%u, min=%u, max=%u",
		      pipe_name(__entry->pipe), __entry->frame,
		       __entry->scanline, __entry->min, __entry->max)
);

TRACE_EVENT(i915_pipe_update_end,
	    TP_PROTO(struct intel_crtc *crtc, u32 frame),
	    TP_ARGS(crtc, frame),

	    TP_STRUCT__entry(
			     __field(enum pipe, pipe)
			     __field(u32, frame)
			     __field(u32, scanline)
			     ),

	    TP_fast_assign(
			   __entry->pipe = crtc->pipe;
			   __entry->frame = frame;
			   __entry->scanline = intel_get_crtc_scanline(crtc);
			   ),

	    TP_printk("pipe %c, frame=%u, scanline=%u",
		      pipe_name(__entry->pipe), __entry->frame,
		      __entry->scanline)
);

/* object tracking */

TRACE_EVENT(i915_gem_object_create,
	    TP_PROTO(struct drm_i915_gem_object *obj),
	    TP_ARGS(obj),

	    TP_STRUCT__entry(
			     __field(struct drm_i915_gem_object *, obj)
			     __field(u32, size)
			     ),

	    TP_fast_assign(
			   __entry->obj = obj;
			   __entry->size = obj->base.size;
			   ),

	    TP_printk("obj=%p, size=%u", __entry->obj, __entry->size)
);

TRACE_EVENT(i915_vma_bind,
	    TP_PROTO(struct i915_vma *vma, unsigned flags),
	    TP_ARGS(vma, flags),

	    TP_STRUCT__entry(
			     __field(struct drm_i915_gem_object *, obj)
			     __field(struct i915_address_space *, vm)
			     __field(u32, offset)
			     __field(u32, size)
			     __field(unsigned, flags)
			     ),

	    TP_fast_assign(
			   __entry->obj = vma->obj;
			   __entry->vm = vma->vm;
			   __entry->offset = vma->node.start;
			   __entry->size = vma->node.size;
			   __entry->flags = flags;
			   ),

	    TP_printk("obj=%p, offset=%08x size=%x%s vm=%p",
		      __entry->obj, __entry->offset, __entry->size,
		      __entry->flags & PIN_MAPPABLE ? ", mappable" : "",
		      __entry->vm)
);

TRACE_EVENT(i915_vma_unbind,
	    TP_PROTO(struct i915_vma *vma),
	    TP_ARGS(vma),

	    TP_STRUCT__entry(
			     __field(struct drm_i915_gem_object *, obj)
			     __field(struct i915_address_space *, vm)
			     __field(u32, offset)
			     __field(u32, size)
			     ),

	    TP_fast_assign(
			   __entry->obj = vma->obj;
			   __entry->vm = vma->vm;
			   __entry->offset = vma->node.start;
			   __entry->size = vma->node.size;
			   ),

	    TP_printk("obj=%p, offset=%08x size=%x vm=%p",
		      __entry->obj, __entry->offset, __entry->size, __entry->vm)
);

TRACE_EVENT(i915_gem_object_change_domain,
	    TP_PROTO(struct drm_i915_gem_object *obj, u32 old_read, u32 old_write),
	    TP_ARGS(obj, old_read, old_write),

	    TP_STRUCT__entry(
			     __field(struct drm_i915_gem_object *, obj)
			     __field(u32, read_domains)
			     __field(u32, write_domain)
			     ),

	    TP_fast_assign(
			   __entry->obj = obj;
			   __entry->read_domains = obj->base.read_domains | (old_read << 16);
			   __entry->write_domain = obj->base.write_domain | (old_write << 16);
			   ),

	    TP_printk("obj=%p, read=%02x=>%02x, write=%02x=>%02x",
		      __entry->obj,
		      __entry->read_domains >> 16,
		      __entry->read_domains & 0xffff,
		      __entry->write_domain >> 16,
		      __entry->write_domain & 0xffff)
);

TRACE_EVENT(i915_gem_object_pwrite,
	    TP_PROTO(struct drm_i915_gem_object *obj, u32 offset, u32 len),
	    TP_ARGS(obj, offset, len),

	    TP_STRUCT__entry(
			     __field(struct drm_i915_gem_object *, obj)
			     __field(u32, offset)
			     __field(u32, len)
			     ),

	    TP_fast_assign(
			   __entry->obj = obj;
			   __entry->offset = offset;
			   __entry->len = len;
			   ),

	    TP_printk("obj=%p, offset=%u, len=%u",
		      __entry->obj, __entry->offset, __entry->len)
);

TRACE_EVENT(i915_gem_object_pread,
	    TP_PROTO(struct drm_i915_gem_object *obj, u32 offset, u32 len),
	    TP_ARGS(obj, offset, len),

	    TP_STRUCT__entry(
			     __field(struct drm_i915_gem_object *, obj)
			     __field(u32, offset)
			     __field(u32, len)
			     ),

	    TP_fast_assign(
			   __entry->obj = obj;
			   __entry->offset = offset;
			   __entry->len = len;
			   ),

	    TP_printk("obj=%p, offset=%u, len=%u",
		      __entry->obj, __entry->offset, __entry->len)
);

TRACE_EVENT(i915_gem_object_fault,
	    TP_PROTO(struct drm_i915_gem_object *obj, u32 index, bool gtt, bool write),
	    TP_ARGS(obj, index, gtt, write),

	    TP_STRUCT__entry(
			     __field(struct drm_i915_gem_object *, obj)
			     __field(u32, index)
			     __field(bool, gtt)
			     __field(bool, write)
			     ),

	    TP_fast_assign(
			   __entry->obj = obj;
			   __entry->index = index;
			   __entry->gtt = gtt;
			   __entry->write = write;
			   ),

	    TP_printk("obj=%p, %s index=%u %s",
		      __entry->obj,
		      __entry->gtt ? "GTT" : "CPU",
		      __entry->index,
		      __entry->write ? ", writable" : "")
);

DECLARE_EVENT_CLASS(i915_gem_object,
	    TP_PROTO(struct drm_i915_gem_object *obj),
	    TP_ARGS(obj),

	    TP_STRUCT__entry(
			     __field(struct drm_i915_gem_object *, obj)
			     ),

	    TP_fast_assign(
			   __entry->obj = obj;
			   ),

	    TP_printk("obj=%p", __entry->obj)
);

DEFINE_EVENT(i915_gem_object, i915_gem_object_clflush,
	     TP_PROTO(struct drm_i915_gem_object *obj),
	     TP_ARGS(obj)
);

DEFINE_EVENT(i915_gem_object, i915_gem_object_destroy,
	    TP_PROTO(struct drm_i915_gem_object *obj),
	    TP_ARGS(obj)
);

TRACE_EVENT(i915_gem_evict,
	    TP_PROTO(struct drm_device *dev, u32 size, u32 align, unsigned flags),
	    TP_ARGS(dev, size, align, flags),

	    TP_STRUCT__entry(
			     __field(u32, dev)
			     __field(u32, size)
			     __field(u32, align)
			     __field(unsigned, flags)
			    ),

	    TP_fast_assign(
			   __entry->dev = dev->primary->index;
			   __entry->size = size;
			   __entry->align = align;
			   __entry->flags = flags;
			  ),

	    TP_printk("dev=%d, size=%d, align=%d %s",
		      __entry->dev, __entry->size, __entry->align,
		      __entry->flags & PIN_MAPPABLE ? ", mappable" : "")
);

TRACE_EVENT(i915_gem_evict_everything,
	    TP_PROTO(struct drm_device *dev),
	    TP_ARGS(dev),

	    TP_STRUCT__entry(
			     __field(u32, dev)
			    ),

	    TP_fast_assign(
			   __entry->dev = dev->primary->index;
			  ),

	    TP_printk("dev=%d", __entry->dev)
);

TRACE_EVENT(i915_gem_evict_vm,
	    TP_PROTO(struct i915_address_space *vm),
	    TP_ARGS(vm),

	    TP_STRUCT__entry(
			     __field(u32, dev)
			     __field(struct i915_address_space *, vm)
			    ),

	    TP_fast_assign(
			   __entry->dev = vm->dev->primary->index;
			   __entry->vm = vm;
			  ),

	    TP_printk("dev=%d, vm=%p", __entry->dev, __entry->vm)
);

TRACE_EVENT(i915_gem_ring_sync_to,
	    TP_PROTO(struct intel_engine_cs *from,
		     struct intel_engine_cs *to,
		     u32 seqno),
	    TP_ARGS(from, to, seqno),

	    TP_STRUCT__entry(
			     __field(u32, dev)
			     __field(u32, sync_from)
			     __field(u32, sync_to)
			     __field(u32, seqno)
			     ),

	    TP_fast_assign(
			   __entry->dev = from->dev->primary->index;
			   __entry->sync_from = from->id;
			   __entry->sync_to = to->id;
			   __entry->seqno = seqno;
			   ),

	    TP_printk("dev=%u, sync-from=%u, sync-to=%u, seqno=%u",
		      __entry->dev,
		      __entry->sync_from, __entry->sync_to,
		      __entry->seqno)
);

TRACE_EVENT(i915_gem_ring_dispatch,
	    TP_PROTO(struct intel_engine_cs *ring, u32 seqno, u32 flags),
	    TP_ARGS(ring, seqno, flags),

	    TP_STRUCT__entry(
			     __field(u32, dev)
			     __field(u32, ring)
			     __field(u32, seqno)
			     __field(u32, flags)
			     ),

	    TP_fast_assign(
			   __entry->dev = ring->dev->primary->index;
			   __entry->ring = ring->id;
			   __entry->seqno = seqno;
			   __entry->flags = flags;
			   i915_trace_irq_get(ring, seqno);
			   ),

	    TP_printk("dev=%u, ring=%u, seqno=%u, flags=%x",
		      __entry->dev, __entry->ring, __entry->seqno, __entry->flags)
);

TRACE_EVENT(i915_gem_ring_flush,
	    TP_PROTO(struct intel_engine_cs *ring, u32 invalidate, u32 flush),
	    TP_ARGS(ring, invalidate, flush),

	    TP_STRUCT__entry(
			     __field(u32, dev)
			     __field(u32, ring)
			     __field(u32, invalidate)
			     __field(u32, flush)
			     ),

	    TP_fast_assign(
			   __entry->dev = ring->dev->primary->index;
			   __entry->ring = ring->id;
			   __entry->invalidate = invalidate;
			   __entry->flush = flush;
			   ),

	    TP_printk("dev=%u, ring=%x, invalidate=%04x, flush=%04x",
		      __entry->dev, __entry->ring,
		      __entry->invalidate, __entry->flush)
);

DECLARE_EVENT_CLASS(i915_gem_request,
	    TP_PROTO(struct intel_engine_cs *ring, u32 seqno),
	    TP_ARGS(ring, seqno),

	    TP_STRUCT__entry(
			     __field(u32, dev)
			     __field(u32, ring)
			     __field(u32, seqno)
			     ),

	    TP_fast_assign(
			   __entry->dev = ring->dev->primary->index;
			   __entry->ring = ring->id;
			   __entry->seqno = seqno;
			   ),

	    TP_printk("dev=%u, ring=%u, seqno=%u",
		      __entry->dev, __entry->ring, __entry->seqno)
);

DEFINE_EVENT(i915_gem_request, i915_gem_request_add,
	    TP_PROTO(struct intel_engine_cs *ring, u32 seqno),
	    TP_ARGS(ring, seqno)
);

TRACE_EVENT(i915_gem_request_complete,
	    TP_PROTO(struct intel_engine_cs *ring),
	    TP_ARGS(ring),

	    TP_STRUCT__entry(
			     __field(u32, dev)
			     __field(u32, ring)
			     __field(u32, seqno)
			     ),

	    TP_fast_assign(
			   __entry->dev = ring->dev->primary->index;
			   __entry->ring = ring->id;
			   __entry->seqno = ring->get_seqno(ring, false);
			   ),

	    TP_printk("dev=%u, ring=%u, seqno=%u",
		      __entry->dev, __entry->ring, __entry->seqno)
);

DEFINE_EVENT(i915_gem_request, i915_gem_request_retire,
	    TP_PROTO(struct intel_engine_cs *ring, u32 seqno),
	    TP_ARGS(ring, seqno)
);

TRACE_EVENT(i915_gem_request_wait_begin,
	    TP_PROTO(struct intel_engine_cs *ring, u32 seqno),
	    TP_ARGS(ring, seqno),

	    TP_STRUCT__entry(
			     __field(u32, dev)
			     __field(u32, ring)
			     __field(u32, seqno)
			     __field(bool, blocking)
			     ),

	    /* NB: the blocking information is racy since mutex_is_locked
	     * doesn't check that the current thread holds the lock. The only
	     * other option would be to pass the boolean information of whether
	     * or not the class was blocking down through the stack which is
	     * less desirable.
	     */
	    TP_fast_assign(
			   __entry->dev = ring->dev->primary->index;
			   __entry->ring = ring->id;
			   __entry->seqno = seqno;
			   __entry->blocking = mutex_is_locked(&ring->dev->struct_mutex);
			   ),

	    TP_printk("dev=%u, ring=%u, seqno=%u, blocking=%s",
		      __entry->dev, __entry->ring, __entry->seqno,
		      __entry->blocking ?  "yes (NB)" : "no")
);

DEFINE_EVENT(i915_gem_request, i915_gem_request_wait_end,
	    TP_PROTO(struct intel_engine_cs *ring, u32 seqno),
	    TP_ARGS(ring, seqno)
);

DECLARE_EVENT_CLASS(i915_ring,
	    TP_PROTO(struct intel_engine_cs *ring),
	    TP_ARGS(ring),

	    TP_STRUCT__entry(
			     __field(u32, dev)
			     __field(u32, ring)
			     ),

	    TP_fast_assign(
			   __entry->dev = ring->dev->primary->index;
			   __entry->ring = ring->id;
			   ),

	    TP_printk("dev=%u, ring=%u", __entry->dev, __entry->ring)
);

DEFINE_EVENT(i915_ring, i915_ring_wait_begin,
	    TP_PROTO(struct intel_engine_cs *ring),
	    TP_ARGS(ring)
);

DEFINE_EVENT(i915_ring, i915_ring_wait_end,
	    TP_PROTO(struct intel_engine_cs *ring),
	    TP_ARGS(ring)
);

TRACE_EVENT(i915_flip_request,
	    TP_PROTO(int plane, struct drm_i915_gem_object *obj),

	    TP_ARGS(plane, obj),

	    TP_STRUCT__entry(
		    __field(int, plane)
		    __field(struct drm_i915_gem_object *, obj)
		    ),

	    TP_fast_assign(
		    __entry->plane = plane;
		    __entry->obj = obj;
		    ),

	    TP_printk("plane=%d, obj=%p", __entry->plane, __entry->obj)
);

TRACE_EVENT(i915_flip_complete,
	    TP_PROTO(int plane, struct drm_i915_gem_object *obj),

	    TP_ARGS(plane, obj),

	    TP_STRUCT__entry(
		    __field(int, plane)
		    __field(struct drm_i915_gem_object *, obj)
		    ),

	    TP_fast_assign(
		    __entry->plane = plane;
		    __entry->obj = obj;
		    ),

	    TP_printk("plane=%d, obj=%p", __entry->plane, __entry->obj)
);

TRACE_EVENT_CONDITION(i915_reg_rw,
	TP_PROTO(bool write, u32 reg, u64 val, int len, bool trace),

	TP_ARGS(write, reg, val, len, trace),

	TP_CONDITION(trace),

	TP_STRUCT__entry(
		__field(u64, val)
		__field(u32, reg)
		__field(u16, write)
		__field(u16, len)
		),

	TP_fast_assign(
		__entry->val = (u64)val;
		__entry->reg = reg;
		__entry->write = write;
		__entry->len = len;
		),

	TP_printk("%s reg=0x%x, len=%d, val=(0x%x, 0x%x)",
		__entry->write ? "write" : "read",
		__entry->reg, __entry->len,
		(u32)(__entry->val & 0xffffffff),
		(u32)(__entry->val >> 32))
);

TRACE_EVENT(intel_gpu_freq_change,
	    TP_PROTO(u32 freq),
	    TP_ARGS(freq),

	    TP_STRUCT__entry(
			     __field(u32, freq)
			     ),

	    TP_fast_assign(
			   __entry->freq = freq;
			   ),

	    TP_printk("new_freq=%u", __entry->freq)
);

#endif /* _I915_TRACE_H_ */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#include <trace/define_trace.h>
