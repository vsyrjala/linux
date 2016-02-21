/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#ifndef DRM_DP_DUAL_MODE_HELPER_H
#define DRM_DP_DUAL_MODE_HELPER_H

#include <linux/types.h>

/*
 * Optional for type 1 DVI adaptors
 * Mandatory for type 1 HDMI and type 2 adators
 */
#define DP_DUAL_MODE_HDMI_ID 0x00 /* 00-0f */
#define  DP_DUAL_MODE_HDMI_ID_LEN 16
/*
 * Optional for type 1 adaptors
 * Mandatory for type 2 adators
 */
#define DP_DUAL_MODE_ADAPTOR_ID 0x10
#define  DP_DUAL_MODE_REV_MASK 0x07
#define  DP_DUAL_MODE_REV_TYPE2 0x00
#define  DP_DUAL_MODE_TYPE_MASK 0xf0
#define  DP_DUAL_MODE_TYPE_TYPE2 0xa0
#define DP_DUAL_MODE_IEEE_OUI 0x11 /* 11-13*/
#define  DP_DUAL_IEEE_OUI_LEN 3
#define DP_DUAL_DEVICE_ID 0x14 /* 14-19 */
#define  DP_DUAL_DEVICE_ID_LEN 6
#define DP_DUAL_MODE_HARDWARE_REV 0x1a
#define DP_DUAL_MODE_FIRMWARE_MAJOR_REV 0x1b
#define DP_DUAL_MODE_FIRMWARE_MINOR_REV 0x1c
#define DP_DUAL_MODE_MAX_TMDS_CLOCK 0x1d
#define DP_DUAL_MODE_I2C_SPEED_CAP 0x1e
#define DP_DUAL_MODE_TMDS_OEN 0x20
#define  DP_DUAL_MODE_TMDS_DISABLE 0x01
#define DP_DUAL_MODE_HDMI_PIN_CTRL 0x21
#define  DP_DUAL_MODE_CEC_ENABLE 0x01
#define DP_DUAL_MODE_I2C_SPEED_CTRL 0x22
#define DP_DUAL_MODE_LAST_RESERVED 0xff

struct i2c_adapter;

ssize_t drm_dp_dual_mode_read(struct i2c_adapter *adapter,
			      u8 offset, void *buffer, size_t size);
ssize_t drm_dp_dual_mode_write(struct i2c_adapter *adapter,
			       u8 offset, const void *buffer, size_t size);

enum drm_dp_dual_mode_type {
	DRM_DP_DUAL_MODE_NONE,
	DRM_DP_DUAL_MODE_TYPE1_DVI,
	DRM_DP_DUAL_MODE_TYPE1_HDMI,
	DRM_DP_DUAL_MODE_TYPE2_DVI,
	DRM_DP_DUAL_MODE_TYPE2_HDMI,
};

enum drm_dp_dual_mode_type drm_dp_dual_mode_detect(struct i2c_adapter *adapter);
int drm_dp_dual_mode_max_tmds_clock(struct i2c_adapter *adapter);
int drm_dp_dual_mode_get_tmds_output(struct i2c_adapter *adapter, bool *enabled);
int drm_dp_dual_mode_set_tmds_output(struct i2c_adapter *adapter, bool enable);
const char *drm_dp_get_dual_mode_type_name(enum drm_dp_dual_mode_type type);

#endif
