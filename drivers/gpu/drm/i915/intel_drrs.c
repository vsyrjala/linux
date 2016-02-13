/*
 * Copyright © 2016 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "i915_drv.h"
#include "intel_drv.h"

/**
 * DOC: Display Refresh Rate Switching (DRRS)
 *
 * Display Refresh Rate Switching (DRRS) is a power conservation feature
 * which enables swtching between low and high refresh rates,
 * dynamically, based on the usage scenario. This feature is applicable
 * for internal panels.
 *
 * Indication that the panel supports DRRS is given by the panel EDID, which
 * would list multiple refresh rates for one resolution.
 *
 * DRRS is of 2 types - static and seamless.
 * Static DRRS involves changing refresh rate (RR) by doing a full modeset
 * (may appear as a blink on screen) and is used in dock-undock scenario.
 * Seamless DRRS involves changing RR without any visual effect to the user
 * and can be used during normal system usage. This is done by programming
 * certain registers.
 *
 * Support for static/seamless DRRS may be indicated in the VBT based on
 * inputs from the panel spec.
 *
 * DRRS saves power by switching to low RR based on usage scenarios.
 *
 * eDP DRRS:-
 *        The implementation is based on frontbuffer tracking implementation.
 * When there is a disturbance on the screen triggered by user activity or a
 * periodic system activity, DRRS is disabled (RR is changed to high RR).
 * When there is no movement on screen, after a timeout of 1 second, a switch
 * to low RR is made.
 *        For integration with frontbuffer tracking code,
 * intel_drrs_invalidate() and intel_drrs_flush() are called.
 *
 * DRRS can be further extended to support other internal panels and also
 * the scenario of video playback wherein RR is set based on the rate
 * requested by userspace.
 */

/**
 * intel_crtc_drrs_set_refresh_rate - program registers for RR switch to take effect
 * @crtc: crtc
 * @refresh_rate: RR to be programmed
 *
 * This function gets called when refresh rate (RR) has to be changed from
 * one frequency to another. Switches can be between high and low RR
 * supported by the panel or to any other RR based on media playback (in
 * this case, RR value needs to be passed from user space).
 *
 * The caller of this function needs to take a lock on dev_priv->drrs.
 */
static void intel_crtc_drrs_set_refresh_rate(struct intel_crtc *crtc,
					     enum drrs_refresh_rate rate)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);

	lockdep_assert_held(&dev_priv->drrs.mutex);

	if (WARN_ON(!crtc->drrs.enable))
		return;

	if (crtc->drrs.rate == rate)
		return;

	if (IS_BROADWELL(dev_priv) || INTEL_INFO(dev_priv)->gen >= 9)
		intel_crtc_dp_m_n_set_refresh_rate(crtc, rate);
	else if (INTEL_INFO(dev_priv)->gen >= 5)
		intel_crtc_pipeconf_set_refresh_rate(crtc, rate);
	else
		intel_crtc_dpll_set_refresh_rate(crtc, rate);

	crtc->drrs.rate = rate;

	DRM_DEBUG_KMS("pipe %c refresh rate set to: %s\n",
		      pipe_name(crtc->pipe),
		      rate == DRRS_REFRESH_RATE_HIGH ? "high" : "low");
}

static void intel_crtc_drrs_downclock_work(struct work_struct *work)
{
	struct intel_crtc *crtc =
		container_of(work, typeof(*crtc), drrs.work.work);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);

	mutex_lock(&dev_priv->drrs.mutex);

	if (crtc->drrs.enable && !crtc->drrs.busy_bits)
		intel_crtc_drrs_set_refresh_rate(crtc, DRRS_REFRESH_RATE_LOW);

	mutex_unlock(&dev_priv->drrs.mutex);
}

/**
 * intel_crtc_drrs_disable - Disable DRRS
 * @crtc: crtc
 *
 * Disable DRRS for @crtc. Should be called after to enabling
 * the crtc.
 */
void intel_crtc_drrs_enable(struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);

	if (!i915.drrs)
		return;

	if (crtc->config->crtc_clock_low ==
	    crtc->config->base.adjusted_mode.crtc_clock)
		return;

	DRM_DEBUG_KMS("enabling DRRS for pipe %c\n", pipe_name(crtc->pipe));

	mutex_lock(&dev_priv->drrs.mutex);

	WARN_ON(crtc->drrs.enable);

	crtc->drrs.enable = true;
	crtc->drrs.busy_bits = 0;

	mutex_unlock(&dev_priv->drrs.mutex);
}

/**
 * intel_crtc_drrs_disable - Disable DRRS
 * @crtc: crtc
 *
 * Disable DRRS for @crtc. Should be called prior to disabling
 * the pipe.
 */
void intel_crtc_drrs_disable(struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);

	mutex_lock(&dev_priv->drrs.mutex);

	if (crtc->drrs.enable) {
		DRM_DEBUG_KMS("disabling DRRS for pipe %c\n", pipe_name(crtc->pipe));

		intel_crtc_drrs_set_refresh_rate(crtc, DRRS_REFRESH_RATE_HIGH);
		cancel_delayed_work(&crtc->drrs.work);
		crtc->drrs.enable = false;
		crtc->drrs.busy_bits = 0;
	}

	mutex_unlock(&dev_priv->drrs.mutex);
}

void intel_crtc_drrs_init(struct intel_crtc *crtc)
{
	INIT_DELAYED_WORK(&crtc->drrs.work, intel_crtc_drrs_downclock_work);
}

void intel_drrs_init(struct drm_i915_private *dev_priv)
{
	mutex_init(&dev_priv->drrs.mutex);
}

void intel_drrs_cleanup(struct drm_i915_private *dev_priv)
{
	struct drm_device *dev = dev_priv->dev;
	struct intel_crtc *crtc;

	for_each_intel_crtc(dev, crtc)
		intel_crtc_drrs_disable(crtc);
}

static void intel_drrs_update(struct drm_i915_private *dev_priv,
			      unsigned int frontbuffer_bits,
			      bool invalidate)
{
	struct drm_device *dev = dev_priv->dev;
	struct intel_crtc *crtc;

	mutex_lock(&dev_priv->drrs.mutex);

	for_each_intel_crtc(dev, crtc) {
		unsigned int bits;

		bits = frontbuffer_bits & INTEL_FRONTBUFFER_ALL_MASK(crtc->pipe);
		if (!bits)
			continue;

		frontbuffer_bits &= ~bits;

		if (!crtc->drrs.enable)
			continue;

		intel_crtc_drrs_set_refresh_rate(crtc, DRRS_REFRESH_RATE_HIGH);

		if (invalidate)
			crtc->drrs.busy_bits |= bits;
		else
			crtc->drrs.busy_bits &= ~bits;

		if (crtc->drrs.busy_bits)
			cancel_delayed_work(&crtc->drrs.work);
		else
			schedule_delayed_work(&crtc->drrs.work,
					      msecs_to_jiffies(100));
	}

	mutex_unlock(&dev_priv->drrs.mutex);
}

/**
 * intel_drrs_invalidate - Disable Idleness DRRS
 * @dev_priv: device
 * @frontbuffer_bits: frontbuffer plane tracking bits
 *
 * This function gets called everytime rendering on the given planes start.
 * Hence DRRS needs to be Upclocked, i.e. (LOW_RR -> HIGH_RR).
 */
void intel_drrs_invalidate(struct drm_i915_private *dev_priv,
			   unsigned int frontbuffer_bits)
{
	intel_drrs_update(dev_priv, frontbuffer_bits, true);
}

/**
 * intel_drrs_flush - Restart Idleness DRRS
 * @dev_priv: device
 * @frontbuffer_bits: frontbuffer plane tracking bits
 *
 * This function gets called every time rendering on the given planes has
 * completed or flip on a crtc is completed. So DRRS should be upclocked
 * (LOW_RR -> HIGH_RR). And also Idleness detection should be started again,
 * if no other planes are dirty.
 */
void intel_drrs_flush(struct drm_i915_private *dev_priv,
		      unsigned int frontbuffer_bits)
{
	intel_drrs_update(dev_priv, frontbuffer_bits, false);
}
