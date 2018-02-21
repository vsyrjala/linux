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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "intel_drv.h"

#define CTM_COEFF_SIGN	(1ULL << 63)

#define CTM_COEFF_1_0	(1ULL << 32)
#define CTM_COEFF_2_0	(CTM_COEFF_1_0 << 1)
#define CTM_COEFF_4_0	(CTM_COEFF_2_0 << 1)
#define CTM_COEFF_8_0	(CTM_COEFF_4_0 << 1)
#define CTM_COEFF_0_5	(CTM_COEFF_1_0 >> 1)
#define CTM_COEFF_0_25	(CTM_COEFF_0_5 >> 1)
#define CTM_COEFF_0_125	(CTM_COEFF_0_25 >> 1)

#define CTM_COEFF_LIMITED_RANGE ((235ULL - 16ULL) * CTM_COEFF_1_0 / 255)

#define CTM_COEFF_NEGATIVE(coeff)	(((coeff) & CTM_COEFF_SIGN) != 0)
#define CTM_COEFF_ABS(coeff)		((coeff) & (CTM_COEFF_SIGN - 1))

/* Post offset values for RGB->YCBCR conversion */
#define POSTOFF_RGB_TO_YUV_HI 0x800
#define POSTOFF_RGB_TO_YUV_ME 0x100
#define POSTOFF_RGB_TO_YUV_LO 0x800

/*
 * These values are direct register values specified in the Bspec,
 * for RGB->YUV conversion matrix (colorspace BT709)
 */
#define CSC_RGB_TO_YUV_RU_GU 0x2ba809d8
#define CSC_RGB_TO_YUV_BU 0x37e80000
#define CSC_RGB_TO_YUV_RY_GY 0x1e089cc0
#define CSC_RGB_TO_YUV_BY 0xb5280000
#define CSC_RGB_TO_YUV_RV_GV 0xbce89ad8
#define CSC_RGB_TO_YUV_BV 0x1e080000

/*
 * Extract the CSC coefficient from a CTM coefficient (in U32.32 fixed point
 * format). This macro takes the coefficient we want transformed and the
 * number of fractional bits.
 *
 * We only have a 9 bits precision window which slides depending on the value
 * of the CTM coefficient and we write the value from bit 3. We also round the
 * value.
 */
#define ILK_CSC_COEFF_FP(coeff, fbits)	\
	(clamp_val(((coeff) >> (32 - (fbits) - 3)) + 4, 0, 0xfff) & 0xff8)

#define ILK_CSC_COEFF_LIMITED_RANGE	\
	ILK_CSC_COEFF_FP(CTM_COEFF_LIMITED_RANGE, 9)
#define ILK_CSC_COEFF_1_0		\
	((7 << 12) | ILK_CSC_COEFF_FP(CTM_COEFF_1_0, 8))

/*
 * When using limited range, multiply the matrix given by userspace by
 * the matrix that we would use for the limited range.
 */
static u64 *ctm_mult_by_limited(u64 *result, const u64 *input)
{
	int i;

	for (i = 0; i < 9; i++) {
		u64 user_coeff = input[i];
		u32 limited_coeff = CTM_COEFF_LIMITED_RANGE;
		u32 abs_coeff = clamp_val(CTM_COEFF_ABS(user_coeff), 0,
					  CTM_COEFF_4_0 - 1) >> 2;

		/*
		 * By scaling every co-efficient with limited range (16-235)
		 * vs full range (0-255) the final o/p will be scaled down to
		 * fit in the limited range supported by the panel.
		 */
		result[i] = mul_u32_u32(limited_coeff, abs_coeff) >> 30;
		result[i] |= user_coeff & CTM_COEFF_SIGN;
	}

	return result;
}

static void ilk_load_ycbcr_conversion_matrix(struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;

	I915_WRITE(PIPE_CSC_PREOFF_HI(pipe), 0);
	I915_WRITE(PIPE_CSC_PREOFF_ME(pipe), 0);
	I915_WRITE(PIPE_CSC_PREOFF_LO(pipe), 0);

	I915_WRITE(PIPE_CSC_COEFF_RU_GU(pipe), CSC_RGB_TO_YUV_RU_GU);
	I915_WRITE(PIPE_CSC_COEFF_BU(pipe), CSC_RGB_TO_YUV_BU);

	I915_WRITE(PIPE_CSC_COEFF_RY_GY(pipe), CSC_RGB_TO_YUV_RY_GY);
	I915_WRITE(PIPE_CSC_COEFF_BY(pipe), CSC_RGB_TO_YUV_BY);

	I915_WRITE(PIPE_CSC_COEFF_RV_GV(pipe), CSC_RGB_TO_YUV_RV_GV);
	I915_WRITE(PIPE_CSC_COEFF_BV(pipe), CSC_RGB_TO_YUV_BV);

	I915_WRITE(PIPE_CSC_POSTOFF_HI(pipe), POSTOFF_RGB_TO_YUV_HI);
	I915_WRITE(PIPE_CSC_POSTOFF_ME(pipe), POSTOFF_RGB_TO_YUV_ME);
	I915_WRITE(PIPE_CSC_POSTOFF_LO(pipe), POSTOFF_RGB_TO_YUV_LO);
	I915_WRITE(PIPE_CSC_MODE(pipe), 0);
}

static void ilk_load_csc_matrix(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;
	bool limited_color_range = false;
	u16 coeffs[9] = {};
	int i;

	if (crtc_state->ycbcr420) {
		ilk_load_ycbcr_conversion_matrix(crtc);
		return;
	} else if (crtc_state->base.ctm) {
		struct drm_color_ctm *ctm = crtc_state->base.ctm->data;
		const u64 *input;
		u64 temp[9];

		if (limited_color_range)
			input = ctm_mult_by_limited(temp, ctm->matrix);
		else
			input = ctm->matrix;

		/*
		 * Convert fixed point S31.32 input to format supported by the
		 * hardware.
		 */
		for (i = 0; i < ARRAY_SIZE(coeffs); i++) {
			uint64_t abs_coeff = ((1ULL << 63) - 1) & input[i];

			/*
			 * Clamp input value to min/max supported by
			 * hardware.
			 */
			abs_coeff = clamp_val(abs_coeff, 0, CTM_COEFF_4_0 - 1);

			/* sign bit */
			if (CTM_COEFF_NEGATIVE(input[i]))
				coeffs[i] |= 1 << 15;

			if (abs_coeff < CTM_COEFF_0_125)
				coeffs[i] |= (3 << 12) |
					ILK_CSC_COEFF_FP(abs_coeff, 12);
			else if (abs_coeff < CTM_COEFF_0_25)
				coeffs[i] |= (2 << 12) |
					ILK_CSC_COEFF_FP(abs_coeff, 11);
			else if (abs_coeff < CTM_COEFF_0_5)
				coeffs[i] |= (1 << 12) |
					ILK_CSC_COEFF_FP(abs_coeff, 10);
			else if (abs_coeff < CTM_COEFF_1_0)
				coeffs[i] |= ILK_CSC_COEFF_FP(abs_coeff, 9);
			else if (abs_coeff < CTM_COEFF_2_0)
				coeffs[i] |= (7 << 12) |
					ILK_CSC_COEFF_FP(abs_coeff, 8);
			else
				coeffs[i] |= (6 << 12) |
					ILK_CSC_COEFF_FP(abs_coeff, 7);
		}
	} else {
		/*
		 * Load an identity matrix if no coefficients are provided.
		 *
		 * TODO: Check what kind of values actually come out of the
		 * pipe with these coeff/postoff values and adjust to get the
		 * best accuracy. Perhaps we even need to take the bpc value
		 * into consideration.
		 */
		for (i = 0; i < 3; i++) {
			if (limited_color_range)
				coeffs[i * 3 + i] =
					ILK_CSC_COEFF_LIMITED_RANGE;
			else
				coeffs[i * 3 + i] = ILK_CSC_COEFF_1_0;
		}
	}

	I915_WRITE(PIPE_CSC_COEFF_RY_GY(pipe), coeffs[0] << 16 | coeffs[1]);
	I915_WRITE(PIPE_CSC_COEFF_BY(pipe), coeffs[2] << 16);

	I915_WRITE(PIPE_CSC_COEFF_RU_GU(pipe), coeffs[3] << 16 | coeffs[4]);
	I915_WRITE(PIPE_CSC_COEFF_BU(pipe), coeffs[5] << 16);

	I915_WRITE(PIPE_CSC_COEFF_RV_GV(pipe), coeffs[6] << 16 | coeffs[7]);
	I915_WRITE(PIPE_CSC_COEFF_BV(pipe), coeffs[8] << 16);

	I915_WRITE(PIPE_CSC_PREOFF_HI(pipe), 0);
	I915_WRITE(PIPE_CSC_PREOFF_ME(pipe), 0);
	I915_WRITE(PIPE_CSC_PREOFF_LO(pipe), 0);

	if (INTEL_GEN(dev_priv) >= 7) {
		uint16_t postoff = 0;

		if (limited_color_range)
			postoff = (16 * (1 << 12) / 255) & 0x1fff;

		I915_WRITE(PIPE_CSC_POSTOFF_HI(pipe), postoff);
		I915_WRITE(PIPE_CSC_POSTOFF_ME(pipe), postoff);
		I915_WRITE(PIPE_CSC_POSTOFF_LO(pipe), postoff);

		I915_WRITE(PIPE_CSC_MODE(pipe), crtc_state->csc_mode);
	} else {
		uint32_t mode = CSC_MODE_YUV_TO_RGB;

		if (limited_color_range)
			mode |= CSC_BLACK_SCREEN_OFFSET;

		I915_WRITE(PIPE_CSC_MODE(pipe), crtc_state->csc_mode | mode);
	}
}

static void chv_load_csc_matrix(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;

	if (crtc_state->base.ctm) {
		const struct drm_color_ctm *ctm = crtc_state->base.ctm->data;
		uint16_t coeffs[9] = { 0, };
		int i;

		for (i = 0; i < ARRAY_SIZE(coeffs); i++) {
			uint64_t abs_coeff =
				((1ULL << 63) - 1) & ctm->matrix[i];

			/* Round coefficient. */
			abs_coeff += 1 << (32 - 13);
			/* Clamp to hardware limits. */
			abs_coeff = clamp_val(abs_coeff, 0, CTM_COEFF_8_0 - 1);

			/* Write coefficients in S3.12 format. */
			if (ctm->matrix[i] & (1ULL << 63))
				coeffs[i] = 1 << 15;
			coeffs[i] |= ((abs_coeff >> 32) & 7) << 12;
			coeffs[i] |= (abs_coeff >> 20) & 0xfff;
		}

		I915_WRITE(CGM_PIPE_CSC_COEFF01(pipe),
			   coeffs[1] << 16 | coeffs[0]);
		I915_WRITE(CGM_PIPE_CSC_COEFF23(pipe),
			   coeffs[3] << 16 | coeffs[2]);
		I915_WRITE(CGM_PIPE_CSC_COEFF45(pipe),
			   coeffs[5] << 16 | coeffs[4]);
		I915_WRITE(CGM_PIPE_CSC_COEFF67(pipe),
			   coeffs[7] << 16 | coeffs[6]);
		I915_WRITE(CGM_PIPE_CSC_COEFF8(pipe), coeffs[8]);
	}
}

enum {
	I9XX_LUT_SIZE_8BIT = 256,
	I9XX_LUT_SIZE_10BIT = 129,

	ILK_LUT_SIZE_10BIT = 1024,
	ILK_LUT_SIZE_12BIT = 513,

	IVB_LUT_SIZE_SPLIT = 512,

	CHV_LUT_SIZE_CGM_DEGAMMA = 65,
	CHV_LUT_SIZE_CGM_GAMMA = 257,
};

static u32 i9xx_lut_8(const struct drm_color_lut *color)
{
	return drm_color_lut_extract(color->red, 8) << 16 |
		drm_color_lut_extract(color->green, 8) << 8 |
		drm_color_lut_extract(color->blue, 8);
}

/* i8xx/i9xx+ 10bit interpolated format w/ slope (high 8 bits) */
static u32 i9xx_lut_10_slope_ldw(const struct drm_color_lut *color)
{
	return drm_color_lut_extract(color[0].red, 10) << 16 |
		drm_color_lut_extract(color[0].green, 10) << 8 |
		drm_color_lut_extract(color[0].blue, 10);
}

/* i8xx/i9xx+ 10bit interpolated format w/ slope (low 8 bits) */
static u8 _i9xx_lut_10_slope_udw(u16 a, u16 b)
{
	unsigned int mantissa, exponent = 0;

	a = drm_color_lut_extract(a, 10) >> 6;
	b = drm_color_lut_extract(b, 10) >> 6;

	/*
	 * Cap the slope to the max if it's too steep,
	 * and prevent negative slope.
	 */
	mantissa = clamp((b - a) >> 3, 0, 0x7f);

	while (mantissa > 0xf) {
		mantissa >>= 1;
		exponent++;
	}

	return (exponent << 6) |
		(mantissa << 2) |
		(a & 0x3);
}

static u32 i9xx_lut_10_slope_udw(const struct drm_color_lut *color)
{
	return _i9xx_lut_10_slope_udw(color[0].red, color[1].red) << 16 |
		_i9xx_lut_10_slope_udw(color[0].green, color[1].green) << 8 |
		_i9xx_lut_10_slope_udw(color[0].blue, color[1].blue);
}

/* i965+ "10.6" interpolated format (high 8 bits) */
static u32 i965_lut_10p6_ldw(const struct drm_color_lut *color)
{
	return (color->red >> 8) << 16 |
		(color->green >> 8) << 8 |
		(color->blue >> 8);
}

/* i965+ "10.6" bit interpolated format (low 8 bits) */
static u32 i965_lut_10p6_udw(const struct drm_color_lut *color)
{
	return (color->red & 0xff) << 16 |
		(color->green & 0xff) << 8 |
		(color->blue & 0xff);
}

static u32 ilk_lut_10(const struct drm_color_lut *color)
{
	return drm_color_lut_extract(color->red, 10) << 20 |
		drm_color_lut_extract(color->green, 10) << 10 |
		drm_color_lut_extract(color->blue, 10);
}

/* ilk+ "12.4" interpolated format (high 10 bits) */
static u32 ilk_lut_12p4_ldw(const struct drm_color_lut *color)
{
	return (color->red >> 6) << 20 |
		(color->green >> 6) << 10 |
		(color->blue >> 6);
}

/* ilk+ "12.4" interpolated format (low 6 bits) */
static u32 ilk_lut_12p4_udw(const struct drm_color_lut *color)
{
	return (color->red & 0x3f) << 24 |
		(color->green & 0x3f) << 14 |
		(color->blue & 0x3f) << 4;
}

static bool i9xx_has_10bit_lut(struct drm_i915_private *dev_priv)
{
	/*
	 * Bspec:
	 " "NOTE: The 8-bit (non-10-bit) mode is the only
	 *  mode supported by BrookDale-G and Springdale-G."
	 * and
	 * "NOTE: The 8-bit (non-10-bit) mode is the only
	 * mode supported by Alviso and Grantsdale."
	 */
	return !IS_I845G(dev_priv) && !IS_I865G(dev_priv) &&
		!IS_I915G(dev_priv) && !IS_I915GM(dev_priv);
}

static void i9xx_load_lut_8(struct intel_crtc *crtc,
			    const struct drm_property_blob *blob)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_color_lut *lut = blob->data;
	int i, lut_size = drm_color_lut_size(blob);
	enum pipe pipe = crtc->pipe;

	WARN_ON(lut_size != I9XX_LUT_SIZE_8BIT);

	for (i = 0; i < lut_size; i++)
		I915_WRITE_FW(PALETTE(pipe, i), i9xx_lut_8(&lut[i]));
}

static void i9xx_load_lut_10_slope(struct intel_crtc *crtc,
				   const struct drm_property_blob *blob)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_color_lut *lut = blob->data;
	int i, lut_size = drm_color_lut_size(blob);
	enum pipe pipe = crtc->pipe;

	WARN_ON(lut_size != I9XX_LUT_SIZE_10BIT);

	for (i = 0; i < lut_size - 1; i++) {
		I915_WRITE_FW(PALETTE(pipe, i),
			      i9xx_lut_10_slope_ldw(&lut[i]));
		I915_WRITE_FW(PALETTE(pipe, i),
			      i9xx_lut_10_slope_udw(&lut[i]));
	}
}

static void i9xx_linear_lut_8(struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;
	int i;

	for (i = 0; i < I9XX_LUT_SIZE_8BIT; i++) {
		uint32_t v = (i << 16) | (i << 8) | i;

		I915_WRITE_FW(PALETTE(pipe, i), v);
	}
}

static void i9xx_load_luts(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	const struct drm_property_blob *blob = crtc_state->base.gamma_lut;

	if (!crtc_state->gamma_enable)
		return;

	switch (crtc_state->gamma_mode) {
	case GAMMA_MODE_MODE_8BIT:
		i9xx_load_lut_8(crtc, blob);
		break;
	case GAMMA_MODE_MODE_10BIT:
		i9xx_load_lut_10_slope(crtc, blob);
		break;
	}
}

static void i965_load_lut_10p6(struct intel_crtc *crtc,
			       const struct drm_property_blob *blob)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_color_lut *lut = blob->data;
	int i, lut_size = drm_color_lut_size(blob);
	enum pipe pipe = crtc->pipe;

	WARN_ON(lut_size != I9XX_LUT_SIZE_10BIT);

	for (i = 0; i < lut_size - 1; i++) {
		I915_WRITE_FW(PALETTE(pipe, 2 * i + 0),
			      i965_lut_10p6_ldw(&lut[i]));
		I915_WRITE_FW(PALETTE(pipe, 2 * i + 1),
			      i965_lut_10p6_udw(&lut[i]));
	}

	I915_WRITE_FW(PIPEGCMAX(pipe, 0), lut[i].red);
	I915_WRITE_FW(PIPEGCMAX(pipe, 1), lut[i].green);
	I915_WRITE_FW(PIPEGCMAX(pipe, 2), lut[i].blue);
}

static void i965_load_luts(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	const struct drm_property_blob *blob = crtc_state->base.gamma_lut;

	if (!crtc_state->gamma_enable)
		return;

	switch (crtc_state->gamma_mode) {
	case GAMMA_MODE_MODE_8BIT:
		i9xx_load_lut_8(crtc, blob);
		break;
	case GAMMA_MODE_MODE_10BIT:
		i965_load_lut_10p6(crtc, blob);
		break;
	}
}

static int i9xx_gamma_mode(struct drm_i915_private *dev_priv,
			   const struct drm_property_blob *blob)
{
	switch (drm_color_lut_size(blob)) {
	case 256:
		return GAMMA_MODE_MODE_8BIT;
	case 129:
		if (!i9xx_has_10bit_lut(dev_priv))
			return -EINVAL;

		return GAMMA_MODE_MODE_10BIT;
	default:
		return -EINVAL;
	}
}

static int i9xx_color_check(struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_property_blob *degamma_lut =
		crtc_state->base.degamma_lut;
	const struct drm_property_blob *gamma_lut =
		crtc_state->base.gamma_lut;
	int ret;

	if (WARN_ON(degamma_lut))
		return -EINVAL;

	crtc_state->gamma_enable = gamma_lut;

	if (gamma_lut) {
		ret = i9xx_gamma_mode(dev_priv, gamma_lut);
		if (ret < 0)
			return ret;

		crtc_state->gamma_mode = ret;
	} else {
		crtc_state->gamma_mode = GAMMA_MODE_MODE_8BIT;
	}

	return 0;
}

static void ilk_load_lut_10(struct intel_crtc *crtc,
			    const struct drm_property_blob *blob)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_color_lut *lut = blob->data;
	int i, lut_size = drm_color_lut_size(blob);
	enum pipe pipe = crtc->pipe;

	WARN_ON(lut_size != ILK_LUT_SIZE_10BIT);

	for (i = 0; i < lut_size; i++)
		I915_WRITE_FW(PREC_PALETTE(pipe, i), ilk_lut_10(&lut[i]));
}

static void ilk_load_lut_12p4(struct intel_crtc *crtc,
			      const struct drm_property_blob *blob)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_color_lut *lut = blob->data;
	int i, lut_size = drm_color_lut_size(blob);
	enum pipe pipe = crtc->pipe;

	WARN_ON(lut_size != ILK_LUT_SIZE_12BIT);

	for (i = 0; i < lut_size - 1; i++) {
		I915_WRITE_FW(PREC_PALETTE(pipe, 2 * i + 0),
			      ilk_lut_12p4_ldw(&lut[i]));
		I915_WRITE_FW(PREC_PALETTE(pipe, 2 * i + 1),
			      ilk_lut_12p4_udw(&lut[i]));
	}

	I915_WRITE_FW(PREC_PIPEGCMAX(pipe, 0), lut[i].red);
	I915_WRITE_FW(PREC_PIPEGCMAX(pipe, 1), lut[i].green);
	I915_WRITE_FW(PREC_PIPEGCMAX(pipe, 2), lut[i].blue);
}

static void ilk_load_luts(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	const struct drm_property_blob *blob =
		crtc_state->base.degamma_lut ?: crtc_state->base.gamma_lut;

	if (!crtc_state->gamma_enable)
		return;

	switch (crtc_state->gamma_mode) {
	case GAMMA_MODE_MODE_8BIT:
		i9xx_load_lut_8(crtc, blob);
		break;
	case GAMMA_MODE_MODE_10BIT:
		ilk_load_lut_10(crtc, blob);
		break;
	case GAMMA_MODE_MODE_12BIT:
		ilk_load_lut_12p4(crtc, blob);
		break;
	}
}

static int ilk_num_ext(struct drm_i915_private *dev_priv)
{
	if (INTEL_GEN(dev_priv) >= 10 || IS_GEMINILAKE(dev_priv))
		return 2; /* PAL_EXT_GC_MAX + PAL_EXT2_GC_MAX */
	else if (INTEL_GEN(dev_priv) >= 7)
		return 1; /* PAL_EXT_GC_MAX */
	else
		return 0;
}

static int __ilk_gamma_mode(int lut_size)
{
	switch (lut_size) {
	case I9XX_LUT_SIZE_8BIT:
		return GAMMA_MODE_MODE_8BIT;
	case ILK_LUT_SIZE_10BIT:
		return GAMMA_MODE_MODE_10BIT;
	case ILK_LUT_SIZE_12BIT:
		return GAMMA_MODE_MODE_12BIT;
	default:
		return -EINVAL;
	}
}

static int ilk_gamma_mode(struct drm_i915_private *dev_priv,
			  const struct drm_property_blob *blob)
{
	int lut_size = drm_color_lut_size(blob);
	int num_ext = ilk_num_ext(dev_priv);
	int ret;

	ret = __ilk_gamma_mode(lut_size);
	if (ret < 0)
		ret = __ilk_gamma_mode(lut_size - num_ext);

	return ret;
}

static int ilk_color_check(struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_property_blob *degamma_lut =
		crtc_state->base.degamma_lut;
	const struct drm_property_blob *gamma_lut =
		crtc_state->base.gamma_lut;
	const struct drm_property_blob *ctm =
		crtc_state->base.ctm;
	int ret;

	if (degamma_lut && gamma_lut)
		return -EINVAL;

	crtc_state->gamma_enable = degamma_lut || gamma_lut;
	crtc_state->csc_enable = ctm;

	if (degamma_lut) {
		ret = ilk_gamma_mode(dev_priv, degamma_lut);
		if (ret < 0)
			return ret;

		crtc_state->gamma_mode = ret;
		crtc_state->csc_mode = 0;
	} else if (gamma_lut) {
		ret = ilk_gamma_mode(dev_priv, gamma_lut);
		if (ret < 0)
			return ret;

		crtc_state->gamma_mode = ret;
		crtc_state->csc_mode = CSC_POSITION_BEFORE_GAMMA;
	} else {
		crtc_state->gamma_mode = GAMMA_MODE_MODE_8BIT;
		crtc_state->csc_mode = 0;
	}

	return 0;
}

static void ivb_load_lut_10(struct intel_crtc *crtc,
			    const struct drm_property_blob *blob,
			    int num_ext)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_color_lut *lut = blob->data;
	int i, lut_size = drm_color_lut_size(blob);
	enum pipe pipe = crtc->pipe;

	WARN_ON((INTEL_GEN(dev_priv) >= 10 || IS_GEMINILAKE(dev_priv)) &&
		lut_size != ILK_LUT_SIZE_10BIT + num_ext);
	WARN_ON(lut_size != IVB_LUT_SIZE_SPLIT + num_ext &&
		lut_size != ILK_LUT_SIZE_10BIT + num_ext);

	for (i = 0; i < lut_size - num_ext; i++)
		I915_WRITE_FW(PREC_PAL_DATA(pipe), ilk_lut_10(&lut[i]));

	if (num_ext) {
		I915_WRITE_FW(PREC_PAL_EXT_GC_MAX(pipe, 0), lut[i].red);
		I915_WRITE_FW(PREC_PAL_EXT_GC_MAX(pipe, 1), lut[i].green);
		I915_WRITE_FW(PREC_PAL_EXT_GC_MAX(pipe, 2), lut[i].blue);
		num_ext--;
		i++;
	}

	if (num_ext) {
		I915_WRITE_FW(PREC_PAL_EXT2_GC_MAX(pipe, 0), lut[i].red);
		I915_WRITE_FW(PREC_PAL_EXT2_GC_MAX(pipe, 1), lut[i].green);
		I915_WRITE_FW(PREC_PAL_EXT2_GC_MAX(pipe, 2), lut[i].blue);
		num_ext--;
		i++;
	}
}

static void ivb_load_lut_12p4(struct intel_crtc *crtc,
			      const struct drm_property_blob *blob,
			      int num_ext)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_color_lut *lut = blob->data;
	int i, lut_size = drm_color_lut_size(blob);
	enum pipe pipe = crtc->pipe;

	WARN_ON(lut_size != ILK_LUT_SIZE_12BIT + num_ext);

	for (i = 0; i < lut_size - 1 - num_ext; i++) {
		I915_WRITE(PREC_PAL_DATA(pipe),
			   ilk_lut_12p4_ldw(&lut[i]));
		I915_WRITE(PREC_PAL_DATA(pipe),
			   ilk_lut_12p4_udw(&lut[i]));
	}

	I915_WRITE_FW(PREC_PAL_GC_MAX(pipe, 0), lut[i].red);
	I915_WRITE_FW(PREC_PAL_GC_MAX(pipe, 1), lut[i].green);
	I915_WRITE_FW(PREC_PAL_GC_MAX(pipe, 2), lut[i].blue);
	i++;

	if (num_ext) {
		I915_WRITE_FW(PREC_PAL_EXT_GC_MAX(pipe, 0), lut[i].red);
		I915_WRITE_FW(PREC_PAL_EXT_GC_MAX(pipe, 1), lut[i].green);
		I915_WRITE_FW(PREC_PAL_EXT_GC_MAX(pipe, 2), lut[i].blue);
		num_ext--;
		i++;
	}

	if (num_ext) {
		I915_WRITE_FW(PREC_PAL_EXT2_GC_MAX(pipe, 0), lut[i].red);
		I915_WRITE_FW(PREC_PAL_EXT2_GC_MAX(pipe, 1), lut[i].green);
		I915_WRITE_FW(PREC_PAL_EXT2_GC_MAX(pipe, 2), lut[i].blue);
		num_ext--;
		i++;
	}
}

static int ivb_split_gamma_mode(struct drm_i915_private *dev_priv,
				const struct drm_property_blob *degamma_lut,
				const struct drm_property_blob *gamma_lut)
{
	int lut_size, num_ext = ilk_num_ext(dev_priv);

	lut_size = drm_color_lut_size(degamma_lut);
	if (lut_size != IVB_LUT_SIZE_SPLIT &&
	    lut_size != IVB_LUT_SIZE_SPLIT + num_ext)
		return -EINVAL;

	lut_size = drm_color_lut_size(gamma_lut);
	if (lut_size != IVB_LUT_SIZE_SPLIT)
		return -EINVAL;

	return GAMMA_MODE_MODE_SPLIT;
}

static int ivb_color_check(struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_property_blob *degamma_lut =
		crtc_state->base.degamma_lut;
	const struct drm_property_blob *gamma_lut =
		crtc_state->base.gamma_lut;
	const struct drm_property_blob *ctm =
		crtc_state->base.ctm;
	bool limited_color_range = false;
	int ret;

	if (INTEL_GEN(dev_priv) >= 8 || IS_HASWELL(dev_priv))
		limited_color_range = crtc_state->limited_color_range;

	crtc_state->gamma_enable = degamma_lut || gamma_lut;
	crtc_state->csc_enable = ctm || crtc_state->ycbcr420 ||
		limited_color_range;

	/*
	 * FIXME could multiply the matrices together, but
	 * that woould only work if no gamma LUT is present
	 * as it should be applied between the matrices.
	 */
	if (ctm && crtc_state->ycbcr420) {
		DRM_DEBUG_KMS("Can't use CTM with YCbCr output\n");
		return -EINVAL;
	}

	if (degamma_lut && gamma_lut) {
		if (crtc_state->ycbcr420) {
			DRM_DEBUG_KMS("Can't use split gamma with YCbCr output\n");
			return -EINVAL;
		}
		/*
		 * FIXME could remove this restriction by adjusting the gamma
		 * LUT instead of the CSC for the full->limited range conversion.
		 */
		if (limited_color_range) {
			DRM_DEBUG_KMS("Can't use split gamma with limited range RGB output\n");
			return -EINVAL;
		}

		ret = ivb_split_gamma_mode(dev_priv,
					   degamma_lut, gamma_lut);
		if (ret < 0)
			return -EINVAL;

		crtc_state->gamma_mode = ret;
	} else if (degamma_lut) {
		ret = ilk_gamma_mode(dev_priv, degamma_lut);
		if (ret < 0)
			return ret;

		crtc_state->gamma_mode = ret;
	} else if (gamma_lut) {
		/*
		 * FIXME could remove this restriction by adjusting the gamma
		 * LUT instead of the CSC for the full->limited range conversion.
		 */
		if (ctm && limited_color_range) {
			DRM_DEBUG_KMS("Can't use gamma with CTM and limited range RGB output\n");
			return -EINVAL;
		}

		ret = ilk_gamma_mode(dev_priv, gamma_lut);
		if (ret < 0)
			return ret;

		crtc_state->gamma_mode = ret;
	} else {
		crtc_state->gamma_mode = GAMMA_MODE_MODE_8BIT;
	}

	/* RGB->YCbCr, RGB full->limited range, and degamma before the CSC */
	if (crtc_state->ycbcr420 || limited_color_range || degamma_lut)
		crtc_state->csc_mode = 0;
	else
		crtc_state->csc_mode = CSC_POSITION_BEFORE_GAMMA;

	return 0;
}

static int ivb_load_num_ext(const struct intel_crtc_state *crtc_state,
			    const struct drm_property_blob *blob)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	int hw_lut_size, lut_size = drm_color_lut_size(blob);
	int num_ext;

	switch (crtc_state->gamma_mode) {
	case GAMMA_MODE_MODE_SPLIT:
		hw_lut_size = IVB_LUT_SIZE_SPLIT;
		break;
	case GAMMA_MODE_MODE_10BIT:
		hw_lut_size = ILK_LUT_SIZE_10BIT;
		break;
	case GAMMA_MODE_MODE_12BIT:
		hw_lut_size = ILK_LUT_SIZE_12BIT;
		break;
	default:
		WARN_ON(lut_size != I9XX_LUT_SIZE_8BIT);
		return 0;
	}

	num_ext = lut_size - hw_lut_size;
	WARN_ON(num_ext != 0 && num_ext != ilk_num_ext(dev_priv));

	return num_ext;
}

static void ivb_load_luts(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_property_blob *blob;
	enum pipe pipe = crtc->pipe;

	if (!crtc_state->gamma_enable)
		return;

	switch (crtc_state->gamma_mode) {
	case GAMMA_MODE_MODE_SPLIT:
		I915_WRITE_FW(PREC_PAL_INDEX(pipe), PAL_PREC_SPLIT_MODE |
			      PAL_PREC_AUTO_INCREMENT);

		blob = crtc_state->base.degamma_lut;
		ivb_load_lut_10(crtc, blob, ivb_load_num_ext(crtc_state, blob));

		blob = crtc_state->base.gamma_lut;
		ivb_load_lut_10(crtc, blob, 0);

		I915_WRITE_FW(PREC_PAL_INDEX(pipe), 0);
		break;
	case GAMMA_MODE_MODE_10BIT:
		I915_WRITE_FW(PREC_PAL_INDEX(pipe),
			      PAL_PREC_AUTO_INCREMENT);

		blob = crtc_state->base.degamma_lut ?:
			crtc_state->base.gamma_lut;
		ivb_load_lut_10(crtc, blob, ivb_load_num_ext(crtc_state, blob));

		I915_WRITE_FW(PREC_PAL_INDEX(pipe), 0);
		break;
	case GAMMA_MODE_MODE_12BIT:
		I915_WRITE_FW(PREC_PAL_INDEX(pipe),
			      PAL_PREC_AUTO_INCREMENT);

		blob = crtc_state->base.degamma_lut ?:
			crtc_state->base.gamma_lut;
		ivb_load_lut_12p4(crtc, blob, ivb_load_num_ext(crtc_state, blob));

		I915_WRITE_FW(PREC_PAL_INDEX(pipe), 0);
		break;
	case GAMMA_MODE_MODE_8BIT:
		blob = crtc_state->base.degamma_lut ?:
			crtc_state->base.gamma_lut;
		i9xx_load_lut_8(crtc, blob);
		break;
	}
}

static void glk_load_degamma_lut(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;
	int i, lut_size = 33;

	/*
	 * Note that we need to always load the degamma
	 * LUT since there is no way to bypass it.
	 */

	/*
	 * When setting the auto-increment bit, the hardware seems to
	 * ignore the index bits, so we need to reset it to index 0
	 * separately.
	 */
	I915_WRITE(PRE_CSC_GAMC_INDEX(pipe), 0);
	I915_WRITE(PRE_CSC_GAMC_INDEX(pipe), PRE_CSC_GAMC_AUTO_INCREMENT);

	/*
	 *  FIXME: The pipe degamma table in geminilake doesn't support
	 *  different values per channel, so this just loads a linear table.
	 */
	for (i = 0; i < lut_size; i++) {
		uint32_t v = (i * (1 << 16)) / (lut_size - 1);

		I915_WRITE(PRE_CSC_GAMC_DATA(pipe), v);
	}

	/* Clamp values > 1.0. */
	for (; i < 35; i++)
		I915_WRITE(PRE_CSC_GAMC_DATA(pipe), (1 << 16));
}

static void glk_load_luts(const struct intel_crtc_state *crtc_state)
{
	glk_load_degamma_lut(crtc_state);
	ivb_load_luts(crtc_state);
}

static int glk_color_check(struct intel_crtc_state *crtc_state)
{
	const struct drm_property_blob *degamma_lut =
		crtc_state->base.degamma_lut;
	int ret;

	if (WARN_ON(degamma_lut))
		return -EINVAL;

	/*
	 * FIXME: Oh dear. We can no longer use the full gamma
	 * LUT with YCbCr or limited range RGB output. We should either
	 * fall back to atually using the degamma LUT (and hope the user
	 * doesn't want different curves for different channels), or
	 * do the range compression with the gamma LUT instead of the CSC.
	 * That last trick obviously doesn't work for the YCbCr case.
	 */
	if (crtc_state->base.gamma_lut) {
		if (crtc_state->ycbcr420) {
			DRM_DEBUG_KMS("Can't use gamma with YCbCr output\n");
			/* FIXME this would surely break some current setups :( */
#if 0
			return -EINVAL;
#endif
		}
		if (crtc_state->limited_color_range) {
			DRM_DEBUG_KMS("Can't use gamma with limited range RGB output\n");
			/* FIXME this would surely break some current setups :( */
#if 0
			return -EINVAL;
#endif
		}
	}

	ret = ivb_color_check(crtc_state);
	if (ret)
		return ret;

	crtc_state->csc_mode = 0; /* register is mbz */

	return 0;
}

static uint32_t chv_lut_ldw(const struct drm_color_lut *color, int bits)
{
	return drm_color_lut_extract(color->green, bits << 16) |
		drm_color_lut_extract(color->blue, bits);
}

static uint32_t chv_lut_udw(const struct drm_color_lut *color, int bits)
{
	return drm_color_lut_extract(color->red, bits);
}

static void chv_load_cgm_degamma(struct intel_crtc *crtc,
				 const struct drm_property_blob *blob)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_color_lut *lut = blob->data;
	int i, lut_size = drm_color_lut_size(blob);
	enum pipe pipe = crtc->pipe;

	WARN_ON(lut_size != CHV_LUT_SIZE_CGM_DEGAMMA);

	for (i = 0; i < lut_size; i++) {
		I915_WRITE(CGM_PIPE_DEGAMMA(pipe, i, 0), chv_lut_ldw(&lut[i], 14));
		I915_WRITE(CGM_PIPE_DEGAMMA(pipe, i, 1), chv_lut_udw(&lut[i], 14));
	}
}

static void chv_load_cgm_gamma(struct intel_crtc *crtc,
			       const struct drm_property_blob *blob)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_color_lut *lut = blob->data;
	int i, lut_size = drm_color_lut_size(blob);
	enum pipe pipe = crtc->pipe;

	WARN_ON(lut_size != CHV_LUT_SIZE_CGM_GAMMA);

	for (i = 0; i < lut_size; i++) {
		I915_WRITE(CGM_PIPE_GAMMA(pipe, i, 0), chv_lut_ldw(&lut[i], 10));
		I915_WRITE(CGM_PIPE_GAMMA(pipe, i, 1), chv_lut_udw(&lut[i], 10));
	}
}

static void chv_load_luts(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	const struct drm_property_blob *degamma_lut =
		crtc_state->base.degamma_lut;
	const struct drm_property_blob *gamma_lut =
		crtc_state->base.gamma_lut;

	if (degamma_lut)
		chv_load_cgm_degamma(crtc, degamma_lut);

	if (gamma_lut && gamma_lut->length == CHV_LUT_SIZE_CGM_GAMMA)
		chv_load_cgm_gamma(crtc, gamma_lut);
	else if (gamma_lut)
		i965_load_luts(crtc_state);

	/*
	 * Also program a linear LUT in the legacy block (behind the
	 * CGM block).
	 * FIXME can we just disable the legacy gamma when using the
	 * cgm? If not we should use the 10bit more here probably to
	 * avoid trucating shit.
	 */
	if (gamma_lut->length == CHV_LUT_SIZE_CGM_GAMMA)
		i9xx_linear_lut_8(crtc);
}

static int chv_color_check(struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	const struct drm_property_blob *degamma_lut =
		crtc_state->base.degamma_lut;
	const struct drm_property_blob *gamma_lut =
		crtc_state->base.gamma_lut;
	const struct drm_property_blob *ctm =
		crtc_state->base.ctm;
	int ret;

	/* FIXME does this affect the cgm unit? */
	crtc_state->gamma_enable = degamma_lut || gamma_lut;
	crtc_state->cgm_mode = 0;

	if (ctm)
		crtc_state->cgm_mode |= CGM_PIPE_MODE_CSC;

	if (degamma_lut) {
		if (degamma_lut->length != CHV_LUT_SIZE_CGM_DEGAMMA)
			return -EINVAL;

		crtc_state->cgm_mode |= CGM_PIPE_MODE_DEGAMMA;
	}

	if (gamma_lut) {
		switch (gamma_lut->length) {
		case CHV_LUT_SIZE_CGM_GAMMA:
			/* FIXME can we just disable legacy gamma here? */
			crtc_state->gamma_mode = GAMMA_MODE_MODE_8BIT;
			crtc_state->cgm_mode |= CGM_PIPE_MODE_GAMMA;
			break;
		default:
			ret = i9xx_gamma_mode(dev_priv, gamma_lut);
			if (ret < 0)
				return ret;

			crtc_state->gamma_mode = ret;
			break;
		}
	}

	if (!degamma_lut && !gamma_lut)
		crtc_state->gamma_mode = GAMMA_MODE_MODE_8BIT;

	return 0;
}

static void i9xx_set_pipe_color_config(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct intel_plane *plane = to_intel_plane(crtc->base.primary);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum i9xx_plane_id i9xx_plane = plane->i9xx_plane;
	u32 tmp;

	/*
	 * Primary plane pipe gamma/csc enable bits also
	 * affect the pipe bottom color, so must update
	 * them even if the primary plane happens to be
	 * currently disabled.
	 *
	 * FIXME would be nice if we didn't have to duplicate
	 * this here...
	 */
	tmp = I915_READ(DSPCNTR(i9xx_plane));
	tmp &= ~(DISPPLANE_GAMMA_ENABLE | DISPPLANE_PIPE_CSC_ENABLE);
	if (crtc_state->gamma_enable)
		tmp |= DISPPLANE_GAMMA_ENABLE;
	if (crtc_state->csc_enable)
		tmp |= DISPPLANE_PIPE_CSC_ENABLE;
	I915_WRITE(DSPCNTR(i9xx_plane), tmp);
}

static void i9xx_set_gamma_mode(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;
	u32 val;

	val = I915_READ(PIPECONF(pipe));
	val &= ~PIPECONF_GAMMA_MODE_MASK_I9XX;
	val |= PIPECONF_GAMMA_MODE(crtc_state->gamma_mode);
	I915_WRITE(PIPECONF(pipe), val);
}

static void ilk_set_gamma_mode(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;
	u32 val;

	val = I915_READ(PIPECONF(pipe));
	val &= ~PIPECONF_GAMMA_MODE_MASK_ILK;
	val |= PIPECONF_GAMMA_MODE(crtc_state->gamma_mode);
	I915_WRITE(PIPECONF(pipe), val);
}

static void i9xx_color_commit(const struct intel_crtc_state *crtc_state)
{
	i9xx_set_pipe_color_config(crtc_state);

	i9xx_set_gamma_mode(crtc_state);
}

static void chv_color_commit(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);

	i9xx_set_pipe_color_config(crtc_state);

	i9xx_set_gamma_mode(crtc_state);

	I915_WRITE(CGM_PIPE_MODE(crtc->pipe), crtc_state->cgm_mode);

	if (crtc_state->csc_enable)
		chv_load_csc_matrix(crtc_state);
}

static void ilk_color_commit(const struct intel_crtc_state *crtc_state)
{
	i9xx_set_pipe_color_config(crtc_state);

	ilk_set_gamma_mode(crtc_state);

	if (crtc_state->csc_enable)
		ilk_load_csc_matrix(crtc_state);
}

static void hsw_color_commit(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);

	i9xx_set_pipe_color_config(crtc_state);

	I915_WRITE(GAMMA_MODE(crtc->pipe), crtc_state->gamma_mode);

	if (crtc_state->csc_enable)
		ilk_load_csc_matrix(crtc_state);
}

static void skl_color_commit(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	u32 tmp;

	tmp = 0;
	if (crtc_state->gamma_enable)
		tmp |= PIPE_BOTTOM_GAMMA_ENABLE;
	if (crtc_state->csc_enable)
		tmp |= PIPE_BOTTOM_CSC_ENABLE;
	I915_WRITE(PIPE_BOTTOM_COLOR(crtc->pipe), tmp);

	I915_WRITE(GAMMA_MODE(crtc->pipe), crtc_state->gamma_mode);

	if (crtc_state->csc_enable)
		ilk_load_csc_matrix(crtc_state);
}

#define I9XX_GAMMA_8 \
	{ \
		.flags = DRM_MODE_LUT_GAMMA, \
		.count = 256, \
		.input_bpc = 8, .output_bpc = 8, \
		.start = 0, .end = (1 << 8) - 1, \
		.min = 0, .max = (1 << 8) - 1, \
	}

static const struct drm_color_lut_range i9xx_gamma_8[] = {
	I9XX_GAMMA_8,
};

static const struct drm_color_lut_range i9xx_gamma_10_slope[] = {
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 129,
		.input_bpc = 10, .output_bpc = 10,
		.start = 0, .end = 1 << 10,
		.min = 0, .max = (1 << 10) - 1,
	},
};

#define I965_GAMMA_10 \
	{ \
		.flags = (DRM_MODE_LUT_GAMMA | \
			  DRM_MODE_LUT_INTERPOLATE | \
			  DRM_MODE_LUT_NON_DECREASING), \
		.count = 128, \
		.input_bpc = 10, .output_bpc = 16, \
		.start = 0, .end = (1 << 10) - (1 << 10) / 128, \
		.min = 0, .max = (1 << 16) - 1, \
	}, \
	/* PIPEGCMAX */ \
	{ \
		.flags = (DRM_MODE_LUT_GAMMA | \
			  DRM_MODE_LUT_INTERPOLATE | \
			  DRM_MODE_LUT_REUSE_LAST | \
			  DRM_MODE_LUT_NON_DECREASING), \
		.count = 1, \
		.input_bpc = 10, .output_bpc = 16, \
		.start = (1 << 10) - (1 << 10) / 128, .end = 1 << 10, \
		.min = 0, .max = 1 << 16, \
	}

static const struct drm_color_lut_range i965_gamma_10[] = {
	I965_GAMMA_10,
};

#define CHV_CGM_DEGAMMA \
	{ \
		.flags = (DRM_MODE_LUT_DEGAMMA | \
			  DRM_MODE_LUT_INTERPOLATE | \
			  DRM_MODE_LUT_NON_DECREASING), \
		.count = 65, \
		.input_bpc = 10, .output_bpc = 14, \
		.start = 0, .end = 1 << 10, \
		.min = 0, .max = (1 << 14) - 1, \
	}
#define CHV_CGM_GAMMA \
	{ \
		.flags = (DRM_MODE_LUT_GAMMA | \
			  DRM_MODE_LUT_INTERPOLATE | \
			  DRM_MODE_LUT_NON_DECREASING), \
		.count = 257, \
		.input_bpc = 14, .output_bpc = 10, \
		.start = 0, .end = 1 << 14, \
		.min = 0, .max = (1 << 10) - 1, \
	}

static const struct drm_color_lut_range chv_cgm_degamma[] = {
	CHV_CGM_DEGAMMA,
};

static const struct drm_color_lut_range chv_cgm_gamma[] = {
	CHV_CGM_GAMMA,
};

static const struct drm_color_lut_range chv_cgm_degamma_i9xx_gamma_8[] = {
	CHV_CGM_DEGAMMA,
	I9XX_GAMMA_8,
};

static const struct drm_color_lut_range chv_cgm_degamma_i965_gamma_10[] = {
	CHV_CGM_DEGAMMA,
	I965_GAMMA_10,
};

static const struct drm_color_lut_range chv_cgm_degamma_cgm_degamma[] = {
	CHV_CGM_DEGAMMA,
	CHV_CGM_GAMMA,
};

static const struct drm_color_lut_range ilk_gamma_degamma_8[] = {
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_DEGAMMA),
		.count = 256,
		.input_bpc = 8, .output_bpc = 8,
		.start = 0, .end = (1 << 8) - 1,
		.min = 0, .max = (1 << 8) - 1,
	},
};

static const struct drm_color_lut_range ilk_gamma_degamma_10[] = {
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_DEGAMMA),
		.count = 1024,
		.input_bpc = 10, .output_bpc = 10,
		.start = 0, .end = (1 << 10) - 1,
		.min = 0, .max = (1 << 10) - 1,
	},
};

static const struct drm_color_lut_range ilk_gamma_degamma_12p4[] = {
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_DEGAMMA |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 512,
		.input_bpc = 12, .output_bpc = 16,
		.start = 0, .end = (1 << 12) - (1 << 12) / 512,
		.min = 0, .max = (1 << 16) - 1,
	},
	/* PIPEGCMAX */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_DEGAMMA |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 10, .output_bpc = 16,
		.start = (1 << 12) - (1 << 12) / 512, .end = 1 << 12,
		.min = 0, .max = 1 << 16,
	},
};

static const struct drm_color_lut_range ivb_gamma_degamma_10[] = {
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_DEGAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE),
		.count = 1024,
		.input_bpc = 10, .output_bpc = 10,
		.start = 0, .end = (1 << 10) - 1,
		.min = 0, .max = (1 << 10) - 1,
	},
	/* PAL_EXT_GC_MAX */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_DEGAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 10, .output_bpc = 16,
		.start = 1 << 10, .end = 3 << 10,
		.min = 0, .max = (8 << 16) - 1,
	}
};

static const struct drm_color_lut_range glk_gamma_10[] = {
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE),
		.count = 1024,
		.input_bpc = 10, .output_bpc = 10,
		.start = 0, .end = (1 << 10) - 1,
		.min = 0, .max = (1 << 10) - 1,
	},
	/* PAL_EXT_GC_MAX */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 10, .output_bpc = 16,
		.start = 1 << 10, .end = 3 << 10,
		.min = 0, .max = (8 << 16) - 1,
	},
	/* PAL_EXT2_GC_MAX */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 10, .output_bpc = 16,
		.start = 3 << 12, .end = 7 << 12,
		.min = 0, .max = (8 << 16) - 1,
	},
};

static const struct drm_color_lut_range ivb_split_gamma[] = {
	{
		.flags = (DRM_MODE_LUT_DEGAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE),
		.count = 512,
		.input_bpc = 9, .output_bpc = 10,
		.start = 0, .end = (1 << 9) - 1,
		.min = 0, .max = (1 << 10) - 1,
	},
	/* PAL_EXT_GC_MAX */
	{
		.flags = (DRM_MODE_LUT_DEGAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 10, .output_bpc = 16,
		.start = 1 << 9, .end = 3 << 9,
		.min = 0, .max = (8 << 16) - 1,
	},
	{
		.flags = DRM_MODE_LUT_GAMMA,
		.count = 512,
		.input_bpc = 9, .output_bpc = 10,
		.start = 0, .end = (1 << 9) - 1,
		.min = 0, .max = (1 << 10) - 1,
	},
};

/* FIXME input bpc? */
static const struct drm_color_lut_range ivb_gamma_degamma_12p4[] = {
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_DEGAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 512,
		.input_bpc = 12, .output_bpc = 16,
		.start = 0, .end = (1 << 12) - (1 << 12) / 512,
		.min = 0, .max = (1 << 16) - 1,
	},
	/* PAL_GC_MAX */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_DEGAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 12, .output_bpc = 16,
		.start = (1 << 12) - (1 << 12) / 512, .end = 1 << 12,
		.min = 0, .max = 1 << 16,
	},
	/* PAL_EXT_GC_MAX */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_DEGAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 12, .output_bpc = 16,
		.start = 1 << 12, .end = 3 << 12,
		.min = 0, .max = (8 << 16) - 1,
	},
};

/* FIXME input bpc? */
static const struct drm_color_lut_range glk_gamma_12p4[] = {
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 512,
		.input_bpc = 16, .output_bpc = 16,
		.start = 0, .end = (1 << 16) - (1 << 16) / 512,
		.min = 0, .max = (1 << 16) - 1,
	},
	/* PAL_GC_MAX */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 16, .output_bpc = 16,
		.start = (1 << 16) - (1 << 16) / 512, .end = 1 << 16,
		.min = 0, .max = 1 << 16,
	},
	/* PAL_EXT_GC_MAX */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 16, .output_bpc = 16,
		.start = 1 << 16, .end = 3 << 16,
		.min = 0, .max = (8 << 16) - 1,
	},
	/* PAL_EXT2_GC_MAX */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 16, .output_bpc = 16,
		.start = 3 << 16, .end = 7 << 16,
		.min = 0, .max = (8 << 16) - 1,
	},
};

 /* FIXME input bpc? */
static const struct drm_color_lut_range icl_multi_seg_gamma[] = {
	/* segment 1 aka. super fine segment */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 257,
		.input_bpc = 24, .output_bpc = 16,
		.start = 0, .end = (1 << 24) / (128 * 256),
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 2 aka. fine segment */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 257,
		.input_bpc = 24, .output_bpc = 16,
		.start = 0, .end = (1 << 24) / 128,
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 3 aka. coarse segment */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 256,
		.input_bpc = 24, .output_bpc = 16,
		.start = 0, .end = (1 << 24) - (1 << 24) / 256,
		.min = 0, .max = (1 << 16) - 1,
	},
	/* segment 3 aka. coarse segment / PAL_GC_MAX */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 24, .output_bpc = 16,
		.start = (1 << 24) - (1 << 24) / 256, .end = 1 << 24,
		.min = 0, .max = 1 << 16,
	},
	/* PAL_EXT_GC_MAX */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 24, .output_bpc = 16,
		.start = 1 << 24, .end = 3 << 24,
		.min = 0, .max = (8 << 16) - 1,
	},
	/* PAL_EXT2_GC_MAX */
	{
		.flags = (DRM_MODE_LUT_GAMMA |
			  DRM_MODE_LUT_REFLECT_NEGATIVE |
			  DRM_MODE_LUT_INTERPOLATE |
			  DRM_MODE_LUT_REUSE_LAST |
			  DRM_MODE_LUT_NON_DECREASING),
		.count = 1,
		.input_bpc = 24, .output_bpc = 16,
		.start = 3 << 24, .end = 7 << 24,
		.min = 0, .max = (8 << 16) - 1,
	},
};

void intel_color_init(struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	int degamma_lut_size, gamma_lut_size;
	bool has_ctm;

	/*
	 * FIXME use the wide gamut csc as ctm on VLV. There is no degamma
	 * unit however. Could at least use it for rgb->ycbcr. Should
	 * probably keep using it for that on CHV as well so that we can
	 * keep the CGM csc purely for ctm.
	 */

	if (HAS_GMCH_DISPLAY(dev_priv)) {
		if (IS_CHERRYVIEW(dev_priv)) {
			degamma_lut_size = CHV_LUT_SIZE_CGM_DEGAMMA;
			gamma_lut_size = CHV_LUT_SIZE_CGM_GAMMA;
			has_ctm = true;

			dev_priv->display.load_luts = chv_load_luts;
			dev_priv->display.color_check = chv_color_check;
			dev_priv->display.color_commit = chv_color_commit;

			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "8bit gamma",
						       i9xx_gamma_8,
						       sizeof(i9xx_gamma_8));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "interpolated gamma",
						       i965_gamma_10,
						       sizeof(i965_gamma_10));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "CGM gamma",
						       chv_cgm_gamma,
						       sizeof(chv_cgm_gamma));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "CGM degamma",
						       chv_cgm_degamma,
						       sizeof(chv_cgm_degamma));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "CGM degamma with 8bit gamma",
						       chv_cgm_degamma_i9xx_gamma_8,
						       sizeof(chv_cgm_degamma));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "CGM degamma with 10bit interpolated gamma",
						       chv_cgm_degamma_i965_gamma_10,
						       sizeof(chv_cgm_degamma));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "CGM degamma with CGM gamma",
						       chv_cgm_degamma_cgm_degamma,
						       sizeof(chv_cgm_degamma));
		} else if (INTEL_GEN(dev_priv) >= 4) {
			/* 10bit interpolated gamma */
			degamma_lut_size = 0;
			gamma_lut_size = I9XX_LUT_SIZE_10BIT;
			has_ctm = false;

			dev_priv->display.load_luts = i965_load_luts;
			dev_priv->display.color_check = i9xx_color_check;
			dev_priv->display.color_commit = i9xx_color_commit;

			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "8bit gamma",
						       i9xx_gamma_8,
						       sizeof(i9xx_gamma_8));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "interpolated gamma",
						       i965_gamma_10,
						       sizeof(i965_gamma_10));
		} else {
			dev_priv->display.load_luts = i9xx_load_luts;
			dev_priv->display.color_check = i9xx_color_check;
			dev_priv->display.color_commit = i9xx_color_commit;

			degamma_lut_size = 0;
			gamma_lut_size = 0;
			has_ctm = false;

			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "8bit gamma",
						       i9xx_gamma_8,
						       sizeof(i9xx_gamma_8));

			if (i9xx_has_10bit_lut(dev_priv)) {
				/* 10bit interpolated gamma */
				gamma_lut_size = I9XX_LUT_SIZE_10BIT;

				drm_color_add_gamma_mode_range(&dev_priv->drm,
							       "interpolated gamma",
							       i9xx_gamma_10_slope,
							       sizeof(i9xx_gamma_10_slope));
			}
		}
	} else {
		if (INTEL_GEN(dev_priv) >= 11) {
			//dev_priv->display.load_luts = icl_load_luts;
			//dev_priv->display.color_check = icl_color_check;
			dev_priv->display.color_commit = skl_color_commit;

			/* don't advertize the >= 1.0 entries */
			degamma_lut_size = 0;
			gamma_lut_size = ILK_LUT_SIZE_10BIT;
			has_ctm = true;

			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "8bit gamma",
						       i9xx_gamma_8,
						       sizeof(i9xx_gamma_8));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "10bit gamma",
						       glk_gamma_10,
						       sizeof(glk_gamma_10));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "interpolated gamma",
						       glk_gamma_12p4,
						       sizeof(glk_gamma_12p4));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "multi-segmented gamma",
						       icl_multi_seg_gamma,
						       sizeof(icl_multi_seg_gamma));
		} else if (INTEL_GEN(dev_priv) >= 10 || IS_GEMINILAKE(dev_priv)) {
			dev_priv->display.load_luts = glk_load_luts;
			dev_priv->display.color_check = glk_color_check;
			dev_priv->display.color_commit = skl_color_commit;

			/* don't advertize the >= 1.0 entries */
			degamma_lut_size = 0;
			gamma_lut_size = ILK_LUT_SIZE_10BIT;
			has_ctm = true;

			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "8bit gamma",
						       i9xx_gamma_8,
						       sizeof(i9xx_gamma_8));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "10bit gamma",
						       glk_gamma_10,
						       sizeof(glk_gamma_10));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "interpolated gamma",
						       glk_gamma_12p4,
						       sizeof(glk_gamma_12p4));
		} else if (INTEL_GEN(dev_priv) >= 9) {
			dev_priv->display.load_luts = ivb_load_luts;
			dev_priv->display.color_check = ivb_color_check;
			dev_priv->display.color_commit = skl_color_commit;

			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "8bit gamma or degamma",
						       ilk_gamma_degamma_8,
						       sizeof(ilk_gamma_degamma_8));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "split gamma",
						       ivb_split_gamma,
						       sizeof(ivb_split_gamma));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "10bit gamma or degamma",
						       ivb_gamma_degamma_10,
						       sizeof(ivb_gamma_degamma_10));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "interpolated gamma or degamma",
						       ivb_gamma_degamma_12p4,
						       sizeof(ivb_gamma_degamma_12p4));

			/* don't advertize the >= 1.0 entries */
			degamma_lut_size = IVB_LUT_SIZE_SPLIT;
			gamma_lut_size = IVB_LUT_SIZE_SPLIT;
			has_ctm = true;
		} else if (INTEL_GEN(dev_priv) >= 8 || IS_HASWELL(dev_priv)) {
			dev_priv->display.load_luts = ivb_load_luts;
			dev_priv->display.color_check = ivb_color_check;
			dev_priv->display.color_commit = hsw_color_commit;

			/* don't advertize the >= 1.0 entries */
			degamma_lut_size = IVB_LUT_SIZE_SPLIT;
			gamma_lut_size = IVB_LUT_SIZE_SPLIT;
			has_ctm = true;

			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "8bit gamma or degamma",
						       ilk_gamma_degamma_8,
						       sizeof(ilk_gamma_degamma_8));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "split gamma",
						       ivb_split_gamma,
						       sizeof(ivb_split_gamma));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "10bit gamma or degamma",
						       ivb_gamma_degamma_10,
						       sizeof(ivb_gamma_degamma_10));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "interpolated gamma or degamma",
						       ivb_gamma_degamma_12p4,
						       sizeof(ivb_gamma_degamma_12p4));
		} else if (IS_IVYBRIDGE(dev_priv)) {
			dev_priv->display.load_luts = ivb_load_luts;
			dev_priv->display.color_check = ivb_color_check;
			dev_priv->display.color_commit = ilk_color_commit;

			/* don't advertize the >= 1.0 entries */
			degamma_lut_size = IVB_LUT_SIZE_SPLIT;
			gamma_lut_size = IVB_LUT_SIZE_SPLIT;
			has_ctm = true;

			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "8bit gamma or degamma",
						       ilk_gamma_degamma_8,
						       sizeof(ilk_gamma_degamma_8));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "split gamma",
						       ivb_split_gamma,
						       sizeof(ivb_split_gamma));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "10bit gamma or degamma",
						       ivb_gamma_degamma_10,
						       sizeof(ivb_gamma_degamma_10));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "interpolated gamma or degamma",
						       ivb_gamma_degamma_12p4,
						       sizeof(ivb_gamma_degamma_12p4));
		} else {
			dev_priv->display.load_luts = ilk_load_luts;
			dev_priv->display.color_check = ilk_color_check;
			dev_priv->display.color_commit = ilk_color_commit;

			degamma_lut_size = 0;
			gamma_lut_size = ILK_LUT_SIZE_10BIT;
			has_ctm = true;

			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "8bit gamma or degamma",
						       ilk_gamma_degamma_8,
						       sizeof(ilk_gamma_degamma_8));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "10bit gamma or degamma",
						       ilk_gamma_degamma_10,
						       sizeof(ilk_gamma_degamma_10));
			drm_color_add_gamma_mode_range(&dev_priv->drm,
						       "interpolated gamma or degamma",
						       ilk_gamma_degamma_12p4,
						       sizeof(ilk_gamma_degamma_12p4));
		}
	}

	drm_mode_crtc_set_gamma_size(&crtc->base, 256);

	drm_crtc_enable_color_mgmt(&crtc->base,
				   degamma_lut_size,
				   has_ctm,
				   gamma_lut_size);
}
