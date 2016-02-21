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

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <drm/drm_dp_dual_mode_helper.h>
#include <drm/drmP.h>

/**
 * DOC: DP dual mode adaptor (aka. DP++) helpers
 *
 * Helpers to deal with DP dual mode adaptors (aka. DP++).
 * Type 1 adaptor registers (if any) may be accessed via DDC.
 * Type 2 adaptor can be accesses either via DDC or I2C-over-AUX.
 * Type 2 compatible source devices may choose to implement either
 * or both of these access methods.
 */

#define DP_DUAL_MODE_SLAVE_ADDRESS 0x40

/**
 * drm_dp_dual_mode_read - Read from the DP dual mode adaptor register(s)
 * adapter: I2C adapter for the DDC bus
 * offset: register offset
 * buffer: buffer for return data
 * size: sizo of the buffer
 *
 * Reads @size bytes from the DP dual mode adaptor registers
 * starting at @offset.
 *
 * Returns:
 * 0 on success, negative error code on failure
 */
ssize_t drm_dp_dual_mode_read(struct i2c_adapter *adapter,
			      u8 offset, void *buffer, size_t size)
{
	struct i2c_msg msgs[] = {
		{
			.addr = DP_DUAL_MODE_SLAVE_ADDRESS,
			.flags = 0,
			.len = 1,
			.buf = &offset,
		},
		{
			.addr = DP_DUAL_MODE_SLAVE_ADDRESS,
			.flags = I2C_M_RD,
			.len = size,
			.buf = buffer,
		},
	};
	int ret;

	ret = i2c_transfer(adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EPROTO;

	return 0;
}

/**
 * drm_dp_dual_mode_write - Write to the DP dual mode adaptor register(s)
 * adapter: I2C adapter for the DDC bus
 * offset: register offset
 * buffer: buffer for write data
 * size: sizo of the buffer
 *
 * Writes @size bytes to the DP dual mode adaptor registers
 * starting at @offset.
 *
 * Returns:
 * 0 on success, negative error code on failure
 */
ssize_t drm_dp_dual_mode_write(struct i2c_adapter *adapter,
			       u8 offset, const void *buffer, size_t size)
{
	struct i2c_msg msg = {
		.addr = DP_DUAL_MODE_SLAVE_ADDRESS,
		.flags = 0,
		.len = 1 + size,
		.buf = NULL,
	};
	void *data;
	int ret;

	data = kmalloc(msg.len, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	msg.buf = data;

	memcpy(data, &offset, 1);
	memcpy(data + 1, buffer, size);

	ret = i2c_transfer(adapter, &msg, 1);

	kfree(data);

	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EPROTO;

	return 0;
}
EXPORT_SYMBOL(drm_dp_dual_mode_write);

static bool is_hdmi_adaptor(const char hdmi_id[DP_DUAL_MODE_HDMI_ID_LEN])
{
	static const char dp_dual_mode_hdmi_id[DP_DUAL_MODE_HDMI_ID_LEN] =
		"DP-HDMI ADAPTOR\x04";

	return memcmp(hdmi_id, dp_dual_mode_hdmi_id,
		      sizeof(dp_dual_mode_hdmi_id)) == 0;
}

/**
 * drm_dp_dual_mode_detect - Identyfy the DP dual mode adaptor
 * adapter: I2C adapter for the DDC bus
 *
 * Attempt to identify the type of the DP dual mode adaptor used.
 *
 * Note that when the answer is @DRM_DP_DUAL_MODE_NONE it's not
 * certain whether we're dealing with a native HDMI port or
 * a type 1 DVI dual mode adaptor. The driver will have to use
 * some other hardware/driver specific mechanism to make that
 * distinction.
 *
 * Returns:
 * The type of the DP dual mode adaptor used
 */
enum drm_dp_dual_mode_type drm_dp_dual_mode_detect(struct i2c_adapter *adapter)
{
	char hdmi_id[DP_DUAL_MODE_HDMI_ID_LEN] = {};
	uint8_t adaptor_id = 0x00;
	ssize_t ret;

	/*
	 * Let's see if the adaptor is there the by reading the
	 * HDMI ID registers.
	 *
	 * Note that type 1 DVI adaptors are not required to implemnt
	 * any registers which presents a problem for detection. If
	 * the i2c transfer is nacked, we may or may not be dealing
	 * with a type 1 DVI adaptor. Some other mechanism of detecting
	 * the presence of the adaptor would then be needed. One way
	 * would be to check the state of the CONFIG1 pin, Another
	 * method would simply require the driver to know whether
	 * the port is a DP++ port or a native HDMI port. Both of
	 * these methods are entirely hardware/driver specific so
	 * we can't deal with them here.
	 *
	 * The problem with failing to detect the type 1 DVI adaptor
	 * is that we moght fail to limit the max TMDS clock to the
	 * correct value (165Mhz), thus presenting the user with
	 * display modes that exceed the capabilities of the adaptor.
	 */
	ret = drm_dp_dual_mode_read(adapter, DP_DUAL_MODE_HDMI_ID,
				    hdmi_id, sizeof(hdmi_id));
	if (ret)
		return DRM_DP_DUAL_MODE_NONE;

	/*
	 * Sigh. Some broken type 1 adaptors ack the offset but
	 * ignore it, and instead they just always return data from
	 * the start of the HDMI ID buffer. So for a broken type 1
	 * HDMI adaptor a single byte read will always give us 0x44,
	 * and for type 1 DVI it should give 0x00 (assuming it
	 * implements any registers). Fortunately neither of those
	 * values will match the type 2 signature of the
	 * DP_DUAL_MODE_ADAPTOR_ID register so we can proceed with
	 * the type 2 adaptor detection safely, even in the presence
	 * of broken type 1 adaptors.
	 */
	ret = drm_dp_dual_mode_read(adapter, DP_DUAL_MODE_ADAPTOR_ID,
				    &adaptor_id, sizeof(adaptor_id));
	if (ret || (adaptor_id != (DP_DUAL_MODE_TYPE_TYPE2 |
				   DP_DUAL_MODE_REV_TYPE2))) {
		if (is_hdmi_adaptor(hdmi_id))
			return DRM_DP_DUAL_MODE_TYPE1_HDMI;
		else
			return DRM_DP_DUAL_MODE_TYPE1_DVI;
	} else {
		if (is_hdmi_adaptor(hdmi_id))
			return DRM_DP_DUAL_MODE_TYPE2_HDMI;
		else
			return DRM_DP_DUAL_MODE_TYPE2_DVI;
	}
}
EXPORT_SYMBOL(drm_dp_dual_mode_detect);

/**
 * drm_dp_dual_mode_max_tmds_clock - Max TMDS clock for DP dual mode adaptor
 * adapter: I2C adapter for the DDC bus
 *
 * Determine the max TMDS clock the adaptor supports based on the
 * DP_DUAL_MODE_MAX_TMDS_CLOCK register. The register is mandatory for
 * type 2 adaptors, optional for type 1 adaptors. Hoever, as some type 1
 * adaptors are broken (see comments in drm_dp_dual_mode_detect() for the
 * details) one probably shouldn't use this with type 1 adaptors at all.
 * Type 1 adaptors should anyway be always limited to 165 MHz.
 *
 * Returns:
 * Maximum supported TMDS clock rate for the DP dual mode adaptor in kHz.
 */
int drm_dp_dual_mode_max_tmds_clock(struct i2c_adapter *adapter)
{
	uint8_t max_tmds_clock;
	ssize_t ret;

	/*
	 * Type 1 adaptors are limited to 165MHz
	 * Type 2 adaptors can tells us their limit
	 */
	ret = drm_dp_dual_mode_read(adapter, DP_DUAL_MODE_MAX_TMDS_CLOCK,
				    &max_tmds_clock, sizeof(max_tmds_clock));
	if (ret)
		return 165000;

	return max_tmds_clock * 5000 / 2;
}
EXPORT_SYMBOL(drm_dp_dual_mode_max_tmds_clock);

/**
 * drm_dp_dual_mode_get_tmds_output - Get the state of the TMDS output buffers in the DP dual mode adaptor
 * adapter: I2C adapter for the DDC bus
 * enabled: current state of the TMDS output buffers
 *
 * Get the state of the TMDS output buffers in the adaptor.
 * DP_DUAL_MODE_TMDS_OEN register is mandatory for type 2 adaptors,
 * optionals for type 1 adaptors. Hoever, as some type 1 adaptors are
 * broken (see comments in drm_dp_dual_mode_detect() for the details)
 * one probably shouldn't use this with type 1 adaptors at all.
 *
 * Returns:
 * 0 on success, negative error code on failure
 */
int drm_dp_dual_mode_get_tmds_output(struct i2c_adapter *adapter, bool *enabled)
{
	uint8_t tmds_oen;
	ssize_t ret;

	ret = drm_dp_dual_mode_read(adapter, DP_DUAL_MODE_TMDS_OEN,
				    &tmds_oen, sizeof(tmds_oen));
	if (ret)
		return ret;

	*enabled = !(tmds_oen & DP_DUAL_MODE_TMDS_DISABLE);

	return 0;
}
EXPORT_SYMBOL(drm_dp_dual_mode_get_tmds_output);

/**
 * drm_dp_dual_mode_set_tmds_output - Enable/disable TMDS output buffers in the DP dual mode adaptor
 * adapter: I2C adapter for the DDC bus
 * enable: enable (as opposed to disable) the TMDS output buffers
 *
 * Set the state of the TMDS output buffers in the adaptor.
 * DP_DUAL_MODE_TMDS_OEN register is mandatory for type 2 adaptors,
 * optionals for type 1 adaptors. Hoever, as some type 1 adaptors are
 * broken (see comments in drm_dp_dual_mode_detect() for the details)
 * one probably shouldn't use this with type 1 adaptors at all.
 *
 * Returns:
 * 0 on success, negative error code on failure
 */
int drm_dp_dual_mode_set_tmds_output(struct i2c_adapter *adapter, bool enable)
{
	uint8_t tmds_oen = enable ? 0 : DP_DUAL_MODE_TMDS_DISABLE;
	ssize_t ret;

	ret = drm_dp_dual_mode_write(adapter, DP_DUAL_MODE_TMDS_OEN,
				     &tmds_oen, sizeof(tmds_oen));
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL(drm_dp_dual_mode_set_tmds_output);

/**
 * drm_dp_get_dual_mode_type_name - Get the name of the DP dual mode adaptor type as a string
 * type: DP dual mode adaptor type
 *
 * Returns:
 * String represantion of the DP dual mode adaptor type
 */
const char *drm_dp_get_dual_mode_type_name(enum drm_dp_dual_mode_type type)
{
	switch (type) {
	case DRM_DP_DUAL_MODE_NONE:
		return "none";
	case DRM_DP_DUAL_MODE_TYPE1_DVI:
		return "type 1 DVI";
	case DRM_DP_DUAL_MODE_TYPE1_HDMI:
		return "type 1 HDMI";
	case DRM_DP_DUAL_MODE_TYPE2_DVI:
		return "type 2 DVI";
	case DRM_DP_DUAL_MODE_TYPE2_HDMI:
		return "type 2 HDMI";
	default:
		return "unknown";
	};

}
EXPORT_SYMBOL(drm_dp_get_dual_mode_type_name);
