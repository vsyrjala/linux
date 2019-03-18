// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <drm/drm_atomic_state_helper.h>

#include "intel_drv.h"

static unsigned int intel_bw_crtc_num_active_planes(const struct intel_crtc_state *crtc_state)
{
	/*
	 * We assume cursors are small enough
	 * to not not cause bandwidth problems.
	 */
	return hweight8(crtc_state->active_planes & ~BIT(PLANE_CURSOR));
}

static unsigned int intel_bw_crtc_data_rate(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	unsigned int data_rate = 0;
	enum plane_id plane_id;

	for_each_plane_id_on_crtc(crtc, plane_id) {
		/*
		 * We assume cursors are small enough
		 * to not not cause bandwidth problems.
		 */
		if (plane_id == PLANE_CURSOR)
			continue;

		data_rate += crtc_state->data_rate[plane_id];
	}

	return data_rate;
}

void intel_bw_crtc_update(struct intel_bw_state *bw_state,
			  const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);

	bw_state->data_rate[crtc->pipe] =
		intel_bw_crtc_data_rate(crtc_state);
	bw_state->num_active_planes[crtc->pipe] =
		intel_bw_crtc_num_active_planes(crtc_state);

	DRM_DEBUG_KMS("pipe %c data rate %u num active planes %u\n",
		      pipe_name(crtc->pipe),
		      bw_state->data_rate[crtc->pipe],
		      bw_state->num_active_planes[crtc->pipe]);
}

static unsigned int intel_bw_num_active_planes(struct drm_i915_private *dev_priv,
					       const struct intel_bw_state *bw_state)
{
	unsigned int num_active_planes = 0;
	enum pipe pipe;

	for_each_pipe(dev_priv, pipe) {
		num_active_planes += bw_state->num_active_planes[pipe];

		DRM_DEBUG_KMS("pipe %c num active planes %u\n",
			      pipe_name(pipe),
			      bw_state->num_active_planes[pipe]);
	}

	return num_active_planes;
}

static unsigned int intel_bw_data_rate(struct drm_i915_private *dev_priv,
				       const struct intel_bw_state *bw_state)
{
	unsigned int data_rate = 0;
	enum pipe pipe;

	for_each_pipe(dev_priv, pipe) {
		data_rate += bw_state->data_rate[pipe];

		DRM_DEBUG_KMS("pipe %c data rate %u\n",
			      pipe_name(pipe),
			      bw_state->data_rate[pipe]);
	}

	return data_rate;
}

int intel_bw_atomic_check(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_crtc_state *new_crtc_state, *old_crtc_state;
	struct intel_bw_state *bw_state = NULL;
	unsigned int data_rate, max_data_rate;
	unsigned int num_active_planes;
	struct intel_crtc *crtc;
	int i;

	/* FIXME earlier gens need some checks too */
	if (INTEL_GEN(dev_priv) < 11)
		return 0;

	for_each_oldnew_intel_crtc_in_state(state, crtc, old_crtc_state,
					    new_crtc_state, i) {
		unsigned int old_data_rate =
			intel_bw_crtc_data_rate(old_crtc_state);
		unsigned int new_data_rate =
			intel_bw_crtc_data_rate(new_crtc_state);
		unsigned int old_active_planes =
			intel_bw_crtc_num_active_planes(old_crtc_state);
		unsigned int new_active_planes =
			intel_bw_crtc_num_active_planes(new_crtc_state);

		/*
		 * Avoid locking the bw state when
		 * nothing significant has changed.
		 */
		if (old_data_rate == new_data_rate &&
		    old_active_planes == new_active_planes)
			continue;

		bw_state  = intel_atomic_get_bw_state(state);
		if (IS_ERR(bw_state))
			return PTR_ERR(bw_state);

		bw_state->data_rate[crtc->pipe] = new_data_rate;
		bw_state->num_active_planes[crtc->pipe] = new_active_planes;

		DRM_DEBUG_KMS("pipe %c data rate %u num active planes %u\n",
			      pipe_name(crtc->pipe),
			      bw_state->data_rate[crtc->pipe],
			      bw_state->num_active_planes[crtc->pipe]);
	}

	if (!bw_state)
		return 0;

	data_rate = intel_bw_data_rate(dev_priv, bw_state);
	num_active_planes = intel_bw_num_active_planes(dev_priv, bw_state);

	max_data_rate = intel_max_data_rate(dev_priv, num_active_planes);

	data_rate = DIV_ROUND_UP(data_rate, 1000);

	if (data_rate > max_data_rate) {
		DRM_DEBUG_KMS("Bandwidth %u MB/s exceeds max available %d MB/s (%d active planes)\n",
			      data_rate, max_data_rate, num_active_planes);
		return -EINVAL;
	}

	return 0;
}

static struct drm_private_state *intel_bw_duplicate_state(struct drm_private_obj *obj)
{
	struct intel_bw_state *state;

	state = kmemdup(obj->state, sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_private_obj_duplicate_state(obj, &state->base);

	return &state->base;
}

static void intel_bw_destroy_state(struct drm_private_obj *obj,
				   struct drm_private_state *state)
{
	kfree(state);
}

static const struct drm_private_state_funcs intel_bw_funcs = {
	.atomic_duplicate_state = intel_bw_duplicate_state,
	.atomic_destroy_state = intel_bw_destroy_state,
};

int intel_bw_init(struct drm_i915_private *dev_priv)
{
	struct intel_bw_state *state;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	drm_atomic_private_obj_init(&dev_priv->drm, &dev_priv->bw_obj,
				    &state->base, &intel_bw_funcs);

	return 0;
}
