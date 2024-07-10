// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 *
 */

#include "i915_drv.h"
#include "i915_irq.h"
#include "i915_reg.h"
#include "intel_crtc.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_dsb.h"
#include "intel_dsb_buffer.h"
#include "intel_dsb_regs.h"
#include "intel_pipe_crc_regs.h"
#include "intel_vblank.h"
#include "intel_vrr.h"
#include "skl_universal_plane_regs.h"
#include "skl_watermark.h"

#include "intel_color_regs.h"

#define CACHELINE_BYTES 64

struct intel_dsb {
	enum intel_dsb_id id;

	struct intel_dsb_buffer dsb_buf;
	struct intel_crtc *crtc;

	/*
	 * maximum number of dwords the buffer will hold.
	 */
	unsigned int size;

	/*
	 * free_pos will point the first free dword and
	 * help in calculating tail of command buffer.
	 */
	unsigned int free_pos;

	/*
	 * ins_start_offset will help to store start dword of the dsb
	 * instuction and help in identifying the batch of auto-increment
	 * register.
	 */
	unsigned int ins_start_offset;

	u32 chicken;
	int dewake_scanline;
};

/**
 * DOC: DSB
 *
 * A DSB (Display State Buffer) is a queue of MMIO instructions in the memory
 * which can be offloaded to DSB HW in Display Controller. DSB HW is a DMA
 * engine that can be programmed to download the DSB from memory.
 * It allows driver to batch submit display HW programming. This helps to
 * reduce loading time and CPU activity, thereby making the context switch
 * faster. DSB Support added from Gen12 Intel graphics based platform.
 *
 * DSB's can access only the pipe, plane, and transcoder Data Island Packet
 * registers.
 *
 * DSB HW can support only register writes (both indexed and direct MMIO
 * writes). There are no registers reads possible with DSB HW engine.
 */

/* DSB opcodes. */
#define DSB_OPCODE_SHIFT		24
#define DSB_OPCODE_NOOP			0x0
#define DSB_OPCODE_MMIO_WRITE		0x1
#define   DSB_BYTE_EN			0xf
#define   DSB_BYTE_EN_SHIFT		20
#define   DSB_REG_VALUE_MASK		0xfffff
#define DSB_OPCODE_WAIT_USEC		0x2
#define DSB_OPCODE_WAIT_SCANLINE	0x3
#define DSB_OPCODE_WAIT_VBLANKS		0x4
#define DSB_OPCODE_WAIT_DSL_IN		0x5
#define DSB_OPCODE_WAIT_DSL_OUT		0x6
#define   DSB_SCANLINE_UPPER_SHIFT	20
#define   DSB_SCANLINE_LOWER_SHIFT	0
#define DSB_OPCODE_INTERRUPT		0x7
#define DSB_OPCODE_INDEXED_WRITE	0x9
/* see DSB_REG_VALUE_MASK */
#define DSB_OPCODE_POLL			0xA
/* see DSB_REG_VALUE_MASK */

static bool assert_dsb_has_room(struct intel_dsb *dsb)
{
	struct intel_crtc *crtc = dsb->crtc;
	struct intel_display *display = to_intel_display(crtc->base.dev);

	/* each instruction is 2 dwords */
	return !drm_WARN(display->drm, dsb->free_pos > dsb->size - 2,
			 "[CRTC:%d:%s] DSB %d buffer overflow\n",
			 crtc->base.base.id, crtc->base.name, dsb->id);
}

static void intel_dsb_dump(struct intel_dsb *dsb)
{
	struct intel_crtc *crtc = dsb->crtc;
	struct intel_display *display = to_intel_display(crtc->base.dev);
	int i;

	drm_dbg_kms(display->drm, "[CRTC:%d:%s] DSB %d commands {\n",
		    crtc->base.base.id, crtc->base.name, dsb->id);
	for (i = 0; i < ALIGN(dsb->free_pos, 64 / 4); i += 4)
		drm_dbg_kms(display->drm,
			    " 0x%08x: 0x%08x 0x%08x 0x%08x 0x%08x\n", i * 4,
			    intel_dsb_buffer_read(&dsb->dsb_buf, i),
			    intel_dsb_buffer_read(&dsb->dsb_buf, i + 1),
			    intel_dsb_buffer_read(&dsb->dsb_buf, i + 2),
			    intel_dsb_buffer_read(&dsb->dsb_buf, i + 3));
	drm_dbg_kms(display->drm, "}\n");
}

static bool is_dsb_busy(struct intel_display *display, enum pipe pipe,
			enum intel_dsb_id dsb_id)
{
	return intel_de_read_fw(display, DSB_CTRL(pipe, dsb_id)) & DSB_STATUS_BUSY;
}

static void intel_dsb_emit(struct intel_dsb *dsb, u32 ldw, u32 udw)
{
	if (!assert_dsb_has_room(dsb))
		return;

	/* Every instruction should be 8 byte aligned. */
	dsb->free_pos = ALIGN(dsb->free_pos, 2);

	dsb->ins_start_offset = dsb->free_pos;

	intel_dsb_buffer_write(&dsb->dsb_buf, dsb->free_pos++, ldw);
	intel_dsb_buffer_write(&dsb->dsb_buf, dsb->free_pos++, udw);
}

static bool intel_dsb_prev_ins_is_write(struct intel_dsb *dsb,
					u32 opcode, i915_reg_t reg)
{
	u32 prev_opcode, prev_reg;

	/*
	 * Nothing emitted yet? Must check before looking
	 * at the actual data since i915_gem_object_create_internal()
	 * does *not* give you zeroed memory!
	 */
	if (dsb->free_pos == 0)
		return false;

	prev_opcode = intel_dsb_buffer_read(&dsb->dsb_buf,
					    dsb->ins_start_offset + 1) & ~DSB_REG_VALUE_MASK;
	prev_reg =  intel_dsb_buffer_read(&dsb->dsb_buf,
					  dsb->ins_start_offset + 1) & DSB_REG_VALUE_MASK;

	return prev_opcode == opcode && prev_reg == i915_mmio_reg_offset(reg);
}

static bool intel_dsb_prev_ins_is_mmio_write(struct intel_dsb *dsb, i915_reg_t reg)
{
	/* only full byte-enables can be converted to indexed writes */
	return intel_dsb_prev_ins_is_write(dsb,
					   DSB_OPCODE_MMIO_WRITE << DSB_OPCODE_SHIFT |
					   DSB_BYTE_EN << DSB_BYTE_EN_SHIFT,
					   reg);
}

static bool intel_dsb_prev_ins_is_indexed_write(struct intel_dsb *dsb, i915_reg_t reg)
{
	return intel_dsb_prev_ins_is_write(dsb,
					   DSB_OPCODE_INDEXED_WRITE << DSB_OPCODE_SHIFT,
					   reg);
}

/**
 * intel_dsb_reg_write() - Emit register wriite to the DSB context
 * @dsb: DSB context
 * @reg: register address.
 * @val: value.
 *
 * This function is used for writing register-value pair in command
 * buffer of DSB.
 */
void intel_dsb_reg_write(struct intel_dsb *dsb,
			 i915_reg_t reg, u32 val)
{
	u32 old_val;

	/*
	 * For example the buffer will look like below for 3 dwords for auto
	 * increment register:
	 * +--------------------------------------------------------+
	 * | size = 3 | offset &| value1 | value2 | value3 | zero   |
	 * |          | opcode  |        |        |        |        |
	 * +--------------------------------------------------------+
	 * +          +         +        +        +        +        +
	 * 0          4         8        12       16       20       24
	 * Byte
	 *
	 * As every instruction is 8 byte aligned the index of dsb instruction
	 * will start always from even number while dealing with u32 array. If
	 * we are writing odd no of dwords, Zeros will be added in the end for
	 * padding.
	 */
	if (!intel_dsb_prev_ins_is_mmio_write(dsb, reg) &&
	    !intel_dsb_prev_ins_is_indexed_write(dsb, reg)) {
		intel_dsb_emit(dsb, val,
			       (DSB_OPCODE_MMIO_WRITE << DSB_OPCODE_SHIFT) |
			       (DSB_BYTE_EN << DSB_BYTE_EN_SHIFT) |
			       i915_mmio_reg_offset(reg));
	} else {
		if (!assert_dsb_has_room(dsb))
			return;

		/* convert to indexed write? */
		if (intel_dsb_prev_ins_is_mmio_write(dsb, reg)) {
			u32 prev_val = intel_dsb_buffer_read(&dsb->dsb_buf,
							     dsb->ins_start_offset + 0);

			intel_dsb_buffer_write(&dsb->dsb_buf,
					       dsb->ins_start_offset + 0, 1); /* count */
			intel_dsb_buffer_write(&dsb->dsb_buf, dsb->ins_start_offset + 1,
					       (DSB_OPCODE_INDEXED_WRITE << DSB_OPCODE_SHIFT) |
					       i915_mmio_reg_offset(reg));
			intel_dsb_buffer_write(&dsb->dsb_buf, dsb->ins_start_offset + 2, prev_val);

			dsb->free_pos++;
		}

		intel_dsb_buffer_write(&dsb->dsb_buf, dsb->free_pos++, val);
		/* Update the count */
		old_val = intel_dsb_buffer_read(&dsb->dsb_buf, dsb->ins_start_offset);
		intel_dsb_buffer_write(&dsb->dsb_buf, dsb->ins_start_offset, old_val + 1);

		/* if number of data words is odd, then the last dword should be 0.*/
		if (dsb->free_pos & 0x1)
			intel_dsb_buffer_write(&dsb->dsb_buf, dsb->free_pos, 0);
	}
}

static u32 intel_dsb_mask_to_byte_en(u32 mask)
{
	return (!!(mask & 0xff000000) << 3 |
		!!(mask & 0x00ff0000) << 2 |
		!!(mask & 0x0000ff00) << 1 |
		!!(mask & 0x000000ff) << 0);
}

/* Note: mask implemented via byte enables! */
void intel_dsb_reg_write_masked(struct intel_dsb *dsb,
				i915_reg_t reg, u32 mask, u32 val)
{
	intel_dsb_emit(dsb, val,
		       (DSB_OPCODE_MMIO_WRITE << DSB_OPCODE_SHIFT) |
		       (intel_dsb_mask_to_byte_en(mask) << DSB_BYTE_EN_SHIFT) |
		       i915_mmio_reg_offset(reg));
}

void intel_dsb_noop(struct intel_dsb *dsb, int count)
{
	int i;

	for (i = 0; i < count; i++)
		intel_dsb_emit(dsb, 0,
			       DSB_OPCODE_NOOP << DSB_OPCODE_SHIFT);
}

void intel_dsb_nonpost_start(struct intel_dsb *dsb)
{
	struct intel_crtc *crtc = dsb->crtc;
	enum pipe pipe = crtc->pipe;

	intel_dsb_reg_write_masked(dsb, DSB_CTRL(pipe, dsb->id),
				   DSB_NON_POSTED, DSB_NON_POSTED);
	intel_dsb_noop(dsb, 4);
}

void intel_dsb_nonpost_end(struct intel_dsb *dsb)
{
	struct intel_crtc *crtc = dsb->crtc;
	enum pipe pipe = crtc->pipe;

	intel_dsb_reg_write_masked(dsb, DSB_CTRL(pipe, dsb->id),
				   DSB_NON_POSTED, 0);
	intel_dsb_noop(dsb, 4);
}

void intel_dsb_wait_usec(struct intel_dsb *dsb, int count)
{
	intel_dsb_emit(dsb, count,
		       DSB_OPCODE_WAIT_USEC << DSB_OPCODE_SHIFT);
}

void intel_dsb_wait_vblanks(struct intel_dsb *dsb, int count)
{
	intel_dsb_emit(dsb, count,
		       DSB_OPCODE_WAIT_VBLANKS << DSB_OPCODE_SHIFT);
}

static void intel_dsb_emit_poll(struct intel_dsb *dsb,
				i915_reg_t reg, u32 mask, u32 val,
				int wait, int count)
{
	struct intel_crtc *crtc = dsb->crtc;
	enum pipe pipe = crtc->pipe;

	intel_dsb_reg_write(dsb, DSB_POLLMASK(pipe, dsb->id), mask);
	intel_dsb_reg_write(dsb, DSB_POLLFUNC(pipe, dsb->id),
			    DSB_POLL_ENABLE |
			    DSB_POLL_WAIT(wait) | DSB_POLL_COUNT(count));

	intel_dsb_noop(dsb, 5);

	intel_dsb_emit(dsb, val,
		       (DSB_OPCODE_POLL << DSB_OPCODE_SHIFT) |
		       i915_mmio_reg_offset(reg));
}

void intel_dsb_poll(struct intel_dsb *dsb,
		    i915_reg_t reg, u32 mask, u32 val)
{
	intel_dsb_emit_poll(dsb, reg, mask, val, 2, 50);
}

void intel_dsb_interrupt(struct intel_dsb *dsb)
{
	intel_dsb_emit(dsb, 0,
		       DSB_OPCODE_INTERRUPT << DSB_OPCODE_SHIFT);
}

static bool pre_commit_is_vrr_active(struct intel_atomic_state *state,
				     struct intel_crtc *crtc)
{
	const struct intel_crtc_state *old_crtc_state =
		intel_atomic_get_old_crtc_state(state, crtc);
	const struct intel_crtc_state *new_crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);

	/* VRR will be enabled afterwards, if necessary */
	if (intel_crtc_needs_modeset(new_crtc_state))
		return false;

	/* VRR will have been disabled during intel_pre_plane_update() */
	return old_crtc_state->vrr.enable && !intel_crtc_vrr_disabling(state, crtc);
}

static const struct intel_crtc_state *pre_commit_crtc_state(struct intel_atomic_state *state,
							    struct intel_crtc *crtc)
{
	const struct intel_crtc_state *old_crtc_state =
		intel_atomic_get_old_crtc_state(state, crtc);
	const struct intel_crtc_state *new_crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);

	/*
	 * During fastsets/etc. the transcoder is still
	 * running with the old timings at this point.
	 */
	if (intel_crtc_needs_modeset(new_crtc_state))
		return new_crtc_state;
	else
		return old_crtc_state;
}

static int dsb_scanline_to_hw(struct intel_atomic_state *state,
			      struct intel_crtc *crtc, int scanline)
{
	const struct intel_crtc_state *crtc_state =
		pre_commit_crtc_state(state, crtc);
	int vtotal;

	if (pre_commit_is_vrr_active(state, crtc))
		vtotal = crtc_state->vrr.vmax;
	else
		vtotal = intel_mode_vtotal(&crtc_state->hw.adjusted_mode);

	return (scanline + vtotal - intel_crtc_scanline_offset(crtc_state)) % vtotal;
}

static int dsb_vblank_start(struct intel_atomic_state *state,
			    struct intel_crtc *crtc)
{
	const struct intel_crtc_state *crtc_state =
		pre_commit_crtc_state(state, crtc);
	int vblank_start;

	if (pre_commit_is_vrr_active(state, crtc))
		vblank_start = intel_vrr_vmin_vblank_start(crtc_state);
	else
		vblank_start = intel_mode_vblank_start(&crtc_state->hw.adjusted_mode);

	return dsb_scanline_to_hw(state, crtc, vblank_start);
}

static int dsb_dewake_scanline(struct intel_atomic_state *state,
			       struct intel_crtc *crtc)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	unsigned int latency = skl_watermark_max_latency(i915, 0);
	const struct intel_crtc_state *crtc_state =
		pre_commit_crtc_state(state, crtc);
	int vblank_start, dewake_scanline;

	if (pre_commit_is_vrr_active(state, crtc))
		vblank_start = intel_vrr_vmin_vblank_start(crtc_state);
	else
		vblank_start = intel_mode_vblank_start(&crtc_state->hw.adjusted_mode);

	dewake_scanline = vblank_start -
		intel_usecs_to_scanlines(&crtc_state->hw.adjusted_mode, latency);

	return dsb_scanline_to_hw(state, crtc, dewake_scanline);
}

static u32 dsb_chicken(struct intel_atomic_state *state,
		       struct intel_crtc *crtc)
{
	if (pre_commit_is_vrr_active(state, crtc))
		return DSB_SKIP_WAITS_EN |
			DSB_CTRL_WAIT_SAFE_WINDOW |
			DSB_CTRL_NO_WAIT_VBLANK |
			DSB_INST_WAIT_SAFE_WINDOW |
			DSB_INST_NO_WAIT_VBLANK;
	else
		return DSB_SKIP_WAITS_EN;
}

void intel_dsb_wait_scanline(struct intel_atomic_state *state,
			     struct intel_dsb *dsb,
			     int scanline)
{
	struct intel_crtc *crtc = dsb->crtc;

	scanline = dsb_scanline_to_hw(state, crtc, scanline);

	intel_dsb_emit(dsb, scanline,
		       DSB_OPCODE_WAIT_SCANLINE << DSB_OPCODE_SHIFT);
}

static void intel_dsb_emit_wait_dsl(struct intel_dsb *dsb,
				    u32 opcode, int lower, int upper)
{
	u64 window;

	window = ((u64)upper << DSB_SCANLINE_UPPER_SHIFT) |
		((u64)lower << DSB_SCANLINE_LOWER_SHIFT);

	DRM_DEBUG_KMS("%d %d -> 0x%08llx\n", lower, upper, window);

	DRM_DEBUG_KMS("%x / %x / %x\n", opcode, upper_32_bits(window), lower_32_bits(window));

	intel_dsb_emit(dsb, lower_32_bits(window),
		       (opcode << DSB_OPCODE_SHIFT) |
		       upper_32_bits(window));
}

static void intel_dsb_wait_dsl(struct intel_atomic_state *state,
			       struct intel_dsb *dsb,
			       int lower_in, int upper_in,
			       int lower_out, int upper_out)
{
	struct intel_crtc *crtc = dsb->crtc;

	lower_in = dsb_scanline_to_hw(state, crtc, lower_in);
	upper_in = dsb_scanline_to_hw(state, crtc, upper_in);

	lower_out = dsb_scanline_to_hw(state, crtc, lower_out);
	upper_out = dsb_scanline_to_hw(state, crtc, upper_out);

	drm_dbg_kms(dsb->crtc->base.dev, "dsl in range %d %d\n", lower_in, upper_in);
	drm_dbg_kms(dsb->crtc->base.dev, "dsl out range %d %d\n", lower_out, upper_out);

	if (upper_in >= lower_in)
	{
		drm_dbg_kms(dsb->crtc->base.dev, "dsl in\n");
		intel_dsb_emit_wait_dsl(dsb, DSB_OPCODE_WAIT_DSL_IN,
					lower_in, upper_in);
	}
	else if (upper_out >= lower_out)
	{
		drm_dbg_kms(dsb->crtc->base.dev, "dsl out\n");
		intel_dsb_emit_wait_dsl(dsb, DSB_OPCODE_WAIT_DSL_OUT,
					lower_out, upper_out);
	}
	else
		drm_WARN_ON(crtc->base.dev, 1); /* assert_dsl_ok() should have caught it already */
}

static void assert_dsl_ok(struct intel_atomic_state *state,
			  struct intel_dsb *dsb,
			  int start, int end)
{
	struct intel_crtc *crtc = dsb->crtc;
	const struct intel_crtc_state *crtc_state =
		pre_commit_crtc_state(state, crtc);
	int vtotal;

	if (pre_commit_is_vrr_active(state, crtc))
		vtotal = crtc_state->vrr.vmax;
	else
		vtotal = intel_mode_vtotal(&crtc_state->hw.adjusted_mode);

	/*
	 * Waiting for the entire frame doesn't make sense,
	 * (IN==don't wait, OUT=wait forever).
	 */
	drm_WARN(crtc->base.dev, (end - start + vtotal) % vtotal == vtotal - 1,
		 "[CRTC:%d:%s] DSB %d bad scanline window wait: %d-%d (vt=%d)\n",
		 crtc->base.base.id, crtc->base.name, dsb->id,
		 start, end, vtotal);
}

void intel_dsb_wait_scanline_in(struct intel_atomic_state *state,
				struct intel_dsb *dsb,
				int start, int end)
{
	assert_dsl_ok(state, dsb, start, end);

	drm_dbg_kms(dsb->crtc->base.dev, "wait dsl in %d %d\n", start, end);

	intel_dsb_wait_dsl(state, dsb,
			   start, end,
			   end + 1, start - 1);
}

void intel_dsb_wait_scanline_out(struct intel_atomic_state *state,
				 struct intel_dsb *dsb,
				 int start, int end)
{
	assert_dsl_ok(state, dsb, start, end);

	drm_dbg_kms(dsb->crtc->base.dev, "wait dsl out %d %d\n", start, end);

	intel_dsb_wait_dsl(state, dsb,
			   end + 1, start - 1,
			   start, end);
}

static void intel_dsb_align_tail(struct intel_dsb *dsb)
{
	u32 aligned_tail, tail;

	tail = dsb->free_pos * 4;
	aligned_tail = ALIGN(tail, CACHELINE_BYTES);

	if (aligned_tail > tail)
		intel_dsb_buffer_memset(&dsb->dsb_buf, dsb->free_pos, 0,
					aligned_tail - tail);

	dsb->free_pos = aligned_tail / 4;
}

void intel_dsb_finish(struct intel_dsb *dsb)
{
	struct intel_crtc *crtc = dsb->crtc;

	/*
	 * DSB_FORCE_DEWAKE remains active even after DSB is
	 * disabled, so make sure to clear it (if set during
	 * intel_dsb_commit()). And clear DSB_ENABLE_DEWAKE as
	 * well for good measure.
	 */
	intel_dsb_reg_write(dsb, DSB_PMCTRL(crtc->pipe, dsb->id), 0);
	intel_dsb_reg_write_masked(dsb, DSB_PMCTRL_2(crtc->pipe, dsb->id),
				   DSB_FORCE_DEWAKE, 0);

	intel_dsb_align_tail(dsb);

	intel_dsb_buffer_flush_map(&dsb->dsb_buf);
}

static u32 dsb_error_int_status(struct intel_display *display)
{
	struct drm_i915_private *i915 = to_i915(display->drm);
	u32 errors;

	errors = DSB_GTT_FAULT_INT_STATUS |
		DSB_RSPTIMEOUT_INT_STATUS |
		DSB_POLL_ERR_INT_STATUS;

	/*
	 * All the non-existing status bits operate as
	 * normal r/w bits, so any attempt to clear them
	 * will just end up setting them. Never do that so
	 * we won't mistake them for actual error interrupts.
	 */
	if (DISPLAY_VER(i915) >= 14)
		errors |= DSB_ATS_FAULT_INT_STATUS;

	return errors;
}

static u32 dsb_error_int_en(struct intel_display *display)
{
	struct drm_i915_private *i915 = to_i915(display->drm);
	u32 errors;

	errors = DSB_GTT_FAULT_INT_EN |
		DSB_RSPTIMEOUT_INT_EN |
		DSB_POLL_ERR_INT_EN;

	if (DISPLAY_VER(i915) >= 14)
		errors |= DSB_ATS_FAULT_INT_EN;

	return errors;
}

static void _intel_dsb_chain(struct intel_atomic_state *state,
			     struct intel_dsb *dsb,
			     const struct intel_dsb *chained_dsb,
			     u32 ctrl,
			     int vblank_start,
			     int dewake_scanline,
			     int repeat_count)
{
	struct intel_display *display = to_intel_display(state->base.dev);
	struct intel_crtc *crtc = dsb->crtc;
	enum pipe pipe = crtc->pipe;
	u32 tail;

	if (drm_WARN_ON(display->drm, dsb->id == chained_dsb->id))
		return;

	tail = chained_dsb->free_pos * 4;
	if (drm_WARN_ON(display->drm, !IS_ALIGNED(tail, CACHELINE_BYTES)))
		return;

	intel_dsb_reg_write(dsb, DSB_CTRL(pipe, chained_dsb->id),
			    ctrl | DSB_ENABLE);

	/* FIXME is VRR on or off at this point? */
	intel_dsb_reg_write(dsb, DSB_CHICKEN(pipe, chained_dsb->id),
			    dsb_chicken(state, crtc));

	intel_dsb_reg_write(dsb, DSB_BUFRPT_CNT(pipe, chained_dsb->id),
			    repeat_count);

	intel_dsb_reg_write(dsb, DSB_HEAD(pipe, chained_dsb->id),
			    intel_dsb_buffer_ggtt_offset(&chained_dsb->dsb_buf));

	intel_dsb_reg_write(dsb, DSB_INTERRUPT(pipe, chained_dsb->id),
			    dsb_error_int_status(display) | DSB_PROG_INT_STATUS |
			    dsb_error_int_en(display) | DSB_PROG_INT_EN);

	if (dewake_scanline >= 0)
		intel_dsb_reg_write(dsb, DSB_PMCTRL(pipe, chained_dsb->id),
				    DSB_ENABLE_DEWAKE |
				    DSB_SCANLINE_FOR_DEWAKE(dewake_scanline));

	intel_dsb_reg_write(dsb, DSB_TAIL(pipe, chained_dsb->id),
			    intel_dsb_buffer_ggtt_offset(&chained_dsb->dsb_buf) + tail);

	if (dewake_scanline >= 0) {
		/*
		 * Keep DEwake alive via the first DSB, in
		 * case we're already past dewake_scanline
		 * (and thus DSB_ENABLE_DEWAKE won't do its job).
		 */
		intel_dsb_reg_write_masked(dsb, DSB_PMCTRL_2(pipe, dsb->id),
					   DSB_FORCE_DEWAKE, DSB_FORCE_DEWAKE);

		intel_dsb_wait_scanline_out(state, dsb,
					    dewake_scanline, vblank_start);
	}
}

void intel_dsb_chain(struct intel_atomic_state *state,
		     struct intel_dsb *dsb,
		     const struct intel_dsb *chained_dsb,
		     bool wait_for_vblank)
{
	_intel_dsb_chain(state, dsb, chained_dsb,
			 wait_for_vblank ? DSB_WAIT_FOR_VBLANK : 0,
			 wait_for_vblank ? dsb_vblank_start(state, dsb->crtc) : -1,
			 wait_for_vblank ? dsb_dewake_scanline(state, dsb->crtc) : -1,
			 0);
}

static void _intel_dsb_commit(struct intel_dsb *dsb, u32 ctrl,
			      int dewake_scanline, int repeat_count)
{
	struct intel_crtc *crtc = dsb->crtc;
	struct intel_display *display = to_intel_display(crtc->base.dev);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;
	u32 tail;

	tail = dsb->free_pos * 4;
	if (drm_WARN_ON(display->drm, !IS_ALIGNED(tail, CACHELINE_BYTES)))
		return;

	if (is_dsb_busy(display, pipe, dsb->id)) {
		drm_err(display->drm, "[CRTC:%d:%s] DSB %d is busy\n",
			crtc->base.base.id, crtc->base.name, dsb->id);
		return;
	}

	intel_de_write_fw(display, DSB_CTRL(pipe, dsb->id),
			  ctrl | DSB_ENABLE);

	intel_de_write_fw(display, DSB_CHICKEN(pipe, dsb->id),
			  dsb->chicken);

	intel_de_write_fw(display, DSB_BUFRPT_CNT(pipe, dsb->id),
			  repeat_count);

	intel_de_write_fw(display, DSB_INTERRUPT(pipe, dsb->id),
			  dsb_error_int_status(display) | DSB_PROG_INT_STATUS |
			  dsb_error_int_en(display) | DSB_PROG_INT_EN);

	intel_de_write_fw(display, DSB_HEAD(pipe, dsb->id),
			  intel_dsb_buffer_ggtt_offset(&dsb->dsb_buf));

	if (dewake_scanline >= 0) {
		int diff, position;

		intel_de_write_fw(display, DSB_PMCTRL(pipe, dsb->id),
				  DSB_ENABLE_DEWAKE |
				  DSB_SCANLINE_FOR_DEWAKE(dewake_scanline));

		/*
		 * Force DEwake immediately if we're already past
		 * or close to racing past the target scanline.
		 */
		position = intel_de_read_fw(display, PIPEDSL(dev_priv, pipe)) & PIPEDSL_LINE_MASK;
		diff = dewake_scanline - position;

		intel_de_write_fw(display, DSB_PMCTRL_2(pipe, dsb->id),
				  (diff >= 0 && diff < 5 ? DSB_FORCE_DEWAKE : 0) |
				  DSB_BLOCK_DEWAKE_EXTENSION);
	}

#if 0
	drm_dbg_kms(display->drm, "DSB %d head 0x%x, tail 0x%x\n", dsb->id,
		    intel_dsb_buffer_ggtt_offset(&dsb->dsb_buf),
		    intel_dsb_buffer_ggtt_offset(&dsb->dsb_buf) + tail);
#endif

	intel_de_write_fw(display, DSB_TAIL(pipe, dsb->id),
			  intel_dsb_buffer_ggtt_offset(&dsb->dsb_buf) + tail);

}

/**
 * intel_dsb_commit() - Trigger workload execution of DSB.
 * @dsb: DSB context
 * @wait_for_vblank: wait for vblank before executing
 *
 * This function is used to do actual write to hardware using DSB.
 */
void intel_dsb_commit(struct intel_dsb *dsb,
		      bool wait_for_vblank)
{
	_intel_dsb_commit(dsb,
			  wait_for_vblank ? DSB_WAIT_FOR_VBLANK : 0,
			  wait_for_vblank ? dsb->dewake_scanline : -1,
			  0);
}

static void _intel_dsb_wait_inf(struct intel_dsb *dsb)
{
	struct intel_crtc *crtc = dsb->crtc;
	struct intel_display *display = to_intel_display(crtc->base.dev);
	enum pipe pipe = crtc->pipe;

	while (wait_for(!is_dsb_busy(display, pipe, dsb->id), 2000))
		;
}

void intel_dsb_wait(struct intel_dsb *dsb)
{
	struct intel_crtc *crtc = dsb->crtc;
	struct intel_display *display = to_intel_display(crtc->base.dev);
	enum pipe pipe = crtc->pipe;

	if (wait_for(!is_dsb_busy(display, pipe, dsb->id), 1)) {
		u32 offset = intel_dsb_buffer_ggtt_offset(&dsb->dsb_buf);

		intel_de_write_fw(display, DSB_CTRL(pipe, dsb->id),
				  DSB_ENABLE | DSB_HALT);

		drm_err(display->drm,
			"[CRTC:%d:%s] DSB %d timed out waiting for idle (current head=0x%x, head=0x%x, tail=0x%x, ctrl=0x%08x, status=0x%08x)\n",
			crtc->base.base.id, crtc->base.name, dsb->id,
			intel_de_read_fw(display, DSB_CURRENT_HEAD(pipe, dsb->id)) - offset,
			intel_de_read_fw(display, DSB_HEAD(pipe, dsb->id)) - offset,
			intel_de_read_fw(display, DSB_TAIL(pipe, dsb->id)) - offset,
			intel_de_read_fw(display, DSB_CTRL(pipe, dsb->id)),
			intel_de_read_fw(display, DSB_STATUS(pipe, dsb->id)));

		intel_dsb_dump(dsb);
	}

	/* Attempt to reset it */
	dsb->free_pos = 0;
	dsb->ins_start_offset = 0;
	intel_de_write_fw(display, DSB_CTRL(pipe, dsb->id), 0);

	intel_de_write_fw(display, DSB_INTERRUPT(pipe, dsb->id),
			  dsb_error_int_status(display) | DSB_PROG_INT_STATUS);
}

/**
 * intel_dsb_prepare() - Allocate, pin and map the DSB command buffer.
 * @state: the atomic state
 * @crtc: the CRTC
 * @dsb_id: the DSB engine to use
 * @max_cmds: number of commands we need to fit into command buffer
 *
 * This function prepare the command buffer which is used to store dsb
 * instructions with data.
 *
 * Returns:
 * DSB context, NULL on failure
 */
struct intel_dsb *intel_dsb_prepare(struct intel_atomic_state *state,
				    struct intel_crtc *crtc,
				    enum intel_dsb_id dsb_id,
				    unsigned int max_cmds)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	intel_wakeref_t wakeref;
	struct intel_dsb *dsb;
	unsigned int size;

	if (!HAS_DSB(i915))
		return NULL;

	if (!i915->display.params.enable_dsb)
		return NULL;

	/* TODO: DSB is broken in Xe KMD, so disabling it until fixed */
	//if (!IS_ENABLED(I915))
	//return NULL;

	dsb = kzalloc(sizeof(*dsb), GFP_KERNEL);
	if (!dsb)
		goto out;

	wakeref = intel_runtime_pm_get(&i915->runtime_pm);

	/* ~1 qword per instruction, full cachelines */
	size = ALIGN(max_cmds * 8, CACHELINE_BYTES);

	if (!intel_dsb_buffer_create(crtc, &dsb->dsb_buf, size))
		goto out_put_rpm;

	intel_runtime_pm_put(&i915->runtime_pm, wakeref);

	dsb->id = dsb_id;
	dsb->crtc = crtc;
	dsb->size = size / 4; /* in dwords */
	dsb->free_pos = 0;
	dsb->ins_start_offset = 0;

	dsb->chicken = dsb_chicken(state, crtc);
	dsb->dewake_scanline = dsb_dewake_scanline(state, crtc);

	return dsb;

out_put_rpm:
	intel_runtime_pm_put(&i915->runtime_pm, wakeref);
	kfree(dsb);
out:
	drm_info_once(&i915->drm,
		      "[CRTC:%d:%s] DSB %d queue setup failed, will fallback to MMIO for display HW programming\n",
		      crtc->base.base.id, crtc->base.name, dsb_id);

	return NULL;
}

/**
 * intel_dsb_cleanup() - To cleanup DSB context.
 * @dsb: DSB context
 *
 * This function cleanup the DSB context by unpinning and releasing
 * the VMA object associated with it.
 */
void intel_dsb_cleanup(struct intel_dsb *dsb)
{
	struct intel_crtc *crtc = dsb->crtc;
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);

	drm_dbg_kms(&i915->drm, "cleanup %p\n", dsb->dsb_buf.vma);
	intel_dsb_buffer_cleanup(&dsb->dsb_buf);
	kfree(dsb);
}

void intel_dsb_setup(struct intel_display *display)
{
	init_waitqueue_head(&display->dsb.wait_queue);
}

void intel_dsb_wait_interrupt(struct intel_dsb *dsb)
{
	struct intel_crtc *crtc = dsb->crtc;
	struct intel_display *display = to_intel_display(crtc->base.dev);
	DEFINE_WAIT(wait);

	add_wait_queue(&display->dsb.wait_queue, &wait);

	/* FIXME need something fancier here */
	if (wait_for(!is_dsb_busy(display, crtc->pipe, dsb->id), 1000))
		drm_err(display->drm,
			"[CRTC:%d:%s] DSB %d timed out waiting for interrupt\n",
			crtc->base.base.id, crtc->base.name, dsb->id);

	remove_wait_queue(&display->dsb.wait_queue, &wait);
}

void intel_dsb_irq_handler(struct intel_display *display,
			   enum pipe pipe, enum intel_dsb_id dsb_id)
{
	struct intel_crtc *crtc = intel_crtc_for_pipe(to_i915(display->drm), pipe);
	u32 tmp, errors;

	tmp = intel_de_read_fw(display, DSB_INTERRUPT(pipe, dsb_id));
	intel_de_write_fw(display, DSB_INTERRUPT(pipe, dsb_id), tmp);

	if (tmp & DSB_PROG_INT_STATUS)
		wake_up_all(&display->dsb.wait_queue);

	errors = tmp & dsb_error_int_status(display);
	if (errors)
		drm_err(display->drm, "[CRTC:%d:%s] / DSB %d error interrupt: 0x%x\n",
			crtc->base.base.id, crtc->base.name, dsb_id, errors);
}

struct dsb_test_sample {
	u32 flip, frame;
};

struct dsb_test_data {
	struct intel_crtc *crtc;
	struct intel_atomic_state *state;
	struct drm_modeset_acquire_ctx *ctx;
	const char *name;

	enum plane_id plane_id;

	i915_reg_t reg;

	/* original reg values */
	u32 misc2, surf, reg_val;

	struct dsb_test_sample pre, post, ts;
};

static int dsb_test_prepare(struct dsb_test_data *d)
{
	struct intel_crtc *crtc = d->crtc;
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_state *state;

	/* FIXME maybe pick a disabled plane, or disable it explicitly? */
	d->plane_id = PLANE_2;

	/* a mostly harmless register with all bits writable by DSB */
	d->reg = PIPE_CRC_EXP_HSW(crtc->pipe);
	d->reg_val = intel_de_read(i915, d->reg);

	d->misc2 = intel_de_rmw(i915, PIPE_MISC2(crtc->pipe),
				PIPE_MISC2_FLIP_INFO_PLANE_SEL_MASK,
				PIPE_MISC2_FLIP_INFO_PLANE_SEL(d->plane_id));

	d->surf = intel_de_read(i915, PLANE_SURF(crtc->pipe, d->plane_id));

	state = drm_atomic_state_alloc(&i915->drm);
	if (!state)
		return -ENOMEM;

	state->acquire_ctx = d->ctx;
	to_intel_atomic_state(state)->internal = true;

	crtc_state = drm_atomic_get_crtc_state(state, &crtc->base);
	if (IS_ERR(crtc_state)){
		drm_atomic_state_put(state);
		return PTR_ERR(crtc_state);
	}

	d->state = to_intel_atomic_state(state);

	return 0;
}

static void dsb_test_restore(struct dsb_test_data *d)
{
	struct intel_crtc *crtc = d->crtc;
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);

	if (d->state)
		drm_atomic_state_put(&d->state->base);

	intel_de_write(i915, d->reg, d->reg_val);
	intel_de_write(i915, PIPE_MISC2(crtc->pipe), d->misc2);
	intel_de_write(i915, PLANE_SURF(crtc->pipe, d->plane_id), d->surf);
}

static void dsb_test_set_force_dewake(struct intel_crtc *crtc, enum intel_dsb_id dsb_id)
{
	struct intel_display *display = to_intel_display(crtc->base.dev);

	intel_de_write_fw(display, DSB_PMCTRL_2(crtc->pipe, dsb_id),
			  DSB_FORCE_DEWAKE);
}

static void dsb_test_clear_force_dewake(struct intel_crtc *crtc, enum intel_dsb_id dsb_id)
{
	struct intel_display *display = to_intel_display(crtc->base.dev);

	intel_de_write_fw(display, DSB_PMCTRL_2(crtc->pipe, dsb_id),
			  DSB_BLOCK_DEWAKE_EXTENSION);
}

static void dsb_test_sample_timestamps(struct intel_crtc *crtc,
				       struct dsb_test_sample *s)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);

	s->flip = intel_de_read_fw(dev_priv, PIPE_FLIPTMSTMP(crtc->pipe));
	s->frame = intel_de_read_fw(dev_priv, PIPE_FRMTMSTMP(crtc->pipe));
}

static void dsb_test_sample_counts(struct intel_crtc *crtc,
				   struct dsb_test_sample *s)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);

	s->flip = intel_de_read_fw(dev_priv, PIPE_FLIPCOUNT_G4X(dev_priv, crtc->pipe));
	s->frame = intel_de_read_fw(dev_priv, PIPE_FRMCOUNT_G4X(dev_priv, crtc->pipe));
}

static bool dsb_test_compare_flipcount(struct dsb_test_data *d, int expected)
{
	struct drm_i915_private *i915 = to_i915(d->crtc->base.dev);

	drm_dbg_kms(&i915->drm, "%s: flips expected %d, got %d\n",
		    d->name, expected, d->post.flip - d->pre.flip);

	return expected != d->post.flip - d->pre.flip;
}

static bool dsb_test_compare_framecount(struct dsb_test_data *d, int expected)
{
	struct drm_i915_private *i915 = to_i915(d->crtc->base.dev);

	drm_dbg_kms(&i915->drm, "%s: frames expected %d, got %d\n",
		    d->name, expected, d->post.frame - d->pre.frame);

	return expected != d->post.frame - d->pre.frame;
}

static bool dsb_test_compare_timestamps(struct dsb_test_data *d,
					int expected, int max_diff)
{
	struct drm_i915_private *i915 = to_i915(d->crtc->base.dev);
	int diff = d->ts.flip - d->ts.frame;

	drm_dbg_kms(&i915->drm, "%s: timestamp expected %d, got %d (max diff %d)\n",
		    d->name, expected, diff, max_diff);

	return abs(diff - expected) > max_diff;
}

static bool dsb_test_compare_latency(struct dsb_test_data *d,
				     int expected, int max_diff)
{
	struct drm_i915_private *i915 = to_i915(d->crtc->base.dev);
	int diff = d->ts.flip - d->ts.frame;

	drm_dbg_kms(&i915->drm, "%s: latency expected %d, got %d (max diff %d)\n",
		    d->name, expected, diff, max_diff);

	return diff > expected + max_diff;
}

static int timestamps_to_scanline(const struct drm_display_mode *adjusted_mode,
				  struct dsb_test_sample *s)
{
	return (intel_usecs_to_scanlines(adjusted_mode, s->flip - s->frame) +
		adjusted_mode->crtc_vblank_start) %
		adjusted_mode->crtc_vtotal;
}

static bool dsb_test_compare_scanline(struct dsb_test_data *d,
				      int expected, int max_diff)
{
	struct drm_i915_private *i915 = to_i915(d->crtc->base.dev);
	const struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(d->state, d->crtc);
	int scanline = timestamps_to_scanline(&crtc_state->hw.adjusted_mode, &d->ts);

	drm_dbg_kms(&i915->drm, "%s: scanline expected %d, got %d (max diff %d)\n",
		    d->name, expected, scanline, max_diff);

	return abs(scanline - expected) > max_diff;
}

static bool dsb_test_compare_reg(struct dsb_test_data *d,
				 u32 val, u32 expected, u32 mask)
{
	struct drm_i915_private *i915 = to_i915(d->crtc->base.dev);

	drm_dbg_kms(&i915->drm, "%s: value expected 0x%08x, got 0x%0x (mask 0x%08x)\n",
		    d->name, expected, val, mask);

	return val != expected;
}

static int test_noop(struct dsb_test_data *d)
{
	struct intel_crtc *crtc = d->crtc;
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);
	int counts[] = { 1, 1<<5, (1<<10) - 64, (1<<15) - 64, (1<<20) - 64 };
	int i, ret;

	ret = dsb_test_prepare(d);
	if (ret)
		goto restore;

	ret = -EINVAL;

	for (i = 0; i < ARRAY_SIZE(counts); i++) {
		struct intel_dsb *dsb;
		ktime_t pre, post;

		dsb = intel_dsb_prepare(d->state, crtc, INTEL_DSB_0, counts[i] + 64);
		if (!dsb)
			goto restore;

		/* FIXME come up with something to check. Duration of noops? */
		intel_dsb_noop(dsb, counts[i]);

		intel_dsb_finish(dsb);
		if (counts[i] == 1)
			intel_dsb_dump(dsb);

		/* TODO use flip timestamps to check here too? */
		pre = ktime_get();

		_intel_dsb_commit(dsb, 0, -1, 0);
		_intel_dsb_wait_inf(dsb);
		intel_dsb_wait(dsb);

		post = ktime_get();

		intel_dsb_cleanup(dsb);

		drm_dbg_kms(&i915->drm, "%s: %d NOOPs took %lld usecs\n",
			    d->name, counts[i], ktime_us_delta(post, pre));
	}

	ret = 0;

restore:
	dsb_test_restore(d);

	return ret;
}

static int test_reg_time(struct dsb_test_data *d,
			 bool indexed, bool nonposted)
{
	struct intel_crtc *crtc = d->crtc;
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);
	int counts[] = { 1, 1<<5, (1<<10) - 64, (1<<15) - 64, (1<<20) - 64 };
	int i, ret;

	ret = dsb_test_prepare(d);
	if (ret)
		goto restore;

	ret = -EINVAL;

	for (i = 0; i < ARRAY_SIZE(counts); i++) {
		struct intel_dsb *dsb;
		ktime_t pre, post;
		int j;

		dsb = intel_dsb_prepare(d->state, crtc, INTEL_DSB_0, counts[i] + 64);
		if (!dsb)
			goto restore;

		if (nonposted)
			intel_dsb_nonpost_start(dsb);

		for (j = 0; j < counts[i]; j++) {
			if (indexed)
				intel_dsb_reg_write(dsb, d->reg, 0);
			else
				intel_dsb_reg_write_masked(dsb, d->reg, 0xffffffff, 0);
		}

		if (nonposted)
			intel_dsb_nonpost_end(dsb);

		intel_dsb_finish(dsb);
		if (counts[i] == 1)
			intel_dsb_dump(dsb);

		/* TODO use flip timestamps to check here too? */
		pre = ktime_get();

		_intel_dsb_commit(dsb, 0, -1, 0);
		_intel_dsb_wait_inf(dsb);
		intel_dsb_wait(dsb);

		post = ktime_get();

		intel_dsb_cleanup(dsb);

		drm_dbg_kms(&i915->drm, "%s: %d MMIO writes took %lld usecs\n",
			    d->name, counts[i], ktime_us_delta(post, pre));
	}

	ret = 0;

restore:
	dsb_test_restore(d);

	return ret;
}

static int test_reg_time_mmio_posted(struct dsb_test_data *d)
{
	return test_reg_time(d, false, false);
}

static int test_reg_time_mmio_nonposted(struct dsb_test_data *d)
{
	return test_reg_time(d, false, true);
}

static int test_reg_time_indexed_posted(struct dsb_test_data *d)
{
	return test_reg_time(d, true, false);
}

static int test_reg_time_indexed_nonposted(struct dsb_test_data *d)
{
	return test_reg_time(d, true, true);
}

static int test_reg_write(struct dsb_test_data *d)
{
	static const u32 masks[] = {
		0xffffffff,
		0x00000000,
		0xff000000,
		0x00ff0000,
		0x0000ff00,
		0x000000ff,
	};
	struct intel_crtc *crtc = d->crtc;
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);
	int i, ret;

	ret = dsb_test_prepare(d);
	if (ret)
		goto restore;

	ret = -EINVAL;

	for (i = 0; i < ARRAY_SIZE(masks); i++) {
		u32 old, new, expected, val, mask = masks[i];
		struct intel_dsb *dsb;

		old = ~d->reg_val;
		new = old ^ 0xaaaaaaaa;
		expected = (new & mask) | (old & ~mask);

		if (drm_WARN_ON(&i915->drm, mask != 0x00000000 && old == expected))
			goto restore;

		if (drm_WARN_ON(&i915->drm, mask != 0xffffffff && new == expected))
			goto restore;

		intel_de_write_fw(i915, d->reg, old);
		if (intel_de_read_fw(i915, d->reg) != old)
			goto restore;

		dsb = intel_dsb_prepare(d->state, crtc, INTEL_DSB_0, 512);
		if (!dsb)
			goto restore;

		intel_dsb_reg_write_masked(dsb, d->reg, mask, new);

		intel_dsb_finish(dsb);
		intel_dsb_dump(dsb);

		_intel_dsb_commit(dsb, 0, -1, 0);
		intel_dsb_wait(dsb);

		intel_dsb_cleanup(dsb);

		val = intel_de_read_fw(i915, d->reg);

		if (dsb_test_compare_reg(d, val, expected, mask))
			goto restore;
	}

	ret = 0;

restore:
	dsb_test_restore(d);

	return ret;
}

static int test_start_on_vblank(struct dsb_test_data *d)
{
	struct intel_crtc *crtc = d->crtc;
	enum pipe pipe = crtc->pipe;
	int count, ret;

	ret = dsb_test_prepare(d);
	if (ret)
		goto restore;

	ret = -EINVAL;

	for (count = 1; count < 2; count++) {
		struct intel_dsb *dsb;

		dsb = intel_dsb_prepare(d->state, crtc, INTEL_DSB_0, 512);
		if (!dsb)
			goto restore;

		intel_dsb_reg_write(dsb, PLANE_SURF(pipe, d->plane_id), d->surf);

		intel_dsb_finish(dsb);
		intel_dsb_dump(dsb);

		/* make sure pkgC latency is not an issue */
		dsb_test_set_force_dewake(crtc, dsb->id);

		intel_crtc_wait_for_next_vblank(crtc);

		dsb_test_sample_counts(crtc, &d->pre);

		_intel_dsb_commit(dsb, DSB_WAIT_FOR_VBLANK, -1, 0);
		_intel_dsb_wait_inf(dsb);
		intel_dsb_wait(dsb);

		dsb_test_clear_force_dewake(crtc, dsb->id);

		intel_dsb_cleanup(dsb);

		dsb_test_sample_counts(crtc, &d->post);
		dsb_test_sample_timestamps(crtc, &d->ts);

		if (dsb_test_compare_flipcount(d, 1))
			goto restore;

		if (dsb_test_compare_framecount(d, count))
			goto restore;

		/* FIXME this assumes that delayed vblank is not used */
		if (dsb_test_compare_timestamps(d, 0, 5))
			goto restore;

	}

	ret = 0;

restore:
	dsb_test_restore(d);

	return ret;
}

static int test_reiterate(struct dsb_test_data *d)
{
	struct intel_crtc *crtc = d->crtc;
	enum pipe pipe = crtc->pipe;
	int count, ret;

	ret = dsb_test_prepare(d);
	if (ret)
		goto restore;

	ret = -EINVAL;

	/* 0 == 2^32, don't want to wait that long */
	for (count = 0; count <= 5; count++) {
		struct intel_dsb *dsb;

		dsb = intel_dsb_prepare(d->state, crtc, INTEL_DSB_0, 512);
		if (!dsb)
			goto restore;

		intel_dsb_reg_write(dsb, PLANE_SURF(pipe, d->plane_id), d->surf);

		intel_dsb_finish(dsb);
		intel_dsb_dump(dsb);

		intel_crtc_wait_for_next_vblank(crtc);

		dsb_test_sample_counts(crtc, &d->pre);

		_intel_dsb_commit(dsb, DSB_BUF_REITERATE, -1, count);
		_intel_dsb_wait_inf(dsb);
		intel_dsb_wait(dsb);

		intel_dsb_cleanup(dsb);

		dsb_test_sample_counts(crtc, &d->post);

		if (dsb_test_compare_flipcount(d, count))
			goto restore;
	}

	ret = 0;

restore:
	dsb_test_restore(d);

	return ret;
}

static int test_wait_usec(struct dsb_test_data *d)
{
	struct intel_crtc *crtc = d->crtc;
	static const u16 usecs[] = {
		0, 1, 10, 100, 1000,
	};
	enum pipe pipe = crtc->pipe;
	int i, ret;

	ret = dsb_test_prepare(d);
	if (ret)
		goto restore;

	ret = -EINVAL;

	for (i = 0; i < ARRAY_SIZE(usecs); i++) {
		struct intel_dsb *dsb;

		dsb = intel_dsb_prepare(d->state, crtc, INTEL_DSB_0, 512);
		if (!dsb)
			goto restore;

		intel_dsb_wait_usec(dsb, usecs[i]);

		intel_dsb_reg_write(dsb, PLANE_SURF(pipe, d->plane_id), d->surf);

		intel_dsb_finish(dsb);
		intel_dsb_dump(dsb);

		/* make sure pkgC latency is not an issue */
		dsb_test_set_force_dewake(crtc, dsb->id);

		intel_crtc_wait_for_next_vblank(crtc);

		_intel_dsb_commit(dsb, DSB_WAIT_FOR_VBLANK, -1, 0);
		_intel_dsb_wait_inf(dsb);
		intel_dsb_wait(dsb);

		dsb_test_clear_force_dewake(crtc, dsb->id);

		intel_dsb_cleanup(dsb);

		dsb_test_sample_timestamps(crtc, &d->ts);

		/* FIXME this assumes that delayed vblank is not used */
		if (dsb_test_compare_timestamps(d, usecs[i], 5))
			goto restore;
	}

	ret = 0;

restore:
	dsb_test_restore(d);

	return ret;
}

static int test_wait_scanline(struct dsb_test_data *d)
{
	struct intel_crtc *crtc = d->crtc;
	struct intel_display *display = to_intel_display(crtc->base.dev);
	struct drm_i915_private *dev_priv = to_i915(display->drm);
	enum pipe pipe = crtc->pipe;
	int i, ret;

	ret = dsb_test_prepare(d);
	if (ret)
		goto restore;

	ret = -EINVAL;

	{
		u32 post, pre = intel_de_read_fw(dev_priv, PIPE_FRMTMSTMP(pipe));
		while ((post = intel_de_read_fw(dev_priv, PIPE_FRMTMSTMP(pipe))) == pre)
			;
		drm_dbg_kms(&dev_priv->drm, "frame ts %u\n", post - pre);
	}

	for (i = 0; i < 5; i++) {
		const struct intel_crtc_state *crtc_state =
			intel_atomic_get_new_crtc_state(d->state, crtc);
		int target_scanline = i * crtc_state->hw.adjusted_mode.crtc_vtotal / 5;
		struct intel_dsb *dsb;

		dsb = intel_dsb_prepare(d->state, crtc, INTEL_DSB_0, 512);
		if (!dsb) {
			ret = -ENOMEM;
			goto restore;
		}

		intel_dsb_wait_scanline(d->state, dsb, target_scanline);

		intel_dsb_reg_write(dsb, PLANE_SURF(pipe, d->plane_id), d->surf);

		intel_dsb_finish(dsb);
		intel_dsb_dump(dsb);

		/* make sure pkgC latency is not an issue */
		dsb_test_set_force_dewake(crtc, dsb->id);

		intel_crtc_wait_for_next_vblank(crtc);

		dsb_test_sample_counts(crtc, &d->pre);

		_intel_dsb_commit(dsb, DSB_WAIT_FOR_VBLANK, -1, 0);
		_intel_dsb_wait_inf(dsb);
		intel_dsb_wait(dsb);

		dsb_test_clear_force_dewake(crtc, dsb->id);

		intel_dsb_cleanup(dsb);

		dsb_test_sample_timestamps(crtc, &d->ts);
		dsb_test_sample_counts(crtc, &d->post);

		drm_dbg_kms(&dev_priv->drm, "TS flip %u, TS frame %u, diff %d\n",
			    d->ts.flip, d->ts.frame, d->ts.flip - d->ts.frame);

		if (dsb_test_compare_flipcount(d, 1))
			goto restore;

		if (dsb_test_compare_framecount(d, 1))
			goto restore;

		if (dsb_test_compare_scanline(d, target_scanline, 1))
			goto restore;
	}

	ret = 0;

restore:
	dsb_test_restore(d);

	return ret;
}

static int test_wait_vblanks(struct dsb_test_data *d)
{
	struct intel_crtc *crtc = d->crtc;
	enum pipe pipe = crtc->pipe;
	int count, ret;

	ret = dsb_test_prepare(d);
	if (ret)
		goto restore;

	ret = -EINVAL;

	for (count = 0; count <= 5; count++) {
		struct intel_dsb *dsb;

		dsb = intel_dsb_prepare(d->state, crtc, INTEL_DSB_0, 512);
		if (!dsb)
			goto restore;

		intel_dsb_wait_vblanks(dsb, count);

		intel_dsb_reg_write(dsb, PLANE_SURF(pipe, d->plane_id), d->surf);

		intel_dsb_finish(dsb);
		intel_dsb_dump(dsb);

		/* make sure pkgC latency is not an issue */
		dsb_test_set_force_dewake(crtc, dsb->id);

		intel_crtc_wait_for_next_vblank(crtc);

		dsb_test_sample_counts(crtc, &d->pre);

		_intel_dsb_commit(dsb, 0, -1, 0);
		_intel_dsb_wait_inf(dsb);
		intel_dsb_wait(dsb);

		dsb_test_clear_force_dewake(crtc, dsb->id);

		intel_dsb_cleanup(dsb);

		dsb_test_sample_timestamps(crtc, &d->ts);
		dsb_test_sample_counts(crtc, &d->post);

		if (dsb_test_compare_flipcount(d, 1))
			goto restore;

		if (dsb_test_compare_framecount(d, count))
			goto restore;

		/*
		 * Zero vblanks means no wait so the flip happens whenever
		 * DSB starts executing -> flips timestamp is meaningless.
		 */
		/* FIXME this assumes that delayed vblank is not used */
		if (count && dsb_test_compare_timestamps(d, 0, 5))
			goto restore;
	}

	ret = 0;

restore:
	dsb_test_restore(d);

	return ret;
}

static int test_wait_scanline_inout(struct dsb_test_data *d, bool scanline_in)
{
	struct intel_crtc *crtc = d->crtc;
	struct intel_display *display = to_intel_display(crtc->base.dev);
	struct drm_i915_private *dev_priv = to_i915(display->drm);
	enum pipe pipe = crtc->pipe;
	int i, ret;

	ret = dsb_test_prepare(d);
	if (ret)
		goto restore;

	ret = -EINVAL;

	for (i = 0; i < 10; i++) {
		const struct intel_crtc_state *crtc_state =
			intel_atomic_get_new_crtc_state(d->state, crtc);
		int vblank_len = crtc_state->hw.adjusted_mode.crtc_vtotal -
			crtc_state->hw.adjusted_mode.crtc_vdisplay;
		int target_scanline = (crtc_state->hw.adjusted_mode.crtc_vdisplay
				       + (i + 1) * vblank_len / 3) %
			crtc_state->hw.adjusted_mode.crtc_vtotal;
		struct intel_dsb *dsb;

		dsb = intel_dsb_prepare(d->state, crtc, INTEL_DSB_0, 512);
		if (!dsb)
			goto restore;

		if (scanline_in) {
			int start = target_scanline;
			int end = crtc_state->hw.adjusted_mode.crtc_vdisplay - 1;

			intel_dsb_wait_scanline_in(d->state, dsb, start, end);
		} else {
			int start = crtc_state->hw.adjusted_mode.crtc_vdisplay;
			int end = target_scanline - 1;

			intel_dsb_wait_scanline_out(d->state, dsb, start, end);
		}

		if (0) {
			int fuzz = 0;
			int dsl = dsb_scanline_to_hw(d->state, crtc,
						     (target_scanline +
						      crtc_state->hw.adjusted_mode.crtc_vtotal - fuzz) %
						     crtc_state->hw.adjusted_mode.crtc_vtotal);
			drm_dbg_kms(&dev_priv->drm, "poll DSL %d\n", dsl);
			intel_dsb_emit_poll(dsb, PIPEDSL(dev_priv, pipe), ~0, dsl,
					    2, REG_FIELD_GET(DSB_POLL_COUNT_MASK, DSB_POLL_COUNT_MASK));
		}

		intel_dsb_reg_write(dsb, PLANE_SURF(pipe, d->plane_id), d->surf);

		intel_dsb_finish(dsb);
		intel_dsb_dump(dsb);

		/* make sure pkgC latency is not an issue */
		dsb_test_set_force_dewake(crtc, dsb->id);

		intel_crtc_wait_for_next_vblank(crtc);

		dsb_test_sample_counts(crtc, &d->pre);

		_intel_dsb_commit(dsb, DSB_WAIT_FOR_VBLANK, -1, 0);
		_intel_dsb_wait_inf(dsb);
		intel_dsb_wait(dsb);

		dsb_test_clear_force_dewake(crtc, dsb->id);

		intel_dsb_cleanup(dsb);

		dsb_test_sample_timestamps(crtc, &d->ts);
		dsb_test_sample_counts(crtc, &d->post);

		if (dsb_test_compare_flipcount(d, 1))
			goto restore;

		if (dsb_test_compare_framecount(d, 1))
			goto restore;

		if (dsb_test_compare_scanline(d, target_scanline, 1))
			goto restore;
	}

	ret = 0;

restore:
	dsb_test_restore(d);

	return ret;
}

static int test_wait_scanline_in(struct dsb_test_data *d)

{
	return test_wait_scanline_inout(d, true);
}

static int test_wait_scanline_out(struct dsb_test_data *d)
{
	return test_wait_scanline_inout(d, false);
}

static int test_interrupt(struct dsb_test_data *d)
{
	struct intel_crtc *crtc = d->crtc;
	enum pipe pipe = crtc->pipe;
	int count, ret;

	ret = dsb_test_prepare(d);
	if (ret)
		goto restore;

	ret = -EINVAL;

	for (count = 1; count < 2; count++) {
		struct intel_dsb *dsb;

		dsb = intel_dsb_prepare(d->state, crtc, INTEL_DSB_0, 512);
		if (!dsb)
			goto restore;

		intel_dsb_reg_write(dsb, PLANE_SURF(pipe, d->plane_id), d->surf);
		//FDOIXME intel_dsb_interrupt(dsb);

		intel_dsb_finish(dsb);
		intel_dsb_dump(dsb);

		/* make sure pkgC latency is not an issue */
		dsb_test_set_force_dewake(crtc, dsb->id);

		intel_crtc_wait_for_next_vblank(crtc);

		dsb_test_sample_counts(crtc, &d->pre);

		_intel_dsb_commit(dsb, DSB_WAIT_FOR_VBLANK, -1, 0);

		intel_dsb_wait_interrupt(dsb);
		intel_dsb_wait(dsb);

		dsb_test_clear_force_dewake(crtc, dsb->id);

		intel_dsb_cleanup(dsb);

		dsb_test_sample_counts(crtc, &d->post);

		if (dsb_test_compare_flipcount(d, 1))
			goto restore;
	}

	ret = 0;

restore:
	dsb_test_restore(d);

	return ret;
}

static int test_poll(struct dsb_test_data *d)
{
	struct intel_crtc *crtc = d->crtc;
	struct intel_display *display = to_intel_display(crtc->base.dev);
	static const u32 masks[] = {
		0xffffffff,
		0x00000000,
		0xff000000,
		0x00ff0000,
		0x0000ff00,
		0x000000ff,
	};
	enum pipe pipe = crtc->pipe;
	int i, ret;

	ret = dsb_test_prepare(d);
	if (ret)
		goto restore;

	ret = -EINVAL;

	for (i = 0; i < ARRAY_SIZE(masks); i++) {
		struct intel_dsb *dsb;
		int max_wait, max_count;
		u32 interrupt, val;
		int j;

		dsb = intel_dsb_prepare(d->state, crtc, INTEL_DSB_0, 512);
		if (!dsb)
			goto restore;

		max_wait = REG_FIELD_GET(DSB_POLL_WAIT_MASK, DSB_POLL_WAIT_MASK);
		max_count = REG_FIELD_GET(DSB_POLL_COUNT_MASK, DSB_POLL_COUNT_MASK);

		/*
		 * We seem to poll a bit too long which causes the DSB
		 * to raise the poll error interrupt. Silence it.
		 *
		 * FIXME measure how long until the error is raised,
		 * and figure out if it's normal.
		 */
		interrupt = dsb_error_int_status(display) | DSB_PROG_INT_STATUS |
			dsb_error_int_en(display) | DSB_PROG_INT_EN;
		interrupt &= ~DSB_POLL_ERR_INT_EN;
		intel_dsb_reg_write(dsb, DSB_INTERRUPT(pipe, dsb->id), interrupt);

		intel_dsb_emit_poll(dsb, d->reg, masks[i], 0x11223344, max_wait, max_count);

		intel_dsb_reg_write(dsb, PLANE_SURF(pipe, d->plane_id), d->surf);

		intel_dsb_finish(dsb);
		intel_dsb_dump(dsb);

		dsb_test_sample_counts(crtc, &d->pre);

		intel_de_write_fw(display, d->reg, 0xdeadbeef);

		_intel_dsb_commit(dsb, 0, -1, 0);

		if (!wait_for(!is_dsb_busy(display, pipe, dsb->id), 1)) {
			drm_dbg_kms(display->drm, "%s: DSB finished early\n", d->name);
			goto restore;
		}

		for (j = 0; j < ARRAY_SIZE(masks); j++) {
			if (masks[i] == masks[j])
				continue;

			intel_de_write_fw(display, d->reg, 0x55555555 & masks[j]);

			if (!wait_for(!is_dsb_busy(display, pipe, dsb->id), 1)) {
				drm_dbg_kms(display->drm, "%s: DSB finished early (mask 0x%08x)\n",
					    d->name, masks[j]);
				goto restore;
			}
		}

		intel_de_write_fw(display, d->reg, 0x55555555 & masks[i]);

		_intel_dsb_wait_inf(dsb);
		intel_dsb_wait(dsb);

		intel_dsb_cleanup(dsb);

		dsb_test_sample_counts(crtc, &d->post);

		val = intel_de_read_fw(display, d->reg);
		if (dsb_test_compare_reg(d, val & masks[i], 0x55555555 & masks[i], masks[i]))
			goto restore;

		if (dsb_test_compare_flipcount(d, 1))
			goto restore;
	}

	ret = 0;

restore:
	dsb_test_restore(d);

	return ret;
}

static int setup_latency(struct dsb_test_data *d, int num_levels)
{
	struct intel_crtc *crtc = d->crtc;
	struct intel_display *display = to_intel_display(crtc->base.dev);
	struct drm_i915_private *dev_priv = to_i915(display->drm);
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_state *state;
	int ret;

	dev_priv->display.wm.num_levels_limit =
		min(num_levels, dev_priv->display.wm.num_levels);

	state = drm_atomic_state_alloc(&dev_priv->drm);
	if (!state)
		return -ENOMEM;

	state->acquire_ctx = d->ctx;
	to_intel_atomic_state(state)->internal = true;

	crtc_state = drm_atomic_get_crtc_state(state, &crtc->base);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto out;
	}

	ret = drm_atomic_add_affected_planes(state, &crtc->base);
	if (ret)
		return ret;

	ret = drm_atomic_commit(state);

out:
	drm_atomic_state_put(state);

	return ret;
}

static int test_latency_level(struct dsb_test_data *d,
			      int level, bool dewake)
{
	struct intel_crtc *crtc = d->crtc;
	struct intel_display *display = to_intel_display(crtc->base.dev);
	unsigned int pkgc_latency, latency;
	enum pipe pipe = crtc->pipe;
	int count, ret;

	pkgc_latency = display->wm.skl_latency[level];
	if (!pkgc_latency)
		return 0;

	if (dewake)
		latency = 1;
	else
		latency = pkgc_latency;

	ret = setup_latency(d, level + 1);
	if (ret)
		return ret;

	ret = dsb_test_prepare(d);
	if (ret)
		goto restore;

	ret = -EINVAL;

	for (count = 0; count <= 5; count++) {
		struct intel_dsb *dsb;

		dsb = intel_dsb_prepare(d->state, crtc, INTEL_DSB_0, 512);
		if (!dsb)
			goto restore;

		intel_dsb_reg_write(dsb, PLANE_SURF(pipe, d->plane_id), d->surf);

		intel_dsb_finish(dsb);
		intel_dsb_dump(dsb);

		intel_crtc_wait_for_next_vblank(crtc);

		dsb_test_sample_counts(crtc, &d->pre);

		_intel_dsb_commit(dsb, DSB_WAIT_FOR_VBLANK,
				  dewake ? dsb->dewake_scanline : -1,
				  0);

		intel_crtc_wait_for_next_vblank(crtc);

		_intel_dsb_wait_inf(dsb);
		intel_dsb_wait(dsb);

		intel_dsb_cleanup(dsb);

		dsb_test_sample_timestamps(crtc, &d->ts);
		dsb_test_sample_counts(crtc, &d->post);

		if (dsb_test_compare_flipcount(d, 1))
			goto restore;

		if (dsb_test_compare_framecount(d, 1))
			goto restore;

		/* FIXME this assumes that delayed vblank is not used */
		if (dsb_test_compare_latency(d, latency, 2))
			goto restore;
	}

	ret = 0;

restore:
	dsb_test_restore(d);

	drm_WARN_ON(display->drm,
		    setup_latency(d, display->wm.num_levels));

	return ret;
}

static int test_latency_levels(struct dsb_test_data *d, bool dewake)
{
	struct intel_crtc *crtc = d->crtc;
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);
	int level;

	for (level = 0; level < i915->display.wm.num_levels; level++) {
		int ret;

		ret = test_latency_level(d, level, dewake);
		if (ret)
			return ret;
	}

	return 0;
}

static int test_latency(struct dsb_test_data *d)
{
	return test_latency_levels(d, false);
}

static int test_latency_dewake(struct dsb_test_data *d)
{
	return test_latency_levels(d, true);
}

static int test_chain(struct dsb_test_data *d, bool start_on_vblank)
{
	struct intel_crtc *crtc = d->crtc;
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;
	int count, ret;

	ret = dsb_test_prepare(d);
	if (ret)
		goto restore;

	ret = -EINVAL;

	for (count = 1; count < 2; count++) {
		struct intel_dsb *dsb, *chained_dsb;
		u32 dsl;

		chained_dsb = intel_dsb_prepare(d->state, crtc, INTEL_DSB_1, 512);
		if (!chained_dsb)
			goto restore;

		intel_dsb_reg_write(chained_dsb, PLANE_SURF(pipe, d->plane_id), d->surf);

		intel_dsb_finish(chained_dsb);
		intel_dsb_dump(chained_dsb);

		dsb = intel_dsb_prepare(d->state, crtc, INTEL_DSB_0, 512);
		if (!dsb) {
			intel_dsb_finish(chained_dsb);
			goto restore;
		}

		intel_dsb_reg_write(dsb, PLANE_SURF(pipe, d->plane_id), d->surf);
		intel_dsb_chain(d->state, dsb, chained_dsb, start_on_vblank);

		intel_dsb_finish(dsb);
		intel_dsb_dump(dsb);

		intel_crtc_wait_for_next_vblank(crtc);
		while ((intel_de_read_fw(dev_priv, PIPEDSL(dev_priv, pipe)) & PIPEDSL_LINE_MASK) > 100)
			;

		dsb_test_sample_counts(crtc, &d->pre);

		dsl = intel_de_read_fw(dev_priv, PIPEDSL(dev_priv, pipe)) & PIPEDSL_LINE_MASK;
		_intel_dsb_commit(dsb, 0, -1, 0);

		_intel_dsb_wait_inf(dsb);
		_intel_dsb_wait_inf(chained_dsb);

		intel_dsb_wait(dsb);
		intel_dsb_wait(chained_dsb);

		intel_dsb_cleanup(dsb);
		intel_dsb_cleanup(chained_dsb);

		dsb_test_sample_timestamps(crtc, &d->ts);
		dsb_test_sample_counts(crtc, &d->post);

		/* FIXME this assumes that delayed vblank is not used */
		drm_dbg_kms(&dev_priv->drm, "%s: start DSL %d\n", d->name, dsl);
		drm_dbg_kms(&dev_priv->drm, "%s: timestamp expected <=%d, got %d\n",
			    d->name, 5, d->ts.flip - d->ts.frame);
		drm_dbg_kms(&dev_priv->drm, "%s: flips expected %d, got %d\n",
			    d->name, 2, d->post.flip - d->pre.flip);
		drm_dbg_kms(&dev_priv->drm, "%s: frames expected %d, got %d\n",
			    d->name, start_on_vblank, d->post.frame - d->pre.frame);

		if (dsb_test_compare_flipcount(d, 2))
			goto restore;

		if (dsb_test_compare_framecount(d, start_on_vblank))
			goto restore;

		if (start_on_vblank && dsb_test_compare_timestamps(d, 0, 5))
			goto restore;
	}

	ret = 0;

restore:
	dsb_test_restore(d);

	return ret;
}

static int test_chained(struct dsb_test_data *d)
{
	return test_chain(d, false);
}

static int test_chained_start_on_vblank(struct dsb_test_data *d)
{
	return test_chain(d, true);
}

struct intel_dsb_test {
	int (*func)(struct dsb_test_data *d);
	const char *name;
};

static const struct intel_dsb_test tests[] = {
	{ .func = test_noop, .name = "noop", },
	{ .func = test_reg_time_mmio_posted, .name = "reg_time_mmio_posted", },
	{ .func = test_reg_time_indexed_posted, .name = "reg_time_indexed_posted", },
	{ .func = test_reg_time_mmio_nonposted, .name = "reg_time_mmio_nonposted", },
	{ .func = test_reg_time_indexed_nonposted, .name = "reg_time_indexed_nonposted", },
	{ .func = test_reg_write, .name = "reg_write", },
	{ .func = test_start_on_vblank, .name = "start_on_vblank", },
	{ .func = test_reiterate, .name = "reiterate", },
	{ .func = test_wait_usec, .name = "wait_usec", },
	{ .func = test_wait_scanline, .name = "wait_scanline", },
	{ .func = test_wait_vblanks, .name = "wait_vblanks", },
	{ .func = test_wait_scanline_in, .name = "wait_scanline_in", },
	{ .func = test_wait_scanline_out, .name = "wait_scanline_out", },
	{ .func = test_interrupt, .name = "interrupt", },
	{ .func = test_poll, .name = "poll", },
	{ .func = test_latency, .name = "latency", },
	{ .func = test_latency_dewake, .name = "latency_dewake", },
	{ .func = test_chained, .name = "chained", },
	{ .func = test_chained_start_on_vblank, .name = "chained_start_on_vblank", },
};

static int intel_dsb_debugfs_test(const struct intel_dsb_test *test,
				  struct intel_crtc *crtc)
{
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);
	const struct intel_crtc_state *crtc_state;
	struct drm_modeset_acquire_ctx ctx;
	struct dsb_test_data d = {};
	int ret;

	drm_modeset_acquire_init(&ctx, 0);

again:
	drm_modeset_lock(&crtc->base.mutex, &ctx);

	crtc_state = to_intel_crtc_state(crtc->base.state);

	if (!crtc_state->hw.active) {
		ret = -ENOLINK;
		goto unlock;
	}

	if (crtc_state->uapi.commit &&
	    !try_wait_for_completion(&crtc_state->uapi.commit->hw_done)) {
		ret = -EBUSY;
		goto unlock;
	}

	d.crtc = crtc;
	d.ctx = &ctx;
	d.name = test->name;

	ret = test->func(&d);

	if (ret == -EDEADLK) {
		drm_modeset_backoff(&ctx);
		goto again;
	}

	drm_dbg_kms(&i915->drm, "[CRTC:%d:%s] DSB %s test: %s\n",
		    crtc->base.base.id, crtc->base.name,
		    test->name, ret ? "fail" : "success");

unlock:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return ret;
}


static ssize_t intel_dsb_debugfs_test_write(struct file *file,
					    const char __user *ubuf, size_t len,
					    loff_t *offp)
{
	struct seq_file *m = file->private_data;
	struct intel_crtc *crtc = m->private;
	bool valid = false;
	char *name;
	int i;

	printk(KERN_CRIT "len = %zu\n", len);

	name = memdup_user_nul(ubuf, len);
	if (IS_ERR(name))
		return PTR_ERR(name);
	if (len && name[len-1] == '\n')
		name[len-1] = '\0';

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		int ret;

		if (strcmp(name, tests[i].name) && strcmp(name, "all"))
			continue;

		valid = true;

		ret = intel_dsb_debugfs_test(&tests[i], crtc);
		if (ret)
			return ret;
	}

	kfree(name);

	return valid ? len : -EINVAL;
}

static int intel_dsb_debugfs_test_show(struct seq_file *m, void *data)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(tests); i++)
		seq_printf(m, "%s%s", i != 0 ? " " : "", tests[i].name);
	seq_printf(m, "\n");

	return 0;
}

static int intel_dsb_debugfs_test_open(struct inode *inode, struct file *file)
{
	return single_open(file, intel_dsb_debugfs_test_show, inode->i_private);
}

static const struct file_operations intel_dsb_debugfs_test_fops = {
	.owner = THIS_MODULE,
	.open = intel_dsb_debugfs_test_open,
	.read = seq_read,
	.write = intel_dsb_debugfs_test_write,
	.llseek = default_llseek,
	.release = single_release,
};

void intel_dsb_crtc_debugfs_add(struct intel_crtc *crtc)
{
	debugfs_create_file("i915_dsb_test", 0644, crtc->base.debugfs_entry,
			    crtc, &intel_dsb_debugfs_test_fops);
}
