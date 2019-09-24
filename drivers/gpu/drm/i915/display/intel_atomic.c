/*
 * Copyright © 2015 Intel Corporation
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
 * DOC: atomic modeset support
 *
 * The functions here implement the state management and hardware programming
 * dispatch required by the atomic modeset infrastructure.
 * See intel_atomic_plane.c for the plane-specific atomic functionality.
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_plane_helper.h>

#include "intel_atomic.h"
#include "intel_display_types.h"
#include "intel_hdcp.h"
#include "intel_sprite.h"

/**
 * intel_digital_connector_atomic_get_property - hook for connector->atomic_get_property.
 * @connector: Connector to get the property for.
 * @state: Connector state to retrieve the property from.
 * @property: Property to retrieve.
 * @val: Return value for the property.
 *
 * Returns the atomic property value for a digital connector.
 */
int intel_digital_connector_atomic_get_property(struct drm_connector *connector,
						const struct drm_connector_state *state,
						struct drm_property *property,
						u64 *val)
{
	struct drm_device *dev = connector->dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct intel_digital_connector_state *intel_conn_state =
		to_intel_digital_connector_state(state);

	if (property == dev_priv->force_audio_property)
		*val = intel_conn_state->force_audio;
	else if (property == dev_priv->broadcast_rgb_property)
		*val = intel_conn_state->broadcast_rgb;
	else {
		DRM_DEBUG_ATOMIC("Unknown property [PROP:%d:%s]\n",
				 property->base.id, property->name);
		return -EINVAL;
	}

	return 0;
}

/**
 * intel_digital_connector_atomic_set_property - hook for connector->atomic_set_property.
 * @connector: Connector to set the property for.
 * @state: Connector state to set the property on.
 * @property: Property to set.
 * @val: New value for the property.
 *
 * Sets the atomic property value for a digital connector.
 */
int intel_digital_connector_atomic_set_property(struct drm_connector *connector,
						struct drm_connector_state *state,
						struct drm_property *property,
						u64 val)
{
	struct drm_device *dev = connector->dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct intel_digital_connector_state *intel_conn_state =
		to_intel_digital_connector_state(state);

	if (property == dev_priv->force_audio_property) {
		intel_conn_state->force_audio = val;
		return 0;
	}

	if (property == dev_priv->broadcast_rgb_property) {
		intel_conn_state->broadcast_rgb = val;
		return 0;
	}

	DRM_DEBUG_ATOMIC("Unknown property [PROP:%d:%s]\n",
			 property->base.id, property->name);
	return -EINVAL;
}

static bool blob_equal(const struct drm_property_blob *a,
		       const struct drm_property_blob *b)
{
	if (a && b)
		return a->length == b->length &&
			!memcmp(a->data, b->data, a->length);

	return !a == !b;
}

int intel_digital_connector_atomic_check(struct drm_connector *conn,
					 struct drm_atomic_state *state)
{
	struct drm_connector_state *new_state =
		drm_atomic_get_new_connector_state(state, conn);
	struct intel_digital_connector_state *new_conn_state =
		to_intel_digital_connector_state(new_state);
	struct drm_connector_state *old_state =
		drm_atomic_get_old_connector_state(state, conn);
	struct intel_digital_connector_state *old_conn_state =
		to_intel_digital_connector_state(old_state);
	struct drm_crtc_state *crtc_state;

	intel_hdcp_atomic_check(conn, old_state, new_state);

	if (!new_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);

	/*
	 * These properties are handled by fastset, and might not end
	 * up in a modeset.
	 */
	if (new_conn_state->force_audio != old_conn_state->force_audio ||
	    new_conn_state->broadcast_rgb != old_conn_state->broadcast_rgb ||
	    new_conn_state->base.colorspace != old_conn_state->base.colorspace ||
	    new_conn_state->base.picture_aspect_ratio != old_conn_state->base.picture_aspect_ratio ||
	    new_conn_state->base.content_type != old_conn_state->base.content_type ||
	    new_conn_state->base.scaling_mode != old_conn_state->base.scaling_mode ||
	    new_conn_state->base.tv.margins.left != old_conn_state->base.tv.margins.left ||
	    new_conn_state->base.tv.margins.right != old_conn_state->base.tv.margins.right ||
	    new_conn_state->base.tv.margins.top != old_conn_state->base.tv.margins.top ||
	    new_conn_state->base.tv.margins.bottom != old_conn_state->base.tv.margins.bottom ||
	    !blob_equal(new_conn_state->base.hdr_output_metadata,
			old_conn_state->base.hdr_output_metadata))
		crtc_state->mode_changed = true;

	return 0;
}

/**
 * intel_digital_connector_duplicate_state - duplicate connector state
 * @connector: digital connector
 *
 * Allocates and returns a copy of the connector state (both common and
 * digital connector specific) for the specified connector.
 *
 * Returns: The newly allocated connector state, or NULL on failure.
 */
struct drm_connector_state *
intel_digital_connector_duplicate_state(struct drm_connector *connector)
{
	struct intel_digital_connector_state *state;

	state = kmemdup(connector->state, sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_connector_duplicate_state(connector, &state->base);
	return &state->base;
}

/**
 * intel_crtc_duplicate_state - duplicate crtc state
 * @crtc: drm crtc
 *
 * Allocates and returns a copy of the crtc state (both common and
 * Intel-specific) for the specified crtc.
 *
 * Returns: The newly allocated crtc state, or NULL on failure.
 */
struct drm_crtc_state *
intel_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct intel_crtc_state *crtc_state;

	crtc_state = kmemdup(crtc->state, sizeof(*crtc_state), GFP_KERNEL);
	if (!crtc_state)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &crtc_state->base);

	crtc_state->update_pipe = false;
	crtc_state->disable_lp_wm = false;
	crtc_state->disable_cxsr = false;
	crtc_state->update_wm_pre = false;
	crtc_state->update_wm_post = false;
	crtc_state->fb_changed = false;
	crtc_state->fifo_changed = false;
	crtc_state->wm.need_postvbl_update = false;
	crtc_state->fb_bits = 0;
	crtc_state->update_planes = 0;

	return &crtc_state->base;
}

/**
 * intel_crtc_destroy_state - destroy crtc state
 * @crtc: drm crtc
 * @state: the state to destroy
 *
 * Destroys the crtc state (both common and Intel-specific) for the
 * specified crtc.
 */
void
intel_crtc_destroy_state(struct drm_crtc *crtc,
			 struct drm_crtc_state *state)
{
	drm_atomic_helper_crtc_destroy_state(crtc, state);
}

int intel_plane_scaler_user(struct intel_plane *plane)
{
	return drm_plane_index(&plane->base);
}

int intel_crtc_scaler_user(void)
{
	return SKL_CRTC_INDEX;
}

bool skl_can_use_hq_scaler(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct intel_crtc_scaler_state *scaler_state =
		&crtc_state->scaler_state;

	if (INTEL_GEN(dev_priv) >= 10 || IS_GEMINILAKE(dev_priv))
		return false;

	/*
	 * When only 1 scaler is in use on a pipe with 2 scalers
	 * scaler 1 can operate in high quality (HQ) mode.
	 */
	return crtc->num_scalers > 1 && !crtc_state->nv12_planes &&
		is_power_of_2(scaler_state->scaler_users);
}

static void skl_update_hq_scaler(struct intel_crtc_scaler_state *scaler_state,
				 enum scaler *scaler_id)
{
	/* Only scaler 1 can operate in HQ mode */
	scaler_state->scalers[*scaler_id].in_use = false;
	*scaler_id = SCALER_1;
	scaler_state->scalers[*scaler_id].in_use = true;
}

static u32 skl_crtc_scaler_mode(const struct intel_crtc_state *crtc_state)
{
	if (skl_can_use_hq_scaler(crtc_state))
		return SKL_PS_SCALER_MODE_HQ;
	else
		return SKL_PS_SCALER_MODE_DYN;
}

static u32 skl_plane_scaler_mode(const struct intel_crtc_state *crtc_state,
				 const struct intel_plane_state *plane_state)
{
	const struct drm_framebuffer *fb = plane_state->base.fb;

	if (drm_format_info_is_yuv_semiplanar(fb->format))
		return SKL_PS_SCALER_MODE_NV12;
	else
		return skl_crtc_scaler_mode(crtc_state);
}

static u32 glk_crtc_scaler_mode(void)
{
	return PS_SCALER_MODE_NORMAL;
}

static u32 glk_plane_scaler_mode(const struct intel_plane_state *plane_state)
{
	struct intel_plane *plane = to_intel_plane(plane_state->base.plane);
	struct drm_i915_private *dev_priv = to_i915(plane->base.dev);
	const struct drm_framebuffer *fb = plane_state->base.fb;

	/*
	 * On icl+ HDR planes we only use the scaler for
	 * scaling. They have a dedicated chroma upsampler, so
	 * we don't need the scaler to upsample the UV plane.
	 */
	if (drm_format_info_is_yuv_semiplanar(fb->format) &&
	    !icl_is_hdr_plane(dev_priv, plane->id))
		return PS_SCALER_MODE_PLANAR |
			(plane_state->linked_plane ?
			 PS_PLANE_Y_SEL(plane_state->linked_plane->id) : 0);
	else
		return PS_SCALER_MODE_NORMAL;
}

static enum scaler intel_allocate_scaler(struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct intel_crtc_scaler_state *scaler_state =
		&crtc_state->scaler_state;
	enum scaler scaler_id;

	for_each_scaler(crtc, scaler_id) {
		if (scaler_state->scalers[scaler_id].in_use)
			continue;

		scaler_state->scalers[scaler_id].in_use = true;

		return scaler_id;
	}

	return INVALID_SCALER;
}

static void intel_crtc_setup_scaler(struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	struct intel_crtc_scaler_state *scaler_state =
		&crtc_state->scaler_state;
	enum scaler *scaler_id = &crtc_state->pch_pfit.scaler_id;
	u32 mode;

	if (*scaler_id == INVALID_SCALER)
		*scaler_id = intel_allocate_scaler(crtc_state);

	if (WARN_ON(*scaler_id == INVALID_SCALER))
		return;

	if (INTEL_GEN(dev_priv) >= 10 || IS_GEMINILAKE(dev_priv))
		mode = glk_crtc_scaler_mode();
	else
		mode = skl_crtc_scaler_mode(crtc_state);

	if (mode == SKL_PS_SCALER_MODE_HQ)
		skl_update_hq_scaler(scaler_state, scaler_id);

	scaler_state->scalers[*scaler_id].mode = mode;

	DRM_DEBUG_KMS("[CRTC:%d:%s] Attached scaler %u to pipe\n",
		      crtc->base.base.id, crtc->base.name, *scaler_id);
}

static void intel_plane_setup_scaler(struct intel_crtc_state *crtc_state,
				     struct intel_plane_state *plane_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct intel_plane *plane = to_intel_plane(plane_state->base.plane);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	struct intel_crtc_scaler_state *scaler_state =
		&crtc_state->scaler_state;
	enum scaler *scaler_id = &plane_state->scaler_id;
	u32 mode;

	if (*scaler_id == INVALID_SCALER)
		*scaler_id = intel_allocate_scaler(crtc_state);

	if (WARN_ON(*scaler_id == INVALID_SCALER))
		return;

	if (INTEL_GEN(dev_priv) >= 10 || IS_GEMINILAKE(dev_priv))
		mode = glk_plane_scaler_mode(plane_state);
	else
		mode = skl_plane_scaler_mode(crtc_state, plane_state);

	if (mode == SKL_PS_SCALER_MODE_HQ)
		skl_update_hq_scaler(scaler_state, scaler_id);

	scaler_state->scalers[*scaler_id].mode = mode;

	DRM_DEBUG_KMS("[CRTC:%d:%s] Attached scaler %u to [PLANE:%d:%s]\n",
		      crtc->base.base.id, crtc->base.name, *scaler_id,
		      plane->base.base.id, plane->base.name);
}

static int skl_add_scaled_planes(struct intel_crtc_state *crtc_state)
{
	struct intel_atomic_state *state = to_intel_atomic_state(crtc_state->base.state);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct intel_crtc_scaler_state *scaler_state =
		&crtc_state->scaler_state;
	struct intel_plane *plane;

	for_each_intel_plane_on_crtc(&dev_priv->drm, crtc, plane) {
		int scaler_user = intel_plane_scaler_user(plane);
		struct intel_plane_state *plane_state;

		if ((scaler_state->scaler_users & BIT(scaler_user)) == 0)
			continue;

		plane_state = intel_atomic_get_plane_state(state, plane);
		if (IS_ERR(plane_state))
			return PTR_ERR(plane_state);

		crtc_state->update_planes |= BIT(plane->id);
	}

	return 0;
}

/**
 * intel_atomic_setup_scalers() - setup scalers for crtc per staged requests
 * @state: the atomic state
 * @crtc: the crtc
 *
 * This function sets up scalers based on staged scaling requests for
 * a @crtc and its planes. It is called from crtc level check path. If request
 * is a supportable request, it attaches scalers to requested planes and crtc.
 *
 * This function takes into account the current scaler(s) in use by any planes
 * not being part of this atomic state
 *
 *  Returns:
 *         0 - scalers were setup succesfully
 *         error code - otherwise
 */
int intel_atomic_setup_scalers(struct intel_atomic_state *state,
			       struct intel_crtc *crtc)
{
	struct intel_crtc_state *new_crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	const struct intel_crtc_state *old_crtc_state =
		intel_atomic_get_old_crtc_state(state, crtc);
	struct intel_crtc_scaler_state *scaler_state =
		&new_crtc_state->scaler_state;
	struct intel_plane_state *new_plane_state;
	struct intel_plane *plane;
	int num_scaler_users = hweight32(scaler_state->scaler_users);
	int i;

	if (num_scaler_users > crtc->num_scalers) {
		DRM_DEBUG_KMS("[CRTC:%d:%s] Too many scaling requests %d > %d\n",
			      crtc->base.base.id, crtc->base.name,
			      num_scaler_users, crtc->num_scalers);
		return -EINVAL;
	}

	if (skl_can_use_hq_scaler(old_crtc_state) !=
	    skl_can_use_hq_scaler(new_crtc_state)) {
		int ret;

		ret = skl_add_scaled_planes(new_crtc_state);
		if (ret)
			return ret;

		if (scaler_state->scaler_users & BIT(intel_crtc_scaler_user()))
			new_crtc_state->update_pipe = true;
	}

	intel_crtc_setup_scaler(new_crtc_state);

	for_each_new_intel_plane_in_state(state, plane, new_plane_state, i) {
		if (plane->pipe != crtc->pipe)
			continue;

		intel_plane_setup_scaler(new_crtc_state, new_plane_state);
	}

	return 0;
}

struct drm_atomic_state *
intel_atomic_state_alloc(struct drm_device *dev)
{
	struct intel_atomic_state *state = kzalloc(sizeof(*state), GFP_KERNEL);

	if (!state || drm_atomic_state_init(dev, &state->base) < 0) {
		kfree(state);
		return NULL;
	}

	return &state->base;
}

void intel_atomic_state_clear(struct drm_atomic_state *s)
{
	struct intel_atomic_state *state = to_intel_atomic_state(s);
	drm_atomic_state_default_clear(&state->base);
	state->dpll_set = state->modeset = false;
}

struct intel_crtc_state *
intel_atomic_get_crtc_state(struct drm_atomic_state *state,
			    struct intel_crtc *crtc)
{
	struct drm_crtc_state *crtc_state;
	crtc_state = drm_atomic_get_crtc_state(state, &crtc->base);
	if (IS_ERR(crtc_state))
		return ERR_CAST(crtc_state);

	return to_intel_crtc_state(crtc_state);
}
