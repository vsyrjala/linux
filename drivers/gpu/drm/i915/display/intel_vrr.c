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
}

void intel_vrr_enable(struct intel_encoder *encoder,
		      const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->hw.adjusted_mode;
	u32 trans_vrr_ctl;
	u16 framestart_to_pipelinefull_linecnt;

	if (!crtc_state->vrr.enable)
		return;

	framestart_to_pipelinefull_linecnt =
		min_t(u16, 255, (crtc_state->vrr.vtotalmin - adjusted_mode->crtc_vdisplay - 4));

	trans_vrr_ctl = VRR_CTL_VRR_ENABLE |  VRR_CTL_IGN_MAX_SHIFT |
		VRR_CTL_FLIP_LINE_EN | VRR_CTL_LINE_COUNT(framestart_to_pipelinefull_linecnt) |
		VRR_CTL_SW_FULLLINE_COUNT;

	intel_de_write(dev_priv, TRANS_VRR_VMIN(cpu_transcoder), crtc_state->vrr.vtotalmin - 2);
	intel_de_write(dev_priv, TRANS_VRR_VMAX(cpu_transcoder), crtc_state->vrr.vtotalmax - 1);
	intel_de_write(dev_priv, TRANS_VRR_CTL(cpu_transcoder), trans_vrr_ctl);
	intel_de_write(dev_priv, TRANS_VRR_FLIPLINE(cpu_transcoder), crtc_state->vrr.vtotalmin - 1);
	intel_de_write(dev_priv, TRANS_PUSH(cpu_transcoder), TRANS_PUSH_EN);
}

void intel_vrr_send_push(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;

	if (!crtc_state->vrr.enable)
		return;

	intel_de_write(dev_priv, TRANS_PUSH(cpu_transcoder),
		       TRANS_PUSH_EN | TRANS_PUSH_SEND);
}

void intel_vrr_disable(const struct intel_crtc_state *old_crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(old_crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum transcoder cpu_transcoder = old_crtc_state->cpu_transcoder;

	if (!old_crtc_state->vrr.enable)
		return;

	intel_de_write(dev_priv, TRANS_VRR_CTL(cpu_transcoder), 0);
	intel_de_write(dev_priv, TRANS_PUSH(cpu_transcoder), 0);
}

void intel_vrr_get_config(struct intel_crtc *crtc,
			  struct intel_crtc_state *crtc_state)
{
	struct drm_device *dev = crtc->base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	u32 trans_vrr_ctl;

	trans_vrr_ctl = intel_de_read(dev_priv, TRANS_VRR_CTL(cpu_transcoder));
	crtc_state->vrr.enable = trans_vrr_ctl & VRR_CTL_VRR_ENABLE;
	if (!crtc_state->vrr.enable)
		return;

	crtc_state->vrr.vtotalmax = intel_de_read(dev_priv, TRANS_VRR_VMAX(cpu_transcoder)) + 1;
	crtc_state->vrr.vtotalmin = intel_de_read(dev_priv, TRANS_VRR_VMIN(cpu_transcoder)) + 1;
}
