/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * DOC: atomic plane helpers
 *
 * The functions here are used by the atomic plane helper functions to
 * implement legacy plane updates (i.e., drm_plane->update_plane() and
 * drm_plane->disable_plane()).  This allows plane updates to use the
 * atomic state infrastructure and perform plane updates as separate
 * prepare/check/commit/cleanup steps.
 */

#include <linux/dma-fence-chain.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_fourcc.h>

#include "i915_config.h"
#include "i915_reg.h"
#include "intel_atomic_plane.h"
#include "intel_cdclk.h"
#include "intel_display_rps.h"
#include "intel_display_trace.h"
#include "intel_display_types.h"
#include "intel_fb.h"
#include "intel_fb_pin.h"
#include "skl_scaler.h"
#include "skl_universal_plane.h"
#include "skl_watermark.h"

static void intel_plane_state_reset(struct intel_plane_state *plane_state,
				    struct intel_plane *plane)
{
	memset(plane_state, 0, sizeof(*plane_state));

	__drm_atomic_helper_plane_state_reset(&plane_state->uapi, &plane->base);

	plane_state->scaler_id = -1;
	drm_rect_clear(&plane_state->sel_fetch.src);
	drm_rect_clear(&plane_state->sel_fetch.dst);
}

struct intel_plane *intel_plane_alloc(void)
{
	struct intel_plane_state *plane_state;
	struct intel_plane *plane;

	plane = kzalloc(sizeof(*plane), GFP_KERNEL);
	if (!plane)
		return ERR_PTR(-ENOMEM);

	plane_state = kzalloc(sizeof(*plane_state), GFP_KERNEL);
	if (!plane_state) {
		kfree(plane);
		return ERR_PTR(-ENOMEM);
	}

	intel_plane_state_reset(plane_state, plane);

	plane->base.state = &plane_state->uapi;

	return plane;
}

void intel_plane_free(struct intel_plane *plane)
{
	intel_plane_destroy_state(&plane->base, plane->base.state);
	kfree(plane);
}

/**
 * intel_plane_duplicate_state - duplicate plane state
 * @plane: drm plane
 *
 * Allocates and returns a copy of the plane state (both common and
 * Intel-specific) for the specified plane.
 *
 * Returns: The newly allocated plane state, or NULL on failure.
 */
struct drm_plane_state *
intel_plane_duplicate_state(struct drm_plane *plane)
{
	struct intel_plane_state *intel_state;

	intel_state = to_intel_plane_state(plane->state);
	intel_state = kmemdup(intel_state, sizeof(*intel_state), GFP_KERNEL);

	if (!intel_state)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &intel_state->uapi);

	intel_state->ggtt_vma = NULL;
	intel_state->dpt_vma = NULL;
	intel_state->flags = 0;

	/* add reference to fb */
	if (intel_state->hw.fb)
		drm_framebuffer_get(intel_state->hw.fb);

	return &intel_state->uapi;
}

/**
 * intel_plane_destroy_state - destroy plane state
 * @plane: drm plane
 * @state: state object to destroy
 *
 * Destroys the plane state (both common and Intel-specific) for the
 * specified plane.
 */
void
intel_plane_destroy_state(struct drm_plane *plane,
			  struct drm_plane_state *state)
{
	struct intel_plane_state *plane_state = to_intel_plane_state(state);

	drm_WARN_ON(plane->dev, plane_state->ggtt_vma);
	drm_WARN_ON(plane->dev, plane_state->dpt_vma);

	__drm_atomic_helper_plane_destroy_state(&plane_state->uapi);
	if (plane_state->hw.fb)
		drm_framebuffer_put(plane_state->hw.fb);
	kfree(plane_state);
}

unsigned int intel_adjusted_rate(const struct drm_rect *src,
				 const struct drm_rect *dst,
				 unsigned int rate)
{
	unsigned int src_w, src_h, dst_w, dst_h;

	src_w = drm_rect_width(src) >> 16;
	src_h = drm_rect_height(src) >> 16;
	dst_w = drm_rect_width(dst);
	dst_h = drm_rect_height(dst);

	/* Downscaling limits the maximum pixel rate */
	dst_w = min(src_w, dst_w);
	dst_h = min(src_h, dst_h);

	return DIV_ROUND_UP_ULL(mul_u32_u32(rate, src_w * src_h),
				dst_w * dst_h);
}

unsigned int intel_plane_pixel_rate(const struct intel_crtc_state *crtc_state,
				    const struct intel_plane_state *plane_state)
{
	/*
	 * Note we don't check for plane visibility here as
	 * we want to use this when calculating the cursor
	 * watermarks even if the cursor is fully offscreen.
	 * That depends on the src/dst rectangles being
	 * correctly populated whenever the watermark code
	 * considers the cursor to be visible, whether or not
	 * it is actually visible.
	 *
	 * See: intel_wm_plane_visible() and intel_check_cursor()
	 */

	return intel_adjusted_rate(&plane_state->uapi.src,
				   &plane_state->uapi.dst,
				   crtc_state->pixel_rate);
}

unsigned int intel_plane_data_rate(const struct intel_crtc_state *crtc_state,
				   const struct intel_plane_state *plane_state,
				   int color_plane)
{
	const struct drm_framebuffer *fb = plane_state->hw.fb;

	if (!plane_state->uapi.visible)
		return 0;

	return intel_plane_pixel_rate(crtc_state, plane_state) *
		fb->format->cpp[color_plane];
}

static bool
use_min_ddb(const struct intel_crtc_state *crtc_state,
	    struct intel_plane *plane)
{
	struct drm_i915_private *i915 = to_i915(plane->base.dev);

	return DISPLAY_VER(i915) >= 13 &&
	       crtc_state->uapi.async_flip &&
	       plane->async_flip;
}

static unsigned int
intel_plane_relative_data_rate(const struct intel_crtc_state *crtc_state,
			       const struct intel_plane_state *plane_state,
			       int color_plane)
{
	struct intel_plane *plane = to_intel_plane(plane_state->uapi.plane);
	const struct drm_framebuffer *fb = plane_state->hw.fb;
	int width, height;
	unsigned int rel_data_rate;

	if (!plane_state->uapi.visible)
		return 0;

	/*
	 * We calculate extra ddb based on ratio plane rate/total data rate
	 * in case, in some cases we should not allocate extra ddb for the plane,
	 * so do not count its data rate, if this is the case.
	 */
	if (use_min_ddb(crtc_state, plane))
		return 0;

	/*
	 * Src coordinates are already rotated by 270 degrees for
	 * the 90/270 degree plane rotation cases (to match the
	 * GTT mapping), hence no need to account for rotation here.
	 */
	width = drm_rect_width(&plane_state->uapi.src) >> 16;
	height = drm_rect_height(&plane_state->uapi.src) >> 16;

	/* UV plane does 1/2 pixel sub-sampling */
	if (color_plane == 1) {
		width /= 2;
		height /= 2;
	}

	rel_data_rate = width * height * fb->format->cpp[color_plane];

	if (plane->id == PLANE_CURSOR)
		return rel_data_rate;

	return intel_adjusted_rate(&plane_state->uapi.src,
				   &plane_state->uapi.dst,
				   rel_data_rate);
}

int intel_plane_calc_min_cdclk(struct intel_atomic_state *state,
			       struct intel_plane *plane,
			       bool *need_cdclk_calc)
{
	struct drm_i915_private *dev_priv = to_i915(plane->base.dev);
	const struct intel_plane_state *plane_state =
		intel_atomic_get_new_plane_state(state, plane);
	struct intel_crtc *crtc = to_intel_crtc(plane_state->hw.crtc);
	const struct intel_cdclk_state *cdclk_state;
	const struct intel_crtc_state *old_crtc_state;
	struct intel_crtc_state *new_crtc_state;

	if (!plane_state->uapi.visible || !plane->min_cdclk)
		return 0;

	old_crtc_state = intel_atomic_get_old_crtc_state(state, crtc);
	new_crtc_state = intel_atomic_get_new_crtc_state(state, crtc);

	new_crtc_state->min_cdclk[plane->id] =
		plane->min_cdclk(new_crtc_state, plane_state);

	/*
	 * No need to check against the cdclk state if
	 * the min cdclk for the plane doesn't increase.
	 *
	 * Ie. we only ever increase the cdclk due to plane
	 * requirements. This can reduce back and forth
	 * display blinking due to constant cdclk changes.
	 */
	if (new_crtc_state->min_cdclk[plane->id] <=
	    old_crtc_state->min_cdclk[plane->id])
		return 0;

	cdclk_state = intel_atomic_get_cdclk_state(state);
	if (IS_ERR(cdclk_state))
		return PTR_ERR(cdclk_state);

	/*
	 * No need to recalculate the cdclk state if
	 * the min cdclk for the pipe doesn't increase.
	 *
	 * Ie. we only ever increase the cdclk due to plane
	 * requirements. This can reduce back and forth
	 * display blinking due to constant cdclk changes.
	 */
	if (new_crtc_state->min_cdclk[plane->id] <=
	    cdclk_state->min_cdclk[crtc->pipe])
		return 0;

	drm_dbg_kms(&dev_priv->drm,
		    "[PLANE:%d:%s] min cdclk (%d kHz) > [CRTC:%d:%s] min cdclk (%d kHz)\n",
		    plane->base.base.id, plane->base.name,
		    new_crtc_state->min_cdclk[plane->id],
		    crtc->base.base.id, crtc->base.name,
		    cdclk_state->min_cdclk[crtc->pipe]);
	*need_cdclk_calc = true;

	return 0;
}

static void intel_plane_clear_hw_state(struct intel_plane_state *plane_state)
{
	if (plane_state->hw.fb)
		drm_framebuffer_put(plane_state->hw.fb);

	memset(&plane_state->hw, 0, sizeof(plane_state->hw));
}

static void mode_rect_to_rect(struct drm_rect *r,
			      const struct drm_mode_rect *clip)
{
	r->x1 = clip->x1 << 16;
	r->y1 = clip->y1 << 16;
	r->x2 = clip->x2 << 16;
	r->y2 = clip->y2 << 16;
}

static void sel_fetch_add_uapi_damage(struct intel_plane_state *plane_state,
				      const struct drm_plane_state *from_plane_state)
{
	struct drm_i915_private *i915 = to_i915(plane_state->uapi.plane->dev);
	const struct drm_property_blob *blob = from_plane_state->fb_damage_clips;
	const struct drm_mode_rect *clips = blob ? blob->data : NULL;
	int i, num_clips = blob ? blob->length / sizeof(clips[0]) : 0;

	if (!HAS_PSR2_SEL_FETCH(i915))
		return;

	if (!clips) {
		plane_state->sel_fetch.src = drm_plane_state_src(from_plane_state);
		return;
	}

	mode_rect_to_rect(&plane_state->sel_fetch.src, &clips[0]);

	for (i = 1; i < num_clips; i++) {
		struct drm_rect r;

		mode_rect_to_rect(&r, &clips[i]);

		drm_rect_union(&plane_state->sel_fetch.src, &r);
	}
}

void intel_plane_copy_uapi_to_hw_state(struct intel_plane_state *plane_state,
				       const struct intel_plane_state *from_plane_state,
				       struct intel_crtc *crtc)
{
	intel_plane_clear_hw_state(plane_state);

	/*
	 * For the bigjoiner slave uapi.crtc will point at
	 * the master crtc. So we explicitly assign the right
	 * slave crtc to hw.crtc. uapi.crtc!=NULL simply indicates
	 * the plane is logically enabled on the uapi level.
	 */
	plane_state->hw.crtc = from_plane_state->uapi.crtc ? &crtc->base : NULL;

	plane_state->hw.fb = from_plane_state->uapi.fb;
	if (plane_state->hw.fb)
		drm_framebuffer_get(plane_state->hw.fb);

	plane_state->hw.alpha = from_plane_state->uapi.alpha;
	plane_state->hw.pixel_blend_mode =
		from_plane_state->uapi.pixel_blend_mode;
	plane_state->hw.rotation = from_plane_state->uapi.rotation;
	plane_state->hw.color_encoding = from_plane_state->uapi.color_encoding;
	plane_state->hw.color_range = from_plane_state->uapi.color_range;
	plane_state->hw.scaling_filter = from_plane_state->uapi.scaling_filter;

	plane_state->uapi.src = drm_plane_state_src(&from_plane_state->uapi);
	plane_state->uapi.dst = drm_plane_state_dest(&from_plane_state->uapi);

	sel_fetch_add_uapi_damage(plane_state, &from_plane_state->uapi);
}

void intel_plane_copy_hw_state(struct intel_plane_state *plane_state,
			       const struct intel_plane_state *from_plane_state)
{
	intel_plane_clear_hw_state(plane_state);

	memcpy(&plane_state->hw, &from_plane_state->hw,
	       sizeof(plane_state->hw));

	if (plane_state->hw.fb)
		drm_framebuffer_get(plane_state->hw.fb);
}

void intel_plane_set_invisible(struct intel_crtc_state *crtc_state,
			       struct intel_plane_state *plane_state)
{
	struct intel_plane *plane = to_intel_plane(plane_state->uapi.plane);

	crtc_state->active_planes &= ~BIT(plane->id);
	crtc_state->scaled_planes &= ~BIT(plane->id);
	crtc_state->nv12_planes &= ~BIT(plane->id);
	crtc_state->c8_planes &= ~BIT(plane->id);
	crtc_state->async_flip_planes &= ~BIT(plane->id);
	crtc_state->data_rate[plane->id] = 0;
	crtc_state->data_rate_y[plane->id] = 0;
	crtc_state->rel_data_rate[plane->id] = 0;
	crtc_state->rel_data_rate_y[plane->id] = 0;
	crtc_state->min_cdclk[plane->id] = 0;

	plane_state->uapi.visible = false;
}

/* FIXME nuke when all wm code is atomic */
static bool intel_wm_need_update(const struct intel_plane_state *cur,
				 struct intel_plane_state *new)
{
	/* Update watermarks on tiling or size changes. */
	if (new->uapi.visible != cur->uapi.visible)
		return true;

	if (!cur->hw.fb || !new->hw.fb)
		return false;

	if (cur->hw.fb->modifier != new->hw.fb->modifier ||
	    cur->hw.rotation != new->hw.rotation ||
	    drm_rect_width(&new->uapi.src) != drm_rect_width(&cur->uapi.src) ||
	    drm_rect_height(&new->uapi.src) != drm_rect_height(&cur->uapi.src) ||
	    drm_rect_width(&new->uapi.dst) != drm_rect_width(&cur->uapi.dst) ||
	    drm_rect_height(&new->uapi.dst) != drm_rect_height(&cur->uapi.dst))
		return true;

	return false;
}

static bool intel_plane_is_scaled(const struct intel_plane_state *plane_state)
{
	int src_w = drm_rect_width(&plane_state->uapi.src) >> 16;
	int src_h = drm_rect_height(&plane_state->uapi.src) >> 16;
	int dst_w = drm_rect_width(&plane_state->uapi.dst);
	int dst_h = drm_rect_height(&plane_state->uapi.dst);

	return src_w != dst_w || src_h != dst_h;
}

static bool intel_plane_do_async_flip(struct intel_plane *plane,
				      const struct intel_crtc_state *old_crtc_state,
				      const struct intel_crtc_state *new_crtc_state)
{
	struct drm_i915_private *i915 = to_i915(plane->base.dev);

	if (!plane->async_flip)
		return false;

	if (!new_crtc_state->uapi.async_flip)
		return false;

	/*
	 * In platforms after DISPLAY13, we might need to override
	 * first async flip in order to change watermark levels
	 * as part of optimization.
	 * So for those, we are checking if this is a first async flip.
	 * For platforms earlier than DISPLAY13 we always do async flip.
	 */
	return DISPLAY_VER(i915) < 13 || old_crtc_state->uapi.async_flip;
}

static bool i9xx_must_disable_cxsr(const struct intel_crtc_state *new_crtc_state,
				   const struct intel_plane_state *old_plane_state,
				   const struct intel_plane_state *new_plane_state)
{
	struct intel_plane *plane = to_intel_plane(new_plane_state->uapi.plane);
	bool old_visible = old_plane_state->uapi.visible;
	bool new_visible = new_plane_state->uapi.visible;
	u32 old_ctl = old_plane_state->ctl;
	u32 new_ctl = new_plane_state->ctl;
	bool modeset, turn_on, turn_off;

	if (plane->id == PLANE_CURSOR)
		return false;

	modeset = intel_crtc_needs_modeset(new_crtc_state);
	turn_off = old_visible && (!new_visible || modeset);
	turn_on = new_visible && (!old_visible || modeset);

	/* Must disable CxSR around plane enable/disable */
	if (turn_on || turn_off)
		return true;

	if (!old_visible || !new_visible)
		return false;

	/*
	 * Most plane control register updates are blocked while in CxSR.
	 *
	 * Tiling mode is one exception where the primary plane can
	 * apparently handle it, whereas the sprites can not (the
	 * sprite issue being only relevant on VLV/CHV where CxSR
	 * is actually possible with a sprite enabled).
	 */
	if (plane->id == PLANE_PRIMARY) {
		old_ctl &= ~DISP_TILED;
		new_ctl &= ~DISP_TILED;
	}

	return old_ctl != new_ctl;
}

static bool plane_needs_full_fetch_old(const struct intel_plane_state *old_plane_state,
				       const struct intel_plane_state *new_plane_state)
{
	if (!old_plane_state->uapi.visible)
		return false;

	if (old_plane_state->uapi.visible != new_plane_state->uapi.visible)
		return true;

	if (!drm_rect_equals(&old_plane_state->uapi.dst,
			     &new_plane_state->uapi.dst))
		return true;

	return false;
}

static bool plane_needs_full_fetch_new(const struct intel_plane_state *old_plane_state,
				       const struct intel_plane_state *new_plane_state)
{
	if (!new_plane_state->uapi.visible)
		return false;

	if (old_plane_state->uapi.visible != new_plane_state->uapi.visible)
		return true;

	if (!drm_rect_equals(&old_plane_state->uapi.dst,
			     &new_plane_state->uapi.dst))
		return true;

	if (old_plane_state->ctl != new_plane_state->ctl)
		return true;

	if (old_plane_state->color_ctl != new_plane_state->color_ctl)
		return true;

	if (old_plane_state->hw.alpha != new_plane_state->hw.alpha)
		return true;

	if (memcmp(&old_plane_state->ckey, &new_plane_state->ckey,
		   sizeof(new_plane_state->ckey)))
		return true;

	return false;
}

static void sel_fetch_alignment(const struct intel_crtc_state *crtc_state,
				int *halign, int *valign)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);
	const struct drm_dsc_config *vdsc_cfg = &crtc_state->dsc.config;

	*halign = drm_rect_width(&crtc_state->pipe_src);

	/* ADLP aligns the SU region to vdsc slice height in case dsc is enabled */
	if (crtc_state->dsc.compression_enable &&
	    (IS_ALDERLAKE_P(dev_priv) || DISPLAY_VER(dev_priv) >= 14))
		*valign = vdsc_cfg->slice_height;
	else
		*valign = crtc_state->su_y_granularity;
}

static struct drm_rect
rel_pipe_src(const struct intel_crtc_state *crtc_state)
{
	struct drm_rect pipe_src = crtc_state->pipe_src;

	drm_rect_translate_to(&pipe_src, 0, 0);

	return pipe_src;
}

static void sel_fetch_align(struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915 = to_i915(crtc_state->uapi.crtc->dev);
	struct drm_rect pipe_src = rel_pipe_src(crtc_state);
	int halign, valign;

	sel_fetch_alignment(crtc_state, &halign, &valign);

	drm_rect_align(&crtc_state->sel_fetch, halign, valign);

	drm_rect_intersect(&crtc_state->sel_fetch, &pipe_src);

	/* Wa_14014971492 */
	if ((IS_DISPLAY_IP_STEP(i915, IP_VER(14, 0), STEP_A0, STEP_B0) ||
	     IS_ALDERLAKE_P(i915) || IS_TIGERLAKE(i915)) &&
	    crtc_state->splitter.enable && drm_rect_visible(&crtc_state->sel_fetch))
		crtc_state->sel_fetch.y1 = 0;
}

static enum plane_id
planar_uv_plane_id(const struct intel_plane_state *plane_state)
{
	struct intel_plane *plane = to_intel_plane(plane_state->uapi.plane);
	struct intel_plane *linked = plane_state->planar_linked_plane;

	return plane_state->planar_slave ? linked->id : plane->id;
}

static bool plane_needs_full_fetch(const struct intel_plane_state *plane_state)
{
	struct drm_i915_private *i915 = to_i915(plane_state->uapi.plane->dev);
	const struct drm_framebuffer *fb = plane_state->hw.fb;

	/* Scaler doesn't support selective fetch */
	if (intel_plane_is_scaled(plane_state))
		return true;

	/* SDR UV plane always need to use the scaler as well */
	if (intel_format_info_is_yuv_semiplanar(fb->format, fb->modifier) &&
	    !icl_is_hdr_plane(i915, planar_uv_plane_id(plane_state)))
		return true;

	return false;
}

static void intel_crtc_sel_fetch_accumulate_damage(struct intel_atomic_state *state,
						   struct intel_crtc *crtc)
{
	struct intel_crtc_state *new_crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	const struct intel_plane_state *old_plane_state;
	struct intel_plane_state *new_plane_state;
	struct intel_plane *plane;
	int i;

	/*
	 * TODO: Allow smaller selective fetch region with bigjoiner.
	 * All joined pipes will need the same selective fetch region
	 * as this is a transcoder level feature.
	 */
	if (new_crtc_state->pch_pfit.enabled ||
	    new_crtc_state->bigjoiner_pipes ||
	    intel_crtc_needs_modeset(new_crtc_state) ||
	    intel_crtc_needs_fastset(new_crtc_state) ||
	    intel_crtc_needs_color_update(new_crtc_state)) {
		new_crtc_state->sel_fetch = rel_pipe_src(new_crtc_state);
		return;
	}

	drm_rect_clear(&new_crtc_state->sel_fetch);

	for_each_oldnew_intel_plane_in_state(state, plane, old_plane_state, new_plane_state, i) {
		if (old_plane_state->hw.crtc != &crtc->base &&
		    new_plane_state->hw.crtc != &crtc->base)
			continue;

		drm_rect_union(&new_crtc_state->sel_fetch,
			       &new_plane_state->sel_fetch.dst);

		if (plane_needs_full_fetch_old(old_plane_state, new_plane_state))
			drm_rect_union(&new_crtc_state->sel_fetch, &old_plane_state->uapi.dst);

		if (plane_needs_full_fetch_new(old_plane_state, new_plane_state))
			drm_rect_union(&new_crtc_state->sel_fetch, &new_plane_state->uapi.dst);
	}

	sel_fetch_align(new_crtc_state);
}

static int check_src_coordinates(const struct intel_plane_state *plane_state,
				 const struct drm_rect *src);

static void plane_compute_sel_fetch_surface(struct intel_plane_state *plane_state,
					    int color_plane)
{
	int x, y;

	x = plane_state->view.color_plane[color_plane].x;
	y = plane_state->view.color_plane[color_plane].y;

	if (color_plane == 1) {
		x *= 2;
		y *= 2;
	}

	x += plane_state->sel_fetch.src.x1 >> 16;
	y += plane_state->sel_fetch.src.y1 >> 16;

	if (color_plane == 1) {
		x /= 2;
		y /= 2;
	}

	plane_state->sel_fetch.color_plane[color_plane].x = x;
	plane_state->sel_fetch.color_plane[color_plane].y = y;
}

static bool plane_compute_sel_fetch_src(struct intel_plane_state *plane_state)
{
	struct intel_plane *plane = to_intel_plane(plane_state->uapi.plane);
	const struct drm_framebuffer *fb = plane_state->hw.fb;

	if (!drm_rect_visible(&plane_state->sel_fetch.dst)) {
		drm_rect_clear(&plane_state->sel_fetch.dst);
		drm_rect_clear(&plane_state->sel_fetch.src);
		return false;
	}

	/* Cursor doesn't have registers for clipped coordinates */
	if (plane->id == PLANE_CURSOR || plane_needs_full_fetch(plane_state)) {
		plane_state->sel_fetch.src = plane_state->uapi.src;

		plane_state->sel_fetch.color_plane[0].x = plane_state->view.color_plane[0].x;
		plane_state->sel_fetch.color_plane[0].y = plane_state->view.color_plane[0].y;
		plane_state->sel_fetch.color_plane[1].x = plane_state->view.color_plane[1].x;
		plane_state->sel_fetch.color_plane[1].y = plane_state->view.color_plane[1].y;

		return false;
	}

	/*
	 * Translate the selective fetch destination coordinates
	 * to source coordinates (relative to the plane surface address).
	 */
	plane_state->sel_fetch.src = plane_state->sel_fetch.dst;

	drm_rect_translate(&plane_state->sel_fetch.src,
			   -plane_state->uapi.dst.x1, -plane_state->uapi.dst.y1);

	/*
	 * Rotate back to original fb orientation.
	 *
	 * width/height may need to be swapped here since we're
	 * getting them from the rotated dst coordinate space.
	 */
	if (drm_rotation_90_or_270(plane_state->hw.rotation))
		drm_rect_rotate_inv(&plane_state->sel_fetch.src,
				    drm_rect_height(&plane_state->uapi.dst),
				    drm_rect_width(&plane_state->uapi.dst),
				    plane_state->hw.rotation);
	else
		drm_rect_rotate_inv(&plane_state->sel_fetch.src,
				    drm_rect_width(&plane_state->uapi.dst),
				    drm_rect_height(&plane_state->uapi.dst),
				    plane_state->hw.rotation);

	/*
	 * We can assume 1:1 scaling here as all scaled planes will
	 * have taken the plane_needs_full_fetch() path anyway.
	 */
	drm_rect_init(&plane_state->sel_fetch.src,
		      plane_state->sel_fetch.src.x1 << 16,
		      plane_state->sel_fetch.src.y1 << 16,
		      drm_rect_width(&plane_state->sel_fetch.src) << 16,
		      drm_rect_height(&plane_state->sel_fetch.src) << 16);

	/*
	 * Rotate to match rotated GTT view.
	 *
	 * width/height need to be swapped here since we're
	 * getting them from the rotated src coordinate space.
	 */
	if (drm_rotation_90_or_270(plane_state->hw.rotation))
		drm_rect_rotate(&plane_state->sel_fetch.src,
				drm_rect_height(&plane_state->uapi.src),
				drm_rect_width(&plane_state->uapi.src),
				DRM_MODE_ROTATE_270);

	plane_compute_sel_fetch_surface(plane_state, 0);
	if (intel_format_info_is_yuv_semiplanar(fb->format, fb->modifier))
		plane_compute_sel_fetch_surface(plane_state, 1);

	/* Finally make it relative to the plane surface address */
	drm_rect_translate(&plane_state->sel_fetch.src,
			   plane_state->uapi.src.x1,
			   plane_state->uapi.src.y1);

	return check_src_coordinates(plane_state, &plane_state->sel_fetch.src) != 0;
}

static bool sel_fetch_process_plane(struct intel_crtc_state *crtc_state,
				    struct intel_plane_state *plane_state)
{
	struct intel_plane *plane = to_intel_plane(plane_state->uapi.plane);
	const struct drm_rect orig_sel_fetch = crtc_state->sel_fetch;

	if (!plane_state->uapi.visible && !plane_state->planar_slave) {
		drm_rect_clear(&plane_state->sel_fetch.dst);
		return false;
	}

	plane_state->sel_fetch.dst = plane_state->uapi.dst;
	drm_rect_intersect(&plane_state->sel_fetch.dst, &crtc_state->sel_fetch);

	if (!drm_rect_visible(&plane_state->sel_fetch.dst))
		return false;

	/*
	 * Cursor doesn't have registers for clipped coordinates, but
	 * hardware will automagically clip the cursor, so no need to
	 * extend the selective fetch area to cover the entire plane.
	 */
	if (plane->id == PLANE_CURSOR) {
		plane_state->sel_fetch.dst = plane_state->uapi.dst;
		return false;
	}

	if (!plane_needs_full_fetch(plane_state))
		return false;

	plane_state->sel_fetch.dst = plane_state->uapi.dst;
	drm_rect_union(&crtc_state->sel_fetch, &plane_state->sel_fetch.dst);

	sel_fetch_align(crtc_state);

	return !drm_rect_equals(&orig_sel_fetch, &crtc_state->sel_fetch);
}

int intel_crtc_compute_sel_fetch(struct intel_atomic_state *state,
				 struct intel_crtc *crtc);

int intel_crtc_compute_sel_fetch(struct intel_atomic_state *state,
				 struct intel_crtc *crtc)
{
	const struct intel_crtc_state *old_crtc_state =
		intel_atomic_get_old_crtc_state(state, crtc);
	struct intel_crtc_state *new_crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	const struct intel_plane_state *old_plane_state;
	struct intel_plane_state *new_plane_state;
	struct intel_plane *plane;
	bool changed;
	int ret, i;

	intel_crtc_sel_fetch_accumulate_damage(state, crtc);

	/*
	 * We'll need all the active planes in the state to
	 * figure out which ones overlap with the update region.
	 */
	ret = intel_crtc_add_planes_to_state(state, crtc,
					     old_crtc_state->active_planes |
					     new_crtc_state->active_planes);
	if (ret)
		return ret;

	do {
		do {
			changed = false;
			for_each_oldnew_intel_plane_in_state(state, plane, old_plane_state,
							     new_plane_state, i) {
				if (old_plane_state->hw.crtc != &crtc->base &&
				    new_plane_state->hw.crtc != &crtc->base)
					continue;

				if (sel_fetch_process_plane(new_crtc_state, new_plane_state)) {
					changed = true;
					break;
				}
			}
		} while (changed);

		for_each_oldnew_intel_plane_in_state(state, plane, old_plane_state,
						     new_plane_state, i) {
			if (old_plane_state->hw.crtc != &crtc->base &&
			    new_plane_state->hw.crtc != &crtc->base)
				continue;

			if (plane_compute_sel_fetch_src(new_plane_state)) {
				/*
				 * Just punt to a full update if we didn't end
				 * up with suitably aligned src coordinates.
				 */
				new_plane_state->sel_fetch.dst = new_plane_state->uapi.dst;
				changed = true;
				break;
			}

			if (!drm_rect_equals(&old_plane_state->sel_fetch.src,
					     &new_plane_state->sel_fetch.src) ||
			    !drm_rect_equals(&old_plane_state->sel_fetch.dst,
					     &new_plane_state->sel_fetch.dst))
				new_crtc_state->update_planes |= BIT(plane->id);
		}
	} while (changed);

	return 0;
}

static int intel_plane_atomic_calc_changes(const struct intel_crtc_state *old_crtc_state,
					   struct intel_crtc_state *new_crtc_state,
					   const struct intel_plane_state *old_plane_state,
					   struct intel_plane_state *new_plane_state)
{
	struct intel_crtc *crtc = to_intel_crtc(new_crtc_state->uapi.crtc);
	struct intel_plane *plane = to_intel_plane(new_plane_state->uapi.plane);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	bool mode_changed = intel_crtc_needs_modeset(new_crtc_state);
	bool was_crtc_enabled = old_crtc_state->hw.active;
	bool is_crtc_enabled = new_crtc_state->hw.active;
	bool turn_off, turn_on, visible, was_visible;
	int ret;

	if (DISPLAY_VER(dev_priv) >= 9 && plane->id != PLANE_CURSOR) {
		ret = skl_update_scaler_plane(new_crtc_state, new_plane_state);
		if (ret)
			return ret;
	}

	was_visible = old_plane_state->uapi.visible;
	visible = new_plane_state->uapi.visible;

	if (!was_crtc_enabled && drm_WARN_ON(&dev_priv->drm, was_visible))
		was_visible = false;

	/*
	 * Visibility is calculated as if the crtc was on, but
	 * after scaler setup everything depends on it being off
	 * when the crtc isn't active.
	 *
	 * FIXME this is wrong for watermarks. Watermarks should also
	 * be computed as if the pipe would be active. Perhaps move
	 * per-plane wm computation to the .check_plane() hook, and
	 * only combine the results from all planes in the current place?
	 */
	if (!is_crtc_enabled) {
		intel_plane_set_invisible(new_crtc_state, new_plane_state);
		visible = false;
	}

	if (!was_visible && !visible)
		return 0;

	turn_off = was_visible && (!visible || mode_changed);
	turn_on = visible && (!was_visible || mode_changed);

	drm_dbg_atomic(&dev_priv->drm,
		       "[CRTC:%d:%s] with [PLANE:%d:%s] visible %i -> %i, off %i, on %i, ms %i\n",
		       crtc->base.base.id, crtc->base.name,
		       plane->base.base.id, plane->base.name,
		       was_visible, visible,
		       turn_off, turn_on, mode_changed);

	if (turn_on) {
		if (DISPLAY_VER(dev_priv) < 5 && !IS_G4X(dev_priv))
			new_crtc_state->update_wm_pre = true;
	} else if (turn_off) {
		if (DISPLAY_VER(dev_priv) < 5 && !IS_G4X(dev_priv))
			new_crtc_state->update_wm_post = true;
	} else if (intel_wm_need_update(old_plane_state, new_plane_state)) {
		if (DISPLAY_VER(dev_priv) < 5 && !IS_G4X(dev_priv)) {
			/* FIXME bollocks */
			new_crtc_state->update_wm_pre = true;
			new_crtc_state->update_wm_post = true;
		}
	}

	if (visible || was_visible)
		new_crtc_state->fb_bits |= plane->frontbuffer_bit;

	if (HAS_GMCH(dev_priv) &&
	    i9xx_must_disable_cxsr(new_crtc_state, old_plane_state, new_plane_state))
		new_crtc_state->disable_cxsr = true;

	/*
	 * ILK/SNB DVSACNTR/Sprite Enable
	 * IVB SPR_CTL/Sprite Enable
	 * "When in Self Refresh Big FIFO mode, a write to enable the
	 *  plane will be internally buffered and delayed while Big FIFO
	 *  mode is exiting."
	 *
	 * Which means that enabling the sprite can take an extra frame
	 * when we start in big FIFO mode (LP1+). Thus we need to drop
	 * down to LP0 and wait for vblank in order to make sure the
	 * sprite gets enabled on the next vblank after the register write.
	 * Doing otherwise would risk enabling the sprite one frame after
	 * we've already signalled flip completion. We can resume LP1+
	 * once the sprite has been enabled.
	 *
	 *
	 * WaCxSRDisabledForSpriteScaling:ivb
	 * IVB SPR_SCALE/Scaling Enable
	 * "Low Power watermarks must be disabled for at least one
	 *  frame before enabling sprite scaling, and kept disabled
	 *  until sprite scaling is disabled."
	 *
	 * ILK/SNB DVSASCALE/Scaling Enable
	 * "When in Self Refresh Big FIFO mode, scaling enable will be
	 *  masked off while Big FIFO mode is exiting."
	 *
	 * Despite the w/a only being listed for IVB we assume that
	 * the ILK/SNB note has similar ramifications, hence we apply
	 * the w/a on all three platforms.
	 *
	 * With experimental results seems this is needed also for primary
	 * plane, not only sprite plane.
	 */
	if (plane->id != PLANE_CURSOR &&
	    (IS_IRONLAKE(dev_priv) || IS_SANDYBRIDGE(dev_priv) ||
	     IS_IVYBRIDGE(dev_priv)) &&
	    (turn_on || (!intel_plane_is_scaled(old_plane_state) &&
			 intel_plane_is_scaled(new_plane_state))))
		new_crtc_state->disable_lp_wm = true;

	if (intel_plane_do_async_flip(plane, old_crtc_state, new_crtc_state)) {
		new_crtc_state->do_async_flip = true;
		new_crtc_state->async_flip_planes |= BIT(plane->id);
	}

	return 0;
}

int intel_plane_atomic_check_with_state(const struct intel_crtc_state *old_crtc_state,
					struct intel_crtc_state *new_crtc_state,
					const struct intel_plane_state *old_plane_state,
					struct intel_plane_state *new_plane_state)
{
	struct intel_plane *plane = to_intel_plane(new_plane_state->uapi.plane);
	const struct drm_framebuffer *fb = new_plane_state->hw.fb;
	int ret;

	intel_plane_set_invisible(new_crtc_state, new_plane_state);
	new_crtc_state->enabled_planes &= ~BIT(plane->id);

	if (!new_plane_state->hw.crtc && !old_plane_state->hw.crtc)
		return 0;

	ret = plane->check_plane(new_crtc_state, new_plane_state);
	if (ret)
		return ret;

	if (fb)
		new_crtc_state->enabled_planes |= BIT(plane->id);

	/* FIXME pre-g4x don't work like this */
	if (new_plane_state->uapi.visible)
		new_crtc_state->active_planes |= BIT(plane->id);

	if (new_plane_state->uapi.visible &&
	    intel_plane_is_scaled(new_plane_state))
		new_crtc_state->scaled_planes |= BIT(plane->id);

	if (new_plane_state->uapi.visible &&
	    intel_format_info_is_yuv_semiplanar(fb->format, fb->modifier))
		new_crtc_state->nv12_planes |= BIT(plane->id);

	if (new_plane_state->uapi.visible &&
	    fb->format->format == DRM_FORMAT_C8)
		new_crtc_state->c8_planes |= BIT(plane->id);

	if (new_plane_state->uapi.visible || old_plane_state->uapi.visible)
		new_crtc_state->update_planes |= BIT(plane->id);

	if (new_plane_state->uapi.visible &&
	    intel_format_info_is_yuv_semiplanar(fb->format, fb->modifier)) {
		new_crtc_state->data_rate_y[plane->id] =
			intel_plane_data_rate(new_crtc_state, new_plane_state, 0);
		new_crtc_state->data_rate[plane->id] =
			intel_plane_data_rate(new_crtc_state, new_plane_state, 1);

		new_crtc_state->rel_data_rate_y[plane->id] =
			intel_plane_relative_data_rate(new_crtc_state,
						       new_plane_state, 0);
		new_crtc_state->rel_data_rate[plane->id] =
			intel_plane_relative_data_rate(new_crtc_state,
						       new_plane_state, 1);
	} else if (new_plane_state->uapi.visible) {
		new_crtc_state->data_rate[plane->id] =
			intel_plane_data_rate(new_crtc_state, new_plane_state, 0);

		new_crtc_state->rel_data_rate[plane->id] =
			intel_plane_relative_data_rate(new_crtc_state,
						       new_plane_state, 0);
	}

	return intel_plane_atomic_calc_changes(old_crtc_state, new_crtc_state,
					       old_plane_state, new_plane_state);
}

static struct intel_plane *
intel_crtc_get_plane(struct intel_crtc *crtc, enum plane_id plane_id)
{
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);
	struct intel_plane *plane;

	for_each_intel_plane_on_crtc(&i915->drm, crtc, plane) {
		if (plane->id == plane_id)
			return plane;
	}

	return NULL;
}

int intel_plane_atomic_check(struct intel_atomic_state *state,
			     struct intel_plane *plane)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct intel_plane_state *new_plane_state =
		intel_atomic_get_new_plane_state(state, plane);
	const struct intel_plane_state *old_plane_state =
		intel_atomic_get_old_plane_state(state, plane);
	const struct intel_plane_state *new_master_plane_state;
	struct intel_crtc *crtc = intel_crtc_for_pipe(i915, plane->pipe);
	const struct intel_crtc_state *old_crtc_state =
		intel_atomic_get_old_crtc_state(state, crtc);
	struct intel_crtc_state *new_crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);

	if (new_crtc_state && intel_crtc_is_bigjoiner_slave(new_crtc_state)) {
		struct intel_crtc *master_crtc =
			intel_master_crtc(new_crtc_state);
		struct intel_plane *master_plane =
			intel_crtc_get_plane(master_crtc, plane->id);

		new_master_plane_state =
			intel_atomic_get_new_plane_state(state, master_plane);
	} else {
		new_master_plane_state = new_plane_state;
	}

	intel_plane_copy_uapi_to_hw_state(new_plane_state,
					  new_master_plane_state,
					  crtc);

	new_plane_state->uapi.visible = false;
	if (!new_crtc_state)
		return 0;

	return intel_plane_atomic_check_with_state(old_crtc_state,
						   new_crtc_state,
						   old_plane_state,
						   new_plane_state);
}

static struct intel_plane *
skl_next_plane_to_commit(struct intel_atomic_state *state,
			 struct intel_crtc *crtc,
			 struct skl_ddb_entry ddb[I915_MAX_PLANES],
			 struct skl_ddb_entry ddb_y[I915_MAX_PLANES],
			 unsigned int *update_mask)
{
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	struct intel_plane_state __maybe_unused *plane_state;
	struct intel_plane *plane;
	int i;

	if (*update_mask == 0)
		return NULL;

	for_each_new_intel_plane_in_state(state, plane, plane_state, i) {
		enum plane_id plane_id = plane->id;

		if (crtc->pipe != plane->pipe ||
		    !(*update_mask & BIT(plane_id)))
			continue;

		if (skl_ddb_allocation_overlaps(&crtc_state->wm.skl.plane_ddb[plane_id],
						ddb, I915_MAX_PLANES, plane_id) ||
		    skl_ddb_allocation_overlaps(&crtc_state->wm.skl.plane_ddb_y[plane_id],
						ddb_y, I915_MAX_PLANES, plane_id))
			continue;

		*update_mask &= ~BIT(plane_id);
		ddb[plane_id] = crtc_state->wm.skl.plane_ddb[plane_id];
		ddb_y[plane_id] = crtc_state->wm.skl.plane_ddb_y[plane_id];

		return plane;
	}

	/* should never happen */
	drm_WARN_ON(state->base.dev, 1);

	return NULL;
}

void intel_plane_update_noarm(struct intel_plane *plane,
			      const struct intel_crtc_state *crtc_state,
			      const struct intel_plane_state *plane_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	trace_intel_plane_update_noarm(plane, crtc);

	if (plane->update_noarm)
		plane->update_noarm(plane, crtc_state, plane_state);
}

void intel_plane_update_arm(struct intel_plane *plane,
			    const struct intel_crtc_state *crtc_state,
			    const struct intel_plane_state *plane_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	trace_intel_plane_update_arm(plane, crtc);

	if (crtc_state->do_async_flip && plane->async_flip)
		plane->async_flip(plane, crtc_state, plane_state, true);
	else
		plane->update_arm(plane, crtc_state, plane_state);
}

void intel_plane_disable_arm(struct intel_plane *plane,
			     const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	trace_intel_plane_disable_arm(plane, crtc);
	plane->disable_arm(plane, crtc_state);
}

void intel_crtc_planes_update_noarm(struct intel_atomic_state *state,
				    struct intel_crtc *crtc)
{
	struct intel_crtc_state *new_crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	u32 update_mask = new_crtc_state->update_planes;
	struct intel_plane_state *new_plane_state;
	struct intel_plane *plane;
	int i;

	if (new_crtc_state->do_async_flip)
		return;

	/*
	 * Since we only write non-arming registers here,
	 * the order does not matter even for skl+.
	 */
	for_each_new_intel_plane_in_state(state, plane, new_plane_state, i) {
		if (crtc->pipe != plane->pipe ||
		    !(update_mask & BIT(plane->id)))
			continue;

		/* TODO: for mailbox updates this should be skipped */
		if (new_plane_state->uapi.visible ||
		    new_plane_state->planar_slave)
			intel_plane_update_noarm(plane, new_crtc_state, new_plane_state);
	}
}

static void skl_crtc_planes_update_arm(struct intel_atomic_state *state,
				       struct intel_crtc *crtc)
{
	struct intel_crtc_state *old_crtc_state =
		intel_atomic_get_old_crtc_state(state, crtc);
	struct intel_crtc_state *new_crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	struct skl_ddb_entry ddb[I915_MAX_PLANES];
	struct skl_ddb_entry ddb_y[I915_MAX_PLANES];
	u32 update_mask = new_crtc_state->update_planes;
	struct intel_plane *plane;

	memcpy(ddb, old_crtc_state->wm.skl.plane_ddb,
	       sizeof(old_crtc_state->wm.skl.plane_ddb));
	memcpy(ddb_y, old_crtc_state->wm.skl.plane_ddb_y,
	       sizeof(old_crtc_state->wm.skl.plane_ddb_y));

	while ((plane = skl_next_plane_to_commit(state, crtc, ddb, ddb_y, &update_mask))) {
		struct intel_plane_state *new_plane_state =
			intel_atomic_get_new_plane_state(state, plane);

		/*
		 * TODO: for mailbox updates intel_plane_update_noarm()
		 * would have to be called here as well.
		 */
		if (new_plane_state->uapi.visible ||
		    new_plane_state->planar_slave)
			intel_plane_update_arm(plane, new_crtc_state, new_plane_state);
		else
			intel_plane_disable_arm(plane, new_crtc_state);
	}
}

static void i9xx_crtc_planes_update_arm(struct intel_atomic_state *state,
					struct intel_crtc *crtc)
{
	struct intel_crtc_state *new_crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	u32 update_mask = new_crtc_state->update_planes;
	struct intel_plane_state *new_plane_state;
	struct intel_plane *plane;
	int i;

	for_each_new_intel_plane_in_state(state, plane, new_plane_state, i) {
		if (crtc->pipe != plane->pipe ||
		    !(update_mask & BIT(plane->id)))
			continue;

		/*
		 * TODO: for mailbox updates intel_plane_update_noarm()
		 * would have to be called here as well.
		 */
		if (new_plane_state->uapi.visible)
			intel_plane_update_arm(plane, new_crtc_state, new_plane_state);
		else
			intel_plane_disable_arm(plane, new_crtc_state);
	}
}

void intel_crtc_planes_update_arm(struct intel_atomic_state *state,
				  struct intel_crtc *crtc)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);

	if (DISPLAY_VER(i915) >= 9)
		skl_crtc_planes_update_arm(state, crtc);
	else
		i9xx_crtc_planes_update_arm(state, crtc);
}

static void intel_plane_calc_damage(struct intel_plane_state *plane_state,
				    int hscale, int vscale)
{
	struct drm_i915_private *i915 = to_i915(plane_state->uapi.plane->dev);

	if (!HAS_PSR2_SEL_FETCH(i915))
		return;

	if (!plane_state->uapi.visible)
		return;

	/* Translate damage to plane output coordinates */
	plane_state->sel_fetch.dst = plane_state->sel_fetch.src;

	drm_rect_translate(&plane_state->sel_fetch.dst,
			   -plane_state->uapi.src.x1,
			   -plane_state->uapi.src.y1);

	drm_rect_rotate(&plane_state->sel_fetch.dst,
			drm_rect_width(&plane_state->uapi.src),
			drm_rect_height(&plane_state->uapi.src),
			plane_state->hw.rotation);

	plane_state->sel_fetch.dst.x1 = plane_state->sel_fetch.dst.x1 / hscale;
	plane_state->sel_fetch.dst.x2 = DIV_ROUND_UP(plane_state->sel_fetch.dst.x2, hscale);
	plane_state->sel_fetch.dst.y1 = plane_state->sel_fetch.dst.y1 / vscale;
	plane_state->sel_fetch.dst.y2 = DIV_ROUND_UP(plane_state->sel_fetch.dst.y2, vscale);

	drm_rect_translate(&plane_state->sel_fetch.dst,
			   plane_state->uapi.dst.x1,
			   plane_state->uapi.dst.y1);

	drm_rect_intersect(&plane_state->sel_fetch.dst,
			   &plane_state->uapi.dst);
}

int intel_atomic_plane_check_clipping(struct intel_plane_state *plane_state,
				      struct intel_crtc_state *crtc_state,
				      int min_scale, int max_scale,
				      bool can_position)
{
	struct drm_i915_private *i915 = to_i915(plane_state->uapi.plane->dev);
	struct drm_framebuffer *fb = plane_state->hw.fb;
	struct drm_rect *src = &plane_state->uapi.src;
	struct drm_rect *dst = &plane_state->uapi.dst;
	const struct drm_rect *clip = &crtc_state->pipe_src;
	unsigned int rotation = plane_state->hw.rotation;
	int hscale, vscale;

	if (!fb) {
		plane_state->uapi.visible = false;
		return 0;
	}

	drm_rect_rotate(src, fb->width << 16, fb->height << 16, rotation);

	/* Check scaling */
	hscale = drm_rect_calc_hscale(src, dst, min_scale, max_scale);
	vscale = drm_rect_calc_vscale(src, dst, min_scale, max_scale);
	if (hscale < 0 || vscale < 0) {
		drm_dbg_kms(&i915->drm, "Invalid scaling of plane\n");
		drm_rect_debug_print("src: ", src, true);
		drm_rect_debug_print("dst: ", dst, false);
		return -ERANGE;
	}

	/*
	 * FIXME: This might need further adjustment for seamless scaling
	 * with phase information, for the 2p2 and 2p1 scenarios.
	 */
	plane_state->uapi.visible = drm_rect_clip_scaled(src, dst, clip);

	drm_rect_rotate_inv(src, fb->width << 16, fb->height << 16, rotation);

	if (!can_position && plane_state->uapi.visible &&
	    !drm_rect_equals(dst, clip)) {
		drm_dbg_kms(&i915->drm, "Plane must cover entire CRTC\n");
		drm_rect_debug_print("dst: ", dst, false);
		drm_rect_debug_print("clip: ", clip, false);
		return -EINVAL;
	}

	/* final plane coordinates will be relative to the plane's pipe */
	drm_rect_translate(dst, -clip->x1, -clip->y1);

	intel_plane_calc_damage(plane_state, hscale, vscale);

	return 0;
}

static void truncate_subpixel_coords(struct drm_rect *r)
{
	int x, y, w, h;

	x = r->x1 >> 16;
	y = r->y1 >> 16;
	w = drm_rect_width(r) >> 16;
	h = drm_rect_height(r) >> 16;

	drm_rect_init(r, x << 16, y << 16, w << 16, h << 16);
}

static int check_src_coordinates(const struct intel_plane_state *plane_state,
				 const struct drm_rect *src)
{
	struct drm_i915_private *i915 = to_i915(plane_state->uapi.plane->dev);
	const struct drm_framebuffer *fb = plane_state->hw.fb;
	u32 src_x, src_y, src_w, src_h, hsub, vsub;
	bool rotated = drm_rotation_90_or_270(plane_state->hw.rotation);

	/*
	 * FIXME hsub/vsub vs. block size is a mess. Pre-tgl CCS
	 * abuses hsub/vsub so we can't use them here. But as they
	 * are limited to 32bpp RGB formats we don't actually need
	 * to check anything.
	 */
	if (fb->modifier == I915_FORMAT_MOD_Y_TILED_CCS ||
	    fb->modifier == I915_FORMAT_MOD_Yf_TILED_CCS)
		return 0;

	src_x = src->x1 >> 16;
	src_w = drm_rect_width(src) >> 16;
	src_y = src->y1 >> 16;
	src_h = drm_rect_height(src) >> 16;

	if (fb->format->format == DRM_FORMAT_RGB565 && rotated) {
		hsub = 2;
		vsub = 2;
	} else if (DISPLAY_VER(i915) >= 20 &&
		   intel_format_info_is_yuv_semiplanar(fb->format, fb->modifier)) {
		/*
		 * This allows NV12 and P0xx formats to have odd size and/or odd
		 * source coordinates on DISPLAY_VER(i915) >= 20
		 */
		hsub = 1;
		vsub = 1;
	} else {
		hsub = fb->format->hsub;
		vsub = fb->format->vsub;
	}

	if (rotated)
		hsub = vsub = max(hsub, vsub);

	if (src_x % hsub || src_w % hsub) {
		drm_dbg_kms(&i915->drm, "src x/w (%u, %u) must be a multiple of %u (rotated: %s)\n",
			    src_x, src_w, hsub, str_yes_no(rotated));
		return -EINVAL;
	}

	if (src_y % vsub || src_h % vsub) {
		drm_dbg_kms(&i915->drm, "src y/h (%u, %u) must be a multiple of %u (rotated: %s)\n",
			    src_y, src_h, vsub, str_yes_no(rotated));
		return -EINVAL;
	}

	return 0;
}

int intel_plane_check_src_coordinates(struct intel_plane_state *plane_state)
{
	const struct drm_framebuffer *fb = plane_state->hw.fb;

	/*
	 * FIXME hsub/vsub vs. block size is a mess. Pre-tgl CCS
	 * abuses hsub/vsub so we can't use them here. But as they
	 * are limited to 32bpp RGB formats we don't actually need
	 * to check anything.
	 */
	if (fb->modifier == I915_FORMAT_MOD_Y_TILED_CCS ||
	    fb->modifier == I915_FORMAT_MOD_Yf_TILED_CCS)
		return 0;

	/*
	 * Hardware doesn't handle subpixel coordinates,
	 * Adjust to (macro)pixel boundary, but be careful not to
	 * increase the source viewport size, because that could
	 * push the downscaling factor out of bounds.
	 *
	 * TODO: On SKL+ scaler could be used to handle this.
	 */
	truncate_subpixel_coords(&plane_state->uapi.src);

	return check_src_coordinates(plane_state, &plane_state->uapi.src);
}

static int add_dma_resv_fences(struct dma_resv *resv,
			       struct drm_plane_state *new_plane_state)
{
	struct dma_fence *fence = dma_fence_get(new_plane_state->fence);
	struct dma_fence *new;
	int ret;

	ret = dma_resv_get_singleton(resv, dma_resv_usage_rw(false), &new);
	if (ret)
		goto error;

	if (new && fence) {
		struct dma_fence_chain *chain = dma_fence_chain_alloc();

		if (!chain) {
			ret = -ENOMEM;
			goto error;
		}

		dma_fence_chain_init(chain, fence, new, 1);
		fence = &chain->base;

	} else if (new) {
		fence = new;
	}

	dma_fence_put(new_plane_state->fence);
	new_plane_state->fence = fence;
	return 0;

error:
	dma_fence_put(fence);
	return ret;
}

/**
 * intel_prepare_plane_fb - Prepare fb for usage on plane
 * @_plane: drm plane to prepare for
 * @_new_plane_state: the plane state being prepared
 *
 * Prepares a framebuffer for usage on a display plane.  Generally this
 * involves pinning the underlying object and updating the frontbuffer tracking
 * bits.  Some older platforms need special physical address handling for
 * cursor planes.
 *
 * Returns 0 on success, negative error code on failure.
 */
static int
intel_prepare_plane_fb(struct drm_plane *_plane,
		       struct drm_plane_state *_new_plane_state)
{
	struct i915_sched_attr attr = { .priority = I915_PRIORITY_DISPLAY };
	struct intel_plane *plane = to_intel_plane(_plane);
	struct intel_plane_state *new_plane_state =
		to_intel_plane_state(_new_plane_state);
	struct intel_atomic_state *state =
		to_intel_atomic_state(new_plane_state->uapi.state);
	struct drm_i915_private *dev_priv = to_i915(plane->base.dev);
	struct intel_plane_state *old_plane_state =
		intel_atomic_get_old_plane_state(state, plane);
	struct drm_i915_gem_object *obj = intel_fb_obj(new_plane_state->hw.fb);
	struct drm_i915_gem_object *old_obj = intel_fb_obj(old_plane_state->hw.fb);
	int ret;

	if (old_obj) {
		const struct intel_crtc_state *new_crtc_state =
			intel_atomic_get_new_crtc_state(state,
							to_intel_crtc(old_plane_state->hw.crtc));

		/* Big Hammer, we also need to ensure that any pending
		 * MI_WAIT_FOR_EVENT inside a user batch buffer on the
		 * current scanout is retired before unpinning the old
		 * framebuffer. Note that we rely on userspace rendering
		 * into the buffer attached to the pipe they are waiting
		 * on. If not, userspace generates a GPU hang with IPEHR
		 * point to the MI_WAIT_FOR_EVENT.
		 *
		 * This should only fail upon a hung GPU, in which case we
		 * can safely continue.
		 */
		if (new_crtc_state && intel_crtc_needs_modeset(new_crtc_state)) {
			ret = add_dma_resv_fences(intel_bo_to_drm_bo(old_obj)->resv,
						  &new_plane_state->uapi);
			if (ret < 0)
				return ret;
		}
	}

	if (!obj)
		return 0;

	ret = intel_plane_pin_fb(new_plane_state);
	if (ret)
		return ret;

	ret = drm_gem_plane_helper_prepare_fb(&plane->base, &new_plane_state->uapi);
	if (ret < 0)
		goto unpin_fb;

	if (new_plane_state->uapi.fence) {
		i915_gem_fence_wait_priority(new_plane_state->uapi.fence,
					     &attr);

		intel_display_rps_boost_after_vblank(new_plane_state->hw.crtc,
						     new_plane_state->uapi.fence);
	}

	/*
	 * We declare pageflips to be interactive and so merit a small bias
	 * towards upclocking to deliver the frame on time. By only changing
	 * the RPS thresholds to sample more regularly and aim for higher
	 * clocks we can hopefully deliver low power workloads (like kodi)
	 * that are not quite steady state without resorting to forcing
	 * maximum clocks following a vblank miss (see do_rps_boost()).
	 */
	intel_display_rps_mark_interactive(dev_priv, state, true);

	return 0;

unpin_fb:
	intel_plane_unpin_fb(new_plane_state);

	return ret;
}

/**
 * intel_cleanup_plane_fb - Cleans up an fb after plane use
 * @plane: drm plane to clean up for
 * @_old_plane_state: the state from the previous modeset
 *
 * Cleans up a framebuffer that has just been removed from a plane.
 */
static void
intel_cleanup_plane_fb(struct drm_plane *plane,
		       struct drm_plane_state *_old_plane_state)
{
	struct intel_plane_state *old_plane_state =
		to_intel_plane_state(_old_plane_state);
	struct intel_atomic_state *state =
		to_intel_atomic_state(old_plane_state->uapi.state);
	struct drm_i915_private *dev_priv = to_i915(plane->dev);
	struct drm_i915_gem_object *obj = intel_fb_obj(old_plane_state->hw.fb);

	if (!obj)
		return;

	intel_display_rps_mark_interactive(dev_priv, state, false);

	/* Should only be called after a successful intel_prepare_plane_fb()! */
	intel_plane_unpin_fb(old_plane_state);
}

static const struct drm_plane_helper_funcs intel_plane_helper_funcs = {
	.prepare_fb = intel_prepare_plane_fb,
	.cleanup_fb = intel_cleanup_plane_fb,
};

void intel_plane_helper_add(struct intel_plane *plane)
{
	drm_plane_helper_add(&plane->base, &intel_plane_helper_funcs);
}
