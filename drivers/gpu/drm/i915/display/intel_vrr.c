/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 *
 */

#include "i915_drv.h"
#include "intel_display_types.h"
#include "intel_vrr.h"

bool intel_vrr_is_capable(struct drm_connector *connector)
{
	struct intel_dp *intel_dp;
	const struct drm_display_info *info = &connector->display_info;
	struct drm_i915_private *i915 = to_i915(connector->dev);

	if (connector->connector_type != DRM_MODE_CONNECTOR_eDP &&
	    connector->connector_type != DRM_MODE_CONNECTOR_DisplayPort)
		return false;

	intel_dp = intel_attached_dp(to_intel_connector(connector));
	/*
	 * DP Sink is capable of VRR video timings if
	 * Ignore MSA bit is set in DPCD.
	 * EDID monitor range also should be atleast 10 for reasonable
	 * Adaptive Sync or Variable Refresh Rate end user experience.
	 */
	return HAS_VRR(i915) &&
		drm_dp_sink_can_do_video_without_timing_msa(intel_dp->dpcd) &&
		info->monitor_range.max_vfreq - info->monitor_range.min_vfreq > 10;
}

void
intel_vrr_check_modeset(struct intel_atomic_state *state)
{
	int i;
	struct intel_crtc_state *old_crtc_state, *new_crtc_state;
	struct intel_crtc *crtc;

	for_each_oldnew_intel_crtc_in_state(state, crtc, old_crtc_state,
					    new_crtc_state, i) {
		if (new_crtc_state->uapi.vrr_enabled !=
		    old_crtc_state->uapi.vrr_enabled)
			new_crtc_state->uapi.mode_changed = true;
	}
}

void
intel_vrr_compute_config(struct intel_dp *intel_dp,
			 struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);
	struct intel_connector *intel_connector = intel_dp->attached_connector;
	struct drm_connector *connector = &intel_connector->base;
	struct drm_display_mode *adjusted_mode = &crtc_state->hw.adjusted_mode;
	const struct drm_display_info *info = &connector->display_info;

	if (!intel_vrr_is_capable(connector))
		return;

	if (!crtc_state->uapi.vrr_enabled) {
		crtc_state->vrr.enable = false;
		return;
	}

	crtc_state->vrr.enable = true;
	crtc_state->vrr.vtotalmin =
		max_t(u16, adjusted_mode->crtc_vtotal,
		      DIV_ROUND_CLOSEST(adjusted_mode->crtc_clock * 1000,
					adjusted_mode->crtc_htotal *
					info->monitor_range.max_vfreq));
	crtc_state->vrr.vtotalmax =
		max_t(u16, adjusted_mode->crtc_vtotal,
		      DIV_ROUND_UP(adjusted_mode->crtc_clock * 1000,
				   adjusted_mode->crtc_htotal *
				   info->monitor_range.min_vfreq));

	drm_dbg_kms(&i915->drm,
		    "VRR Config: Enable = %s Vtotal Min = %d Vtotal Max = %d\n",
		    yesno(crtc_state->vrr.enable), crtc_state->vrr.vtotalmin,
		    crtc_state->vrr.vtotalmax);
}

