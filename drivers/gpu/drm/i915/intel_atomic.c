/*
 * Copyright (C) 2011-2012 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 * Ville Syrjälä <ville.syrjala@linux.intel.com>
 */

/*
 * TODO:
 * - check crtc.enabled old vs. new state handling
 * - generate a correct timestamp for override flips
 * - send flip events at correct time when previous flip is pending,
 *   and nothing changed in the later flip
 * - flip_seq should be the same for all flips issued at the same time
 * - make GPU reset handling robust
 * - old style frame counter still has possible issues
 * - move primary plane scanout handling into a drm_plane
 * - cursor register handling needs work
 * - should drm_plane be used for cursors too?
 * - more refactoring and cleanups
 */

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_flip.h>

#include "intel_drv.h"

#define USE_WRITE_SEQNO

//#define SURFLIVE_DEBUG

struct intel_flip {
	struct drm_flip base;
	u32 vbl_count;
	bool vblank_ref;
	bool has_cursor;
	struct drm_crtc *crtc;
	struct drm_plane *plane;
	struct drm_i915_gem_object *old_bo;
	struct drm_i915_gem_object *old_cursor_bo;
	struct drm_pending_atomic_event *event;
	uint32_t old_fb_id;
	struct list_head pending_head;
	/* FIXME need cursor regs too */
	struct intel_plane_regs regs;
	struct intel_ring_buffer *ring;
	u32 seqno;
	unsigned int flip_seq;
#ifdef SURFLIVE_DEBUG
	u32 commit_dsl;
	u32 commit_surf;
	u32 commit_surflive;
#endif
};

struct intel_plane_state {
	struct drm_plane *plane;
	struct intel_plane_coords coords;
	bool dirty;
	bool pinned;
	bool changed;
	struct drm_pending_atomic_event *event;
	struct intel_flip *flip;

	struct {
		struct drm_crtc *crtc;
		struct drm_framebuffer *fb;
		uint32_t src_x, src_y, src_w, src_h;
		int32_t crtc_x, crtc_y;
		uint32_t crtc_w, crtc_h;
	} old;
};

struct intel_crtc_state {
	struct drm_crtc *crtc;
	bool mode_dirty;
	bool fb_dirty;
	bool cursor_dirty;
	bool active_dirty;
	bool pinned;
	bool cursor_pinned;
	unsigned long connectors_bitmask;
	unsigned long encoders_bitmask;
	bool changed;
	struct drm_pending_atomic_event *event;
	struct intel_flip *flip;
	bool primary_disabled;

	struct {
		bool enabled;
		struct drm_display_mode mode;
		struct drm_framebuffer *fb;
		int x, y;
		unsigned long connectors_bitmask;
		unsigned long encoders_bitmask;

		struct drm_i915_gem_object *cursor_bo;
		uint32_t cursor_handle;
		int16_t cursor_x, cursor_y;
		int16_t cursor_width, cursor_height;
		bool cursor_visible;
	} old;
};

struct intel_atomic_state {
	struct drm_file *file;
	struct intel_plane_state *plane;
	struct intel_crtc_state *crtc;
	bool dirty;
	bool restore_state;
	bool restore_hw;
	unsigned int flags;
	uint64_t user_data;
	struct drm_plane *saved_planes;
	struct intel_crtc *saved_crtcs;
	struct drm_connector *saved_connectors;
	struct drm_encoder *saved_encoders;
};

static void update_connectors_bitmask(struct intel_crtc *intel_crtc,
				      unsigned long *connectors_bitmask)
{
	struct drm_device *dev = intel_crtc->base.dev;
	struct intel_connector *intel_connector;
	unsigned int i = 0;

	*connectors_bitmask = 0;

	list_for_each_entry(intel_connector, &dev->mode_config.connector_list, base.head) {
		if (intel_connector->new_encoder &&
		    intel_connector->new_encoder->new_crtc == intel_crtc)
			__set_bit(i, connectors_bitmask);

		i++;
	}
}

static void update_encoders_bitmask(struct intel_crtc *intel_crtc,
				    unsigned long *encoders_bitmask)
{
	struct drm_device *dev = intel_crtc->base.dev;
	struct intel_encoder *intel_encoder;
	unsigned int i = 0;

	*encoders_bitmask = 0;

	list_for_each_entry(intel_encoder, &dev->mode_config.encoder_list, base.head) {
		if (intel_encoder->new_crtc == intel_crtc)
			__set_bit(i, encoders_bitmask);

		i++;
	}
}

static bool intel_crtc_in_use(struct intel_crtc *intel_crtc)
{
	struct drm_device *dev = intel_crtc->base.dev;
	struct intel_encoder *intel_encoder;

	list_for_each_entry(intel_encoder, &dev->mode_config.encoder_list, base.head)
		if (intel_encoder->new_crtc == intel_crtc)
			return true;

	return false;
}

static bool intel_encoder_in_use(struct intel_encoder *intel_encoder)
{
	struct drm_device *dev = intel_encoder->base.dev;
	struct intel_connector *intel_connector;

	list_for_each_entry(intel_connector, &dev->mode_config.connector_list, base.head)
		if (intel_connector->new_encoder == intel_encoder)
			return true;

	return false;
}

static int process_connectors(struct intel_crtc_state *s, const uint32_t *ids, int count_ids)
{
	struct drm_crtc *crtc = s->crtc;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	struct drm_connector *connectors[count_ids];
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	int i;

	for (i = 0; i < count_ids; i++) {
		struct drm_mode_object *obj;
		int j;

		/* don't accept duplicates */
		for (j = i + 1; j < count_ids; j++)
			if (ids[i] == ids[j])
				return -EINVAL;

		obj = drm_mode_object_find(dev, ids[i], DRM_MODE_OBJECT_CONNECTOR);
		if (!obj) {
			DRM_DEBUG_KMS("Unknown connector ID %u\n", ids[i]);
			return -ENOENT;
		}

		connector = obj_to_connector(obj);

		encoder = intel_best_encoder(connector);

		if (!intel_encoder_crtc_ok(encoder, crtc))
			return -EINVAL;

		connectors[i] = connector;
	}

	list_for_each_entry(connector, &dev->mode_config.connector_list, head) {
		struct intel_connector *intel_connector = to_intel_connector(connector);
		struct intel_encoder *intel_encoder;

		for (i = 0; i < count_ids; i++) {
			if (connector == connectors[i])
				break;
		}

		/* this connector isn't in the set */
		if (i == count_ids) {
			/* remove the link to the encoder if this crtc was set to drive it */
			if (intel_connector->new_encoder &&
			    intel_connector->new_encoder->new_crtc == intel_crtc)
				intel_connector->new_encoder = NULL;
			continue;
		}

		intel_encoder = to_intel_encoder(intel_best_encoder(connector));

		intel_connector->new_encoder = intel_encoder;
		intel_encoder->new_crtc = intel_crtc;
	}

	/* prune dangling encoder->crtc links pointing to this crtc  */
	list_for_each_entry(encoder, &dev->mode_config.encoder_list, head) {
		struct intel_encoder *intel_encoder = to_intel_encoder(encoder);

		if (intel_encoder->new_crtc == intel_crtc && !intel_encoder_in_use(intel_encoder))
			intel_encoder->new_crtc = NULL;
	}

	update_connectors_bitmask(intel_crtc, &s->connectors_bitmask);
	update_encoders_bitmask(intel_crtc, &s->encoders_bitmask);

	return 0;
}

static size_t intel_atomic_state_size(const struct drm_device *dev)
{
	struct intel_atomic_state *state;
	unsigned int num_connector = dev->mode_config.num_connector;
	unsigned int num_encoder = dev->mode_config.num_encoder;
	unsigned int num_crtc = dev->mode_config.num_crtc;
	unsigned int num_plane = dev->mode_config.num_plane;

	return sizeof *state +
		num_crtc * sizeof state->crtc[0] +
		num_plane * sizeof state->plane[0] +
		num_connector * sizeof state->saved_connectors[0] +
		num_encoder * sizeof state->saved_encoders[0] +
		num_crtc * sizeof state->saved_crtcs[0] +
		num_plane * sizeof state->saved_planes[0];
}

static void *intel_atomic_begin(struct drm_device *dev, struct drm_file *file,
				uint32_t flags, uint64_t user_data)
{
	struct intel_atomic_state *state;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	unsigned int num_connector = dev->mode_config.num_connector;
	unsigned int num_encoder = dev->mode_config.num_encoder;
	unsigned int num_crtc = dev->mode_config.num_crtc;
	unsigned int num_plane = dev->mode_config.num_plane;
	int i;

	state = kzalloc(intel_atomic_state_size(dev), GFP_KERNEL);
	if (!state)
		return ERR_PTR(-ENOMEM);

	state->flags = flags;
	state->file = file;
	state->user_data = user_data;

	state->crtc = (struct intel_crtc_state *)(state + 1);
	state->plane = (struct intel_plane_state  *)(state->crtc + num_crtc);

	state->saved_connectors = (struct drm_connector *)(state->plane + num_plane);
	state->saved_encoders = (struct drm_encoder *)(state->saved_connectors + num_connector);
	state->saved_crtcs = (struct intel_crtc *)(state->saved_encoders + num_encoder);
	state->saved_planes = (struct drm_plane *)(state->saved_crtcs + num_crtc);

	i = 0;
	list_for_each_entry(crtc, &dev->mode_config.crtc_list, head) {
		struct intel_crtc_state *s = &state->crtc[i++];
		struct intel_crtc *intel_crtc = to_intel_crtc(crtc);

		s->crtc = crtc;
		s->old.cursor_bo = intel_crtc->cursor_bo;
		s->old.cursor_x = intel_crtc->cursor_x;
		s->old.cursor_y = intel_crtc->cursor_y;
		s->old.cursor_width = intel_crtc->cursor_width;
		s->old.cursor_height = intel_crtc->cursor_height;
		s->old.cursor_visible = intel_crtc->cursor_visible;

		/* save current config */
		s->old.enabled = crtc->enabled;
		s->old.mode = crtc->mode;
		s->old.fb = crtc->fb;
		s->old.x = crtc->x;
		s->old.y = crtc->y;

		update_connectors_bitmask(intel_crtc, &s->connectors_bitmask);
		update_encoders_bitmask(intel_crtc, &s->encoders_bitmask);

		s->old.connectors_bitmask = s->connectors_bitmask;
		s->old.encoders_bitmask = s->encoders_bitmask;

		s->primary_disabled = intel_crtc->primary_disabled;
	}

	i = 0;
	list_for_each_entry(plane, &dev->mode_config.plane_list, head) {
		struct intel_plane_state *s = &state->plane[i++];

		s->plane = plane;

		/* save current config */
		s->old.crtc = plane->crtc;
		s->old.fb = plane->fb;
		s->old.src_x = plane->src_x;
		s->old.src_y = plane->src_y;
		s->old.src_w = plane->src_w;
		s->old.src_h = plane->src_h;
		s->old.crtc_x = plane->crtc_x;
		s->old.crtc_y = plane->crtc_y;
		s->old.crtc_w = plane->crtc_w;
		s->old.crtc_h = plane->crtc_h;
	}

	i = 0;
	list_for_each_entry(connector, &dev->mode_config.connector_list, head)
		state->saved_connectors[i++] = *connector;
	i = 0;
	list_for_each_entry(encoder, &dev->mode_config.encoder_list, head)
		state->saved_encoders[i++] = *encoder;
	i = 0;
	list_for_each_entry(crtc, &dev->mode_config.crtc_list, head) {
		struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
		state->saved_crtcs[i++] = *intel_crtc;
	}
	i = 0;
	list_for_each_entry(plane, &dev->mode_config.plane_list, head)
		state->saved_planes[i++] = *plane;

	state->file = file;

	return state;
}

static int plane_set(struct intel_atomic_state *s,
		     struct intel_plane_state *state,
		     struct drm_property *prop,
		     uint64_t value)
{
	struct drm_plane *plane = state->plane;
	struct drm_mode_config *config = &plane->dev->mode_config;
	struct drm_mode_object *obj;

	state->changed = true;

	if (prop == config->src_x_prop) {
		plane->src_x = value;
	} else if (prop == config->src_y_prop) {
		plane->src_y = value;
	} else if (prop == config->src_w_prop) {
		plane->src_w = value;
	} else if (prop == config->src_h_prop) {
		plane->src_h = value;
	} else if (prop == config->crtc_x_prop) {
		plane->crtc_x = value;
	} else if (prop == config->crtc_y_prop) {
		plane->crtc_y = value;
	} else if (prop == config->crtc_w_prop) {
		plane->crtc_w = value;
	} else if (prop == config->crtc_h_prop) {
		plane->crtc_h = value;
	} else if (prop == config->crtc_id_prop) {
		if (value) {
			obj = drm_mode_object_find(plane->dev, value, DRM_MODE_OBJECT_CRTC);
			if (!obj) {
				DRM_DEBUG_KMS("Unknown CRTC ID %llu\n", value);
				return -ENOENT;
			}
			plane->crtc = obj_to_crtc(obj);
		} else
			plane->crtc = NULL;
	} else if (prop == config->fb_id_prop) {
		if (value) {
			obj = drm_mode_object_find(plane->dev, value, DRM_MODE_OBJECT_FB);
			if (!obj) {
				DRM_DEBUG_KMS("Unknown framebuffer ID %llu\n", value);
				return -ENOENT;
			}
			plane->fb = obj_to_fb(obj);
		} else
			plane->fb = NULL;
	} else
		return -ENOENT;

	s->restore_state = true;

	return 0;
}

static int crtc_set(struct intel_atomic_state *s,
		    struct intel_crtc_state *state,
		    struct drm_property *prop,
		    uint64_t value, const void *blob_data)
{
	struct drm_crtc *crtc = state->crtc;
	struct drm_mode_config *config = &crtc->dev->mode_config;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct drm_mode_object *obj;

	state->changed = true;

	if (prop == config->src_x_prop) {
		crtc->x = value;
	} else if (prop == config->src_y_prop) {
		crtc->y = value;
	} else if (prop == config->mode_prop) {
		const struct drm_mode_modeinfo *umode = blob_data;

		if (value != 0 && value != sizeof *umode) {
			DRM_DEBUG_KMS("Invalid mode length\n");
			return -EINVAL;
		}

		if (value) {
			struct drm_display_mode *mode;
			int ret;

			mode = drm_mode_create(crtc->dev);
			if (!mode)
				return -ENOMEM;

			ret = drm_crtc_convert_umode(mode, umode);
			if (ret) {
				DRM_DEBUG_KMS("Invalid mode\n");
				drm_mode_debug_printmodeline(mode);
				drm_mode_destroy(crtc->dev, mode);
				return ret;
			}

			drm_mode_set_crtcinfo(mode, CRTC_INTERLACE_HALVE_V);

			crtc->mode = *mode;
			crtc->enabled = true;
			drm_mode_destroy(crtc->dev, mode);
		} else
			crtc->enabled = false;
	} else if (prop == config->fb_id_prop) {
		if (value) {
			obj = drm_mode_object_find(crtc->dev, value, DRM_MODE_OBJECT_FB);
			if (!obj) {
				DRM_DEBUG_KMS("Unknown framebuffer ID %llu\n", value);
				return -ENOENT;
			}
			crtc->fb = obj_to_fb(obj);
		} else
			crtc->fb = NULL;
	} else if (prop == config->connector_ids_prop) {
		const uint32_t *ids = blob_data;
		uint64_t count_ids = value / sizeof(uint32_t);
		int ret;

		if (value & 3) {
			DRM_DEBUG_KMS("Invalid connectors length\n");
			return -EINVAL;
		}

		if (count_ids > config->num_connector) {
			DRM_DEBUG_KMS("Too many connectors specified\n");
			return -ERANGE;
		}

		ret = process_connectors(state, ids, count_ids);
		if (ret)
			return ret;
	} else if (prop == config->cursor_id_prop) {
		intel_crtc->cursor_handle = value;
	} else if (prop == config->cursor_x_prop) {
		intel_crtc->cursor_x = value;
	} else if (prop == config->cursor_y_prop) {
		intel_crtc->cursor_y = value;
	} else if (prop == config->cursor_w_prop) {
		if (value != 0 && value != 64) {
			DRM_DEBUG_KMS("only 64x64 cursor sprites are supported\n");
			return -EINVAL;
		}
		intel_crtc->cursor_width = value;
	} else if (prop == config->cursor_h_prop) {
		if (value != 0 && value != 64) {
			DRM_DEBUG_KMS("only 64x64 cursor sprites are supported\n");
			return -EINVAL;
		}
		intel_crtc->cursor_height = value;
	} else
		return -ENOENT;

	s->restore_state = true;

	return 0;
}

static void crtc_compute_dirty(struct intel_atomic_state *s,
			       struct intel_crtc_state *state)
{
	struct drm_crtc *crtc = state->crtc;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);

	/* if no properties were specified nothing is dirty */
	if (!state->changed)
		return;

	/* fb/pan changed? */
	if (crtc->x != state->old.x ||
	    crtc->y != state->old.y ||
	    crtc->fb != state->old.fb) {
		/* FIXME do we need a full modeset sometimes? */
		state->fb_dirty = true;
	}

	/* enabled <-> disabled? */
	if (crtc->enabled != state->old.enabled) {
		state->mode_dirty = true;
		state->active_dirty = true;
	}

	/* mode changed? */
	if (crtc->enabled && state->old.enabled &&
	    !drm_mode_equal(&crtc->mode, &state->old.mode)) {
		state->mode_dirty = true;

		if (crtc->mode.hdisplay != state->old.mode.hdisplay ||
		    crtc->mode.vdisplay != state->old.mode.vdisplay)
			state->active_dirty = true;
	}

	/* connectors changed? */
	if (state->connectors_bitmask != state->old.connectors_bitmask ||
	    state->encoders_bitmask != state->old.encoders_bitmask)
		state->mode_dirty = true;

	/* cursor changed? */
	if (intel_crtc->cursor_handle != state->old.cursor_handle ||
	    intel_crtc->cursor_x != state->old.cursor_x ||
	    intel_crtc->cursor_y != state->old.cursor_y ||
	    intel_crtc->cursor_width != state->old.cursor_width ||
	    intel_crtc->cursor_height != state->old.cursor_height)
		state->cursor_dirty = true;

	if (state->fb_dirty || state->mode_dirty || state->cursor_dirty)
		s->dirty = true;
}

static void plane_compute_dirty(struct intel_atomic_state *s,
				struct intel_plane_state *state)
{
	struct drm_plane *plane = state->plane;

	/* if no properties were specified nothing is dirty */
	if (!state->changed)
		return;

	if (plane->src_x != state->old.src_x ||
	    plane->src_y != state->old.src_y ||
	    plane->src_w != state->old.src_w ||
	    plane->src_h != state->old.src_h ||
	    plane->crtc_x != state->old.crtc_x ||
	    plane->crtc_y != state->old.crtc_y ||
	    plane->crtc_w != state->old.crtc_w ||
	    plane->crtc_h != state->old.crtc_h ||
	    plane->crtc != state->old.crtc ||
	    plane->fb != state->old.fb)
		state->dirty = true;

	if (state->dirty)
		s->dirty = true;
}

static struct intel_plane_state *get_plane_state(const struct drm_device *dev,
						 struct intel_atomic_state *state,
						 const struct drm_plane *plane)
{
	int i;

	for (i = 0; i < dev->mode_config.num_plane; i++)
		if (plane == state->plane[i].plane)
			return &state->plane[i];

	return NULL;
}

static struct intel_crtc_state *get_crtc_state(const struct drm_device *dev,
					       struct intel_atomic_state *state,
					       const struct drm_crtc *crtc)
{
	int i;

	for (i = 0; i < dev->mode_config.num_crtc; i++)
		if (crtc == state->crtc[i].crtc)
			return &state->crtc[i];

	return NULL;
}

static void crtc_prepare(struct intel_crtc_state *st,
			 struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct intel_encoder *intel_encoder;

	dev_priv->display.crtc_disable(&intel_crtc->base);

	/* FIXME need to check where this stuff is used really */
	list_for_each_entry(intel_encoder, &dev->mode_config.encoder_list, base.head) {
		if (intel_encoder->base.crtc == crtc)
			intel_encoder->connectors_active = false;
	}
}

static int crtc_set_base(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;

	return dev_priv->display.update_plane(crtc, crtc->fb, crtc->x, crtc->y);
}

static int crtc_mode_set(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_encoder *encoder;
	int pipe = to_intel_crtc(crtc)->pipe;
	int ret;

	if (!crtc->enabled) {
		dev_priv->display.off(crtc);
		return 0;
	}

	drm_vblank_pre_modeset(dev, pipe);

	ret = dev_priv->display.crtc_mode_set(crtc, &crtc->mode, &crtc->hwmode,
					      crtc->x, crtc->y, crtc->fb);
	if (!ret)
		ret = dev_priv->display.update_plane(crtc, crtc->fb, crtc->x, crtc->y);

	if (!ret) {
		intel_update_watermarks(dev);

		if (HAS_PCH_SPLIT(dev))
			intel_update_linetime_watermarks(dev, pipe, &crtc->hwmode);
	}

	drm_vblank_post_modeset(dev, pipe);

	if (ret)
		return ret;

	list_for_each_entry(encoder, &dev->mode_config.encoder_list, head) {
		const struct drm_encoder_helper_funcs *encoder_funcs = encoder->helper_private;

		if (encoder->crtc != crtc)
			continue;

		encoder_funcs->mode_set(encoder, &crtc->mode, &crtc->hwmode);
	}

	return 0;
}

static void crtc_commit(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct intel_encoder *intel_encoder;

	if (!crtc->enabled)
		return;

	dev_priv->display.crtc_enable(&intel_crtc->base);

	/* FIXME need to check where this stuff is used really */
	list_for_each_entry(intel_encoder, &dev->mode_config.encoder_list, base.head) {
		if (intel_encoder->base.crtc == crtc)
			intel_encoder->connectors_active = true;
	}
}

static void unpin_cursors(struct drm_device *dev,
			  struct intel_atomic_state *s)
{
	int i;

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];
		struct drm_crtc *crtc = st->crtc;
		struct intel_crtc *intel_crtc = to_intel_crtc(crtc);

		if (!st->cursor_pinned)
			continue;

		intel_crtc_cursor_bo_unref(crtc, intel_crtc->cursor_bo);

		st->cursor_pinned = false;
	}
}

static int pin_cursors(struct drm_device *dev,
			struct intel_atomic_state *s)
{
	int i, ret;

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];
		struct drm_crtc *crtc = st->crtc;
		struct intel_crtc *intel_crtc = to_intel_crtc(crtc);

		if (!st->cursor_dirty)
			continue;

		ret = intel_crtc_cursor_prepare(crtc, s->file,
						intel_crtc->cursor_handle,
						intel_crtc->cursor_width,
						intel_crtc->cursor_height,
						&intel_crtc->cursor_bo,
						&intel_crtc->cursor_addr);
		if (ret)
			goto unpin;

		if (intel_crtc->cursor_bo)
			st->cursor_pinned = true;
	}

	return 0;

unpin:
	unpin_cursors(dev, s);

	return ret;
}

static void unpin_old_cursors(struct drm_device *dev,
			      struct intel_atomic_state *s)
{
	int i;

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];
		struct drm_crtc *crtc = st->crtc;

		if (!st->cursor_dirty)
			continue;

		if (!st->old.cursor_bo)
			continue;

		intel_crtc_cursor_bo_unref(crtc, st->old.cursor_bo);
	}
}

static void unpin_fbs(struct drm_device *dev,
		      struct intel_atomic_state *s)
{
	int i;

	for (i = dev->mode_config.num_plane - 1; i >= 0; i--) {
		struct intel_plane_state *st = &s->plane[i];
		struct drm_plane *plane = st->plane;
		struct drm_i915_gem_object *obj;

		if (!st->pinned)
			continue;

		obj = to_intel_framebuffer(plane->fb)->obj;

		mutex_lock(&dev->struct_mutex);
		intel_unpin_fb_obj(obj);
		mutex_unlock(&dev->struct_mutex);

		st->pinned = false;
	}

	for (i = dev->mode_config.num_crtc - 1; i >= 0; i--) {
		struct intel_crtc_state *st = &s->crtc[i];
		struct drm_crtc *crtc = st->crtc;
		struct drm_i915_gem_object *obj;

		if (!st->pinned)
			continue;

		obj = to_intel_framebuffer(crtc->fb)->obj;

		mutex_lock(&dev->struct_mutex);
		intel_unpin_fb_obj(obj);
		mutex_unlock(&dev->struct_mutex);

		st->pinned = false;
	}
}

extern unsigned int drm_async_gpu;

static int pin_fbs(struct drm_device *dev,
		   struct intel_atomic_state *s)
{
	int i, ret;
	bool nonblock = drm_async_gpu && (s->flags & DRM_MODE_ATOMIC_NONBLOCK);

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];
		struct drm_crtc *crtc = st->crtc;
		struct drm_i915_gem_object *obj;

		if (!st->fb_dirty)
			continue;

		if (!crtc->fb)
			continue;

		obj = to_intel_framebuffer(crtc->fb)->obj;

		mutex_lock(&dev->struct_mutex);
		ret = intel_pin_and_fence_fb_obj(dev, obj, nonblock ? obj->ring : NULL);
		mutex_unlock(&dev->struct_mutex);

		if (ret)
			goto unpin;

		st->pinned = true;
	}

	for (i = 0; i < dev->mode_config.num_plane; i++) {
		struct intel_plane_state *st = &s->plane[i];
		struct drm_plane *plane = st->plane;
		struct drm_i915_gem_object *obj;

		if (!st->dirty)
			continue;

		if (!plane->fb)
			continue;

		obj = to_intel_framebuffer(plane->fb)->obj;

		mutex_lock(&dev->struct_mutex);
		ret = intel_pin_and_fence_fb_obj(dev, obj, nonblock ? obj->ring : NULL);
		mutex_unlock(&dev->struct_mutex);

		if (ret)
			goto unpin;

		st->pinned = true;
	}

	return 0;

 unpin:
	unpin_fbs(dev, s);

	return ret;
}

static void unpin_old_fbs(struct drm_device *dev,
			  struct intel_atomic_state *s)
{
	int i;

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];
		struct drm_i915_gem_object *obj;

		if (!st->fb_dirty)
			continue;

		if (!st->old.fb)
			continue;

		obj = to_intel_framebuffer(st->old.fb)->obj;

		mutex_lock(&dev->struct_mutex);
		intel_unpin_fb_obj(obj);
		mutex_unlock(&dev->struct_mutex);
	}

	for (i = 0; i < dev->mode_config.num_plane; i++) {
		struct intel_plane_state *st = &s->plane[i];
		struct drm_i915_gem_object *obj;

		if (!st->dirty)
			continue;

		if (!st->old.fb)
			continue;

		obj = to_intel_framebuffer(st->old.fb)->obj;

		mutex_lock(&dev->struct_mutex);
		intel_unpin_fb_obj(obj);
		mutex_unlock(&dev->struct_mutex);
	}
}

static void update_plane_obj(struct drm_device *dev,
			     struct intel_atomic_state *s)
{
	int i;

	for (i = 0; i < dev->mode_config.num_plane; i++) {
		struct intel_plane_state *st = &s->plane[i];
		struct drm_plane *plane = st->plane;
		struct intel_plane *intel_plane = to_intel_plane(plane);

		if (!st->dirty)
			continue;

		if (plane->fb)
			intel_plane->obj = to_intel_framebuffer(plane->fb)->obj;
		else
			intel_plane->obj = NULL;
	}
}

static struct drm_pending_atomic_event *alloc_event(struct drm_device *dev,
						    struct drm_file *file_priv,
						    uint64_t user_data)
{
	struct drm_pending_atomic_event *e;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);

	if (file_priv->event_space < sizeof e->event) {
		spin_unlock_irqrestore(&dev->event_lock, flags);
		return ERR_PTR(-ENOSPC);
	}

	file_priv->event_space -= sizeof e->event;
	spin_unlock_irqrestore(&dev->event_lock, flags);

	e = kzalloc(sizeof *e, GFP_KERNEL);
	if (!e) {
		spin_lock_irqsave(&dev->event_lock, flags);
		file_priv->event_space += sizeof e->event;
		spin_unlock_irqrestore(&dev->event_lock, flags);

		return ERR_PTR(-ENOMEM);
	}

	e->event.base.type = DRM_EVENT_ATOMIC_COMPLETE;
	e->event.base.length = sizeof e->event;
	e->event.user_data = user_data;
	e->base.event = &e->event.base;
	e->base.file_priv = file_priv;
	e->base.destroy = (void (*) (struct drm_pending_event *)) kfree;

	return e;
}

static void free_event(struct drm_pending_atomic_event *e)
{
	e->base.file_priv->event_space += sizeof e->event;
	kfree(e);
}

void intel_atomic_free_events(struct drm_device *dev, struct drm_file *file)
{
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct intel_flip *intel_flip, *next;

	spin_lock_irq(&dev->event_lock);

	list_for_each_entry_safe(intel_flip, next, &file_priv->pending_flips, pending_head) {
		free_event(intel_flip->event);
		intel_flip->event = NULL;
		list_del_init(&intel_flip->pending_head);
	}

	spin_unlock_irq(&dev->event_lock);
}

static void queue_event(struct drm_device *dev, struct drm_crtc *crtc,
			struct drm_pending_atomic_event *e)
{
	struct timeval tvbl;

	if (crtc) {
		int pipe = to_intel_crtc(crtc)->pipe;

		/* FIXME this is wrong for flips that are completed not at vblank */
		e->event.sequence = drm_vblank_count_and_time(dev, pipe, &tvbl);
		e->event.tv_sec = tvbl.tv_sec;
		e->event.tv_usec = tvbl.tv_usec;
	} else {
		e->event.sequence = 0;
		e->event.tv_sec = 0;
		e->event.tv_usec = 0;
	}

	list_add_tail(&e->base.link, &e->base.file_priv->event_list);
	wake_up_interruptible(&e->base.file_priv->event_wait);
}

static void queue_remaining_events(struct drm_device *dev, struct intel_atomic_state *s)
{
	int i;

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];

		if (st->event) {
			if (st->old.fb)
				st->event->event.old_fb_id = st->old.fb->base.id;

			spin_lock_irq(&dev->event_lock);
			queue_event(dev, st->crtc, st->event);
			spin_unlock_irq(&dev->event_lock);

			st->event = NULL;
		}
	}

	for (i = 0; i < dev->mode_config.num_plane; i++) {
		struct intel_plane_state *st = &s->plane[i];
		struct drm_crtc *crtc;

		if (!st->event)
			continue;

		crtc = st->plane->crtc;
		if (!crtc)
			crtc = st->old.crtc;

		if (st->old.fb)
			st->event->event.old_fb_id = st->old.fb->base.id;

		spin_lock_irq(&dev->event_lock);
		queue_event(dev, crtc, st->event);
		spin_unlock_irq(&dev->event_lock);

		st->event = NULL;
	}
}

static int apply_config(struct drm_device *dev,
			struct intel_atomic_state *s)
{
	int i, ret;

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];

		mutex_lock(&dev->struct_mutex);

		if (st->mode_dirty) {
			/* wait for pending MI_WAIT_FOR_EVENTs */
			if (st->old.fb)
				intel_finish_fb(st->old.fb);
		}

		mutex_unlock(&dev->struct_mutex);

		if (!st->mode_dirty)
			continue;

		crtc_prepare(st, st->crtc);
	}

	intel_modeset_commit_output_state(dev);

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];

		if (!st->mode_dirty)
			continue;

		ret = crtc_mode_set(st->crtc);
		if (ret)
			return ret;
	}

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];
		struct drm_crtc *crtc = st->crtc;
		int j;

		if (st->mode_dirty)
			crtc_commit(crtc);
		else if (st->fb_dirty) {
			ret = crtc_set_base(st->crtc);
			if (ret)
				return ret;
		}

		/*
		 * FIXME these should happen alongside the primary plane setup
		 * which occurs inside the crtc_enable() hook.
		 */

		if (st->cursor_dirty) {
			struct intel_crtc *intel_crtc = to_intel_crtc(crtc);

			intel_crtc_cursor_commit(crtc,
						 intel_crtc->cursor_handle,
						 intel_crtc->cursor_width,
						 intel_crtc->cursor_height,
						 intel_crtc->cursor_bo,
						 intel_crtc->cursor_addr);
		}

		if (!st->primary_disabled)
			intel_enable_primary(crtc);

		for (j = 0; j < dev->mode_config.num_plane; j++) {
			struct intel_plane_state *pst = &s->plane[j];
			struct drm_plane *plane = pst->plane;
			struct intel_plane *intel_plane = to_intel_plane(plane);

			if (!pst->dirty)
				continue;

			if (pst->coords.visible && plane->crtc == crtc)
				intel_plane->update_plane(plane, plane->fb, &pst->coords);
			else if (!pst->coords.visible && pst->old.crtc == crtc)
				intel_plane->disable_plane(plane);
		}

		if (st->primary_disabled)
			intel_disable_primary(crtc);
	}

	/* don't restore the old state in end() */
	s->dirty = false;
	s->restore_state = false;

	return 0;
}

static void restore_state(struct drm_device *dev,
			  struct intel_atomic_state *s)
{
	int i;
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	struct drm_crtc *crtc;
	struct drm_plane *plane;

	i = 0;
	list_for_each_entry(connector, &dev->mode_config.connector_list, head)
		*connector = s->saved_connectors[i++];
	i = 0;
	list_for_each_entry(encoder, &dev->mode_config.encoder_list, head)
		*encoder = s->saved_encoders[i++];
	i = 0;
	list_for_each_entry(crtc, &dev->mode_config.crtc_list, head) {
		struct intel_crtc *intel_crtc = to_intel_crtc(crtc);

		intel_crtc->base = s->saved_crtcs[i].base;
		intel_crtc->cursor_bo = s->saved_crtcs[i].cursor_bo;
		intel_crtc->cursor_addr = s->saved_crtcs[i].cursor_addr;
		intel_crtc->cursor_handle = s->saved_crtcs[i].cursor_handle;
		intel_crtc->cursor_x = s->saved_crtcs[i].cursor_x;
		intel_crtc->cursor_y = s->saved_crtcs[i].cursor_y;
		intel_crtc->cursor_width = s->saved_crtcs[i].cursor_width;
		intel_crtc->cursor_height = s->saved_crtcs[i].cursor_height;
		intel_crtc->cursor_visible = s->saved_crtcs[i].cursor_visible;
		intel_crtc->primary_disabled = s->saved_crtcs[i].primary_disabled;

		i++;
	}
	i = 0;
	list_for_each_entry(plane, &dev->mode_config.plane_list, head)
		*plane = s->saved_planes[i++];

	/* must restore the new_crtc and new_encoder pointers as well */
	intel_modeset_update_staged_output_state(dev);

	/* was the hardware state clobbered? */
	if (s->restore_hw)
		apply_config(dev, s);
}

static int intel_atomic_set(struct drm_device *dev, void *state,
			    struct drm_mode_object *obj,
			    struct drm_property *prop,
			    uint64_t value, void *blob_data)
{
	struct intel_atomic_state *s = state;
	int ret = -EINVAL;

	switch (obj->type) {
	case DRM_MODE_OBJECT_PLANE:
		ret = plane_set(s, get_plane_state(dev, s, obj_to_plane(obj)), prop, value);
		break;
	case DRM_MODE_OBJECT_CRTC:
		ret = crtc_set(s, get_crtc_state(dev, s, obj_to_crtc(obj)), prop, value, blob_data);
		break;
	default:
		break;
	}

	kfree(blob_data);

	return ret;
}

int intel_check_plane(const struct drm_plane *plane,
		      const struct drm_crtc *crtc,
		      const struct drm_framebuffer *fb,
		      struct intel_plane_coords *st);

static void dirty_planes(const struct drm_device *dev,
			 struct intel_atomic_state *state,
			 const struct drm_crtc *crtc)
{
	int i;

	for (i = 0; i < dev->mode_config.num_plane; i++) {
		struct intel_plane_state *s = &state->plane[i];

		if (s->plane->crtc == crtc)
			s->dirty = true;
	}
}

static int check_crtc(struct intel_crtc_state *s)
{
	struct drm_crtc *crtc = s->crtc;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	struct drm_encoder *encoder;
	struct drm_framebuffer *fb = crtc->fb;
	int ret;

	/* must have a fb and connectors if we have a mode, and vice versa */
	if (crtc->enabled) {
		if (!fb)
			return -EINVAL;
		if (!intel_crtc_in_use(intel_crtc))
			return -EINVAL;
	} else {
		if (fb)
			return -EINVAL;
		if (intel_crtc_in_use(intel_crtc))
			return -EINVAL;
	}

	if (crtc->enabled) {
		if (crtc->mode.hdisplay > fb->width ||
		    crtc->mode.vdisplay > fb->height ||
		    crtc->x > fb->width - crtc->mode.hdisplay ||
		    crtc->y > fb->height - crtc->mode.vdisplay)
			return -ENOSPC;
	}

	if (fb) {
		/* FIXME refactor and check */
		switch (fb->pixel_format) {
		case DRM_FORMAT_C8:
		case DRM_FORMAT_RGB565:
		case DRM_FORMAT_XRGB8888:
		case DRM_FORMAT_ARGB8888:
		case DRM_FORMAT_XBGR8888:
		case DRM_FORMAT_ABGR8888:
		case DRM_FORMAT_XRGB2101010:
		case DRM_FORMAT_ARGB2101010:
		case DRM_FORMAT_XBGR2101010:
		case DRM_FORMAT_ABGR2101010:
		case DRM_FORMAT_XRGB1555:
		case DRM_FORMAT_ARGB1555:
			break;
		default:
			return -EINVAL;
		}
	}

	if (intel_crtc->cursor_handle &&
	    (intel_crtc->cursor_width != 64 ||
	     intel_crtc->cursor_height != 64)) {
		return -EINVAL;
	}

	if (!crtc->enabled || !s->mode_dirty)
		return 0;

	crtc->hwmode = crtc->mode;

	list_for_each_entry(encoder, &dev->mode_config.encoder_list, head) {
		const struct drm_encoder_helper_funcs *encoder_funcs = encoder->helper_private;
		struct intel_encoder *intel_encoder = to_intel_encoder(encoder);

		if (intel_encoder->new_crtc != intel_crtc)
			continue;

		if (!encoder_funcs->mode_fixup(encoder, &crtc->mode, &crtc->hwmode))
			return -EINVAL;
	}

	if (!intel_crtc_mode_fixup(crtc, &crtc->mode, &crtc->hwmode))
		return -EINVAL;

	ret = intel_check_clock(crtc, &crtc->hwmode);
	if (ret)
		return ret;

	return 0;
}

static void update_primary_visibility(struct drm_device *dev,
				      struct intel_atomic_state *s,
				      const struct drm_crtc *crtc,
				      const struct drm_plane *plane,
				      const struct intel_plane_coords *coords)
{
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	bool primary_disabled =
		coords->visible &&
		coords->crtc_x == 0 &&
		coords->crtc_y == 0 &&
		coords->crtc_w == crtc->mode.hdisplay &&
		coords->crtc_h == crtc->mode.vdisplay;

	if (primary_disabled != intel_crtc->primary_disabled) {
		struct intel_crtc_state *st = get_crtc_state(dev, s, crtc);
		st->fb_dirty = true;
		st->primary_disabled = primary_disabled;
		s->dirty = true;
	}
}

static int intel_atomic_check(struct drm_device *dev, void *state)
{
	struct intel_atomic_state *s = state;
	int ret;
	int i;

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];

		crtc_compute_dirty(s, st);
	}

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_plane_state *st = &s->plane[i];

		plane_compute_dirty(s, st);
	}

	if (!s->dirty)
		return 0;

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];

		if (!st->fb_dirty && !st->mode_dirty && !st->cursor_dirty)
			continue;

		if (st->mode_dirty && s->flags & DRM_MODE_ATOMIC_NONBLOCK)
			return -EAGAIN;

		ret = check_crtc(st);
		if (ret)
			return ret;

		/*
		 * Mark all planes on this CRTC as dirty if the active video
		 * area changed so that the planes will get reclipped correctly.
		 *
		 * Also any modesetting will disable+enable the pipe, so the
		 * plane needs to be re-enabled afterwards too.
		 * TODO: there's no need to redo the clipping in such cases
		 * if the computed values were cached, the could be commited
		 * directly.
		 */
		if (st->active_dirty || st->mode_dirty)
			dirty_planes(dev, s, st->crtc);
	}

	/* check for conflicts in encoder/connector assignment */
	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];
		int j;

		for (j = i + 1; j < dev->mode_config.num_crtc; j++) {
			struct intel_crtc_state *st2 = &s->crtc[j];

			if (st->connectors_bitmask & st2->connectors_bitmask)
				return -EINVAL;

			if (st->encoders_bitmask & st2->encoders_bitmask)
				return -EINVAL;
		}
	}

	for (i = 0; i < dev->mode_config.num_plane; i++) {
		struct intel_plane_state *st = &s->plane[i];
		const struct drm_plane *plane = st->plane;

		if (!st->dirty)
			continue;

		st->coords.crtc_x = plane->crtc_x;
		st->coords.crtc_y = plane->crtc_y;
		st->coords.crtc_w = plane->crtc_w;
		st->coords.crtc_h = plane->crtc_h;

		st->coords.src_x = plane->src_x;
		st->coords.src_y = plane->src_y;
		st->coords.src_w = plane->src_w;
		st->coords.src_h = plane->src_h;

		ret = intel_check_plane(plane, plane->crtc, plane->fb, &st->coords);
		if (ret)
			return ret;

		/* FIXME doesn't correctly handle cases where plane moves between crtcs */
		if (plane->crtc)
			update_primary_visibility(dev, s, plane->crtc, plane, &st->coords);
		else if (st->old.crtc)
			update_primary_visibility(dev, s, st->old.crtc, plane, &st->coords);
	}

	return 0;
}

static void update_props(struct drm_device *dev,
			 struct intel_atomic_state *s)
{
	int i;

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];

		if (!st->fb_dirty && !st->mode_dirty)
			continue;

		intel_crtc_update_properties(st->crtc);
	}

	for (i = 0; i < dev->mode_config.num_plane; i++) {
		struct intel_plane_state *st = &s->plane[i];

		if (!st->dirty)
			continue;

		drm_plane_update_properties(st->plane);
	}
}

/*
 * FIXME
 * Perhaps atomic modeset shouldn't actually change the DPMS state,
 * unless explicitly asked to do so. That's the way we treat everything
 * else, so it makes sense. Although the dpms property is already a bit
 * special in the legacy codepaths, so maybe we should follow the same
 * pattern. Ie. a modeset forces DPMS to on (which is what we do here).
 */
static void update_connector_dpms(struct drm_device *dev, struct drm_crtc *crtc)
{
	struct drm_connector *connector;

	list_for_each_entry(connector, &dev->mode_config.connector_list, head) {
		int dpms = connector->dpms;

		if (connector->encoder && connector->encoder->crtc == crtc)
			dpms = DRM_MODE_DPMS_ON;
		else if (!connector->encoder || !connector->encoder->crtc)
			dpms = DRM_MODE_DPMS_OFF;

		if (connector->dpms == dpms)
			continue;

		connector->dpms = dpms;
		drm_connector_property_set_value(connector,
						 dev->mode_config.dpms_property,
						 dpms);
	}
}

static void update_crtc(struct drm_device *dev,
			struct intel_atomic_state *s)
{
	int i;

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];
		struct drm_crtc *crtc = st->crtc;

		if (st->fb_dirty && !st->mode_dirty) {
			mutex_lock(&dev->struct_mutex);
			intel_update_fbc(dev);
			mutex_unlock(&dev->struct_mutex);
		}

		if (st->mode_dirty) {
			drm_calc_timestamping_constants(crtc);
			intel_crtc_update_sarea(crtc, crtc->enabled);
			update_connector_dpms(dev, crtc);
		}

		if (st->fb_dirty)
			intel_crtc_update_sarea_pos(crtc, crtc->x, crtc->y);
	}
}

static void atomic_pipe_commit(struct drm_device *dev,
			       struct intel_atomic_state *state,
			       int pipe);

static void apply_nonblocking(struct drm_device *dev, struct intel_atomic_state *s)
{
	struct intel_crtc *intel_crtc;

	list_for_each_entry(intel_crtc, &dev->mode_config.crtc_list, base.head)
		atomic_pipe_commit(dev, s, intel_crtc->pipe);

	/* don't restore the old state in end() */
	s->dirty = false;
	s->restore_state = false;
}

static int alloc_flip_data(struct drm_device *dev, struct intel_atomic_state *s)
{
	int i;

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];

		if (st->changed && s->flags & DRM_MODE_ATOMIC_EVENT) {
			struct drm_pending_atomic_event *e;

			e = alloc_event(dev, s->file, s->user_data);
			if (IS_ERR(e))
				return PTR_ERR(e);

			e->event.obj_id = st->crtc->base.id;

			st->event = e;
		}

		if (!st->fb_dirty && !st->mode_dirty && !st->cursor_dirty)
			continue;

		st->flip = kzalloc(sizeof *st->flip, GFP_KERNEL);
		if (!st->flip)
			return -ENOMEM;
	}


	for (i = 0; i < dev->mode_config.num_plane; i++) {
		struct intel_plane_state *st = &s->plane[i];

		if (st->changed && s->flags & DRM_MODE_ATOMIC_EVENT) {
			struct drm_pending_atomic_event *e;

			e = alloc_event(dev, s->file, s->user_data);
			if (IS_ERR(e))
				return PTR_ERR(e);

			e->event.obj_id = st->plane->base.id;

			st->event = e;
		}

		if (!st->dirty)
			continue;

		st->flip = kzalloc(sizeof *st->flip, GFP_KERNEL);
		if (!st->flip)
			return -ENOMEM;
	}

	return 0;
}

static void free_flip_data(struct drm_device *dev, struct intel_atomic_state *s)
{
	int i;

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &s->crtc[i];

		if (st->event) {
			spin_lock_irq(&dev->event_lock);
			free_event(st->event);
			spin_unlock_irq(&dev->event_lock);
			st->event = NULL;
		}

		kfree(st->flip);
		st->flip = NULL;
	}

	for (i = 0; i < dev->mode_config.num_plane; i++) {
		struct intel_plane_state *st = &s->plane[i];

		if (st->event) {
			spin_lock_irq(&dev->event_lock);
			free_event(st->event);
			spin_unlock_irq(&dev->event_lock);
			st->event = NULL;
		}

		kfree(st->flip);
		st->flip = NULL;
	}
}

static int intel_atomic_commit(struct drm_device *dev, void *state)
{
	struct intel_atomic_state *s = state;
	int ret;

	ret = alloc_flip_data(dev, s);
	if (ret)
		return ret;

	if (!s->dirty) {
		queue_remaining_events(dev, s);
		return 0;
	}

	ret = pin_fbs(dev, s);
	if (ret)
		return ret;

	ret = pin_cursors(dev, s);
	if (ret)
		return ret;

	/* try to apply in a non blocking manner */
	if (s->flags & DRM_MODE_ATOMIC_NONBLOCK) {
		apply_nonblocking(dev, s);
	} else {
		/* apply in a blocking manner */
		ret = apply_config(dev, s);
		if (ret) {
			unpin_cursors(dev, s);
			unpin_fbs(dev, s);
			s->restore_hw = true;
			return ret;
		}

		unpin_old_cursors(dev, s);
		unpin_old_fbs(dev, s);
	}

	/*
	 * Either we took the blocking code path, or perhaps the state of
	 * some objects didn't actually change? Nonetheless the user wanted
	 * events for all objects he touched, so queue up any events that
	 * are still pending.
	 *
	 * FIXME this needs more work. If the previous flip is still pending
	 * we shouldn't send this event until that flip completes.
	 */
	queue_remaining_events(dev, s);

	update_plane_obj(dev, s);

	update_crtc(dev, s);

	update_props(dev, s);


	return 0;
}

static void intel_atomic_end(struct drm_device *dev, void *state)
{
	struct intel_atomic_state *s = state;

	/* don't send events when restoring old state */
	free_flip_data(dev, state);

	/* restore the state of all objects */
	if (s->restore_state)
		restore_state(dev, state);

	kfree(state);
}

static const struct drm_atomic_funcs intel_atomic_funcs = {
	.begin = intel_atomic_begin,
	.set = intel_atomic_set,
	.check = intel_atomic_check,
	.commit = intel_atomic_commit,
	.end = intel_atomic_end,
};

static void intel_flip_init(struct drm_device *dev);
static void intel_flip_fini(struct drm_device *dev);

int intel_atomic_init(struct drm_device *dev)
{
	struct drm_crtc *crtc;
	struct drm_plane *plane;
	int ret;

	ret = drm_mode_create_properties(dev);
	if (ret)
		goto out;

	list_for_each_entry(plane, &dev->mode_config.plane_list, head) {
		drm_plane_attach_properties(plane);
		drm_plane_update_properties(plane);
	}

	list_for_each_entry(crtc, &dev->mode_config.crtc_list, head) {
		intel_crtc_attach_properties(crtc);

		ret = drm_crtc_create_blobs(crtc);
		if (ret)
			goto destroy_props;

		intel_crtc_update_properties(crtc);
	}

	dev->driver->atomic_funcs = &intel_atomic_funcs;

	intel_flip_init(dev);

	return 0;

 destroy_props:
	drm_mode_destroy_properties(dev);
 out:

	return ret;
}

void intel_atomic_fini(struct drm_device *dev)
{
	struct drm_crtc *crtc;

	intel_flip_fini(dev);

	list_for_each_entry(crtc, &dev->mode_config.crtc_list, head) {
		drm_crtc_destroy_blobs(crtc);
	}

	drm_mode_destroy_properties(dev);
}

enum {
	/* somwehat arbitrary value */
	INTEL_VBL_CNT_TIMEOUT = 5,
};

static void intel_flip_complete(struct drm_flip *flip)
{
	struct intel_flip *intel_flip =
		container_of(flip, struct intel_flip, base);
	struct drm_device *dev = intel_flip->crtc->dev;
	struct drm_crtc *crtc = intel_flip->crtc;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	int pipe = intel_crtc->pipe;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);

	if (intel_flip->event) {
		list_del_init(&intel_flip->pending_head);
		intel_flip->event->event.old_fb_id = intel_flip->old_fb_id;
		queue_event(dev, crtc, intel_flip->event);
	}

	spin_unlock_irqrestore(&dev->event_lock, flags);

	if (intel_flip->vblank_ref)
		drm_vblank_put(dev, pipe);
}

static void intel_flip_finish(struct drm_flip *flip)
{
	struct intel_flip *intel_flip =
		container_of(flip, struct intel_flip, base);
	struct drm_device *dev = intel_flip->crtc->dev;

	if (intel_flip->old_bo) {
		mutex_lock(&dev->struct_mutex);

		intel_unpin_fb_obj(intel_flip->old_bo);

		drm_gem_object_unreference(&intel_flip->old_bo->base);

		mutex_unlock(&dev->struct_mutex);
	}

	if (intel_flip->old_cursor_bo)
		intel_crtc_cursor_bo_unref(intel_flip->crtc, intel_flip->old_cursor_bo);
}

static void intel_flip_cleanup(struct drm_flip *flip)
{
	struct intel_flip *intel_flip =
		container_of(flip, struct intel_flip, base);

	kfree(intel_flip);
}

static void intel_flip_driver_flush(struct drm_flip_driver *driver)
{
	struct drm_i915_private *dev_priv =
		container_of(driver, struct drm_i915_private, flip_driver);

	/* Flush posted writes */
	I915_READ(PIPEDSL(PIPE_A));
}

static bool intel_have_new_frmcount(struct drm_device *dev)
{
	return IS_G4X(dev) || INTEL_INFO(dev)->gen >= 5;
}

static u32 get_vbl_count(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	int pipe = intel_crtc->pipe;

	if (intel_have_new_frmcount(dev)) {
		return I915_READ(PIPE_FRMCOUNT_GM45(pipe));
	} else  {
		u32 high, low1, low2, dsl;
		unsigned int timeout = 0;

		/*
		 * FIXME check where the frame counter increments, and if
		 * it happens in the middle of some line, take appropriate
		 * measures to get a sensible reading.
		 */

		/* All reads must be satisfied during the same frame */
		do {
			low1 = I915_READ(PIPEFRAMEPIXEL(pipe)) >> PIPE_FRAME_LOW_SHIFT;
			high = I915_READ(PIPEFRAME(pipe)) << 8;
			dsl = I915_READ(PIPEDSL(pipe));
			low2 = I915_READ(PIPEFRAMEPIXEL(pipe)) >> PIPE_FRAME_LOW_SHIFT;
		} while (low1 != low2 && timeout++ < INTEL_VBL_CNT_TIMEOUT);

		if (timeout >= INTEL_VBL_CNT_TIMEOUT)
			dev_warn(dev->dev,
				 "Timed out while determining VBL count for pipe %d\n", pipe);

		return ((high | low2) +
			((dsl >= crtc->hwmode.crtc_vdisplay) &&
			 (dsl < crtc->hwmode.crtc_vtotal - 1))) & 0xffffff;
	}
}

static unsigned int usecs_to_scanlines(struct drm_crtc *crtc,
				       unsigned int usecs)
{
	/* paranoia */
	if (!crtc->hwmode.crtc_htotal)
		return 1;

	return DIV_ROUND_UP(usecs * crtc->hwmode.clock,
			    1000 * crtc->hwmode.crtc_htotal);
}

static void intel_pipe_vblank_evade(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	int pipe = intel_crtc->pipe;
	/* FIXME needs to be calibrated sensibly */
	u32 min = crtc->hwmode.crtc_vdisplay - usecs_to_scanlines(crtc, 50);
	u32 max = crtc->hwmode.crtc_vdisplay - 1;
	long timeout = msecs_to_jiffies(3);
	u32 val;

	bool vblank_ref = drm_vblank_get(dev, pipe) == 0;

	intel_crtc->vbl_received = false;

	val = I915_READ(PIPEDSL(pipe));

	while (val >= min && val <= max && timeout > 0) {
		local_irq_enable();

		timeout = wait_event_timeout(intel_crtc->vbl_wait,
					     intel_crtc->vbl_received,
					     timeout);

		local_irq_disable();

		intel_crtc->vbl_received = false;

		val = I915_READ(PIPEDSL(pipe));
	}

	if (vblank_ref)
		drm_vblank_put(dev, pipe);

	if (val >= min && val <= max)
		dev_warn(dev->dev,
			 "Page flipping close to vblank start (DSL=%u, VBL=%u)\n",
			 val, crtc->hwmode.crtc_vdisplay);
}

static bool vbl_count_after_eq_new(u32 a, u32 b)
{
	return !((a - b) & 0x80000000);
}

static bool vbl_count_after_eq(u32 a, u32 b)
{
	return !((a - b) & 0x800000);
}

static bool intel_vbl_check(struct drm_flip *pending_flip, u32 vbl_count)
{
	struct intel_flip *old_intel_flip =
		container_of(pending_flip, struct intel_flip, base);
	struct drm_device *dev = old_intel_flip->crtc->dev;

	if (intel_have_new_frmcount(dev))
		return vbl_count_after_eq_new(vbl_count, old_intel_flip->vbl_count);
	else
		return vbl_count_after_eq(vbl_count, old_intel_flip->vbl_count);
}

static void intel_flip_prepare(struct drm_flip *flip)
{
	struct intel_flip *intel_flip =
		container_of(flip, struct intel_flip, base);

	if (intel_flip->plane) {
		struct drm_plane *plane = intel_flip->plane;
		struct intel_plane *intel_plane = to_intel_plane(plane);

		intel_plane->prepare(plane);
	}
}

#ifdef SURFLIVE_DEBUG
enum flip_action {
	_NEW,
	_FLIPPED,
	_NOT_FLIPPED,
	_MISSED_FLIPPED,
};

static void trace_flip(struct intel_flip *intel_flip, enum flip_action action)
{
	struct drm_crtc *crtc = intel_flip->crtc;
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	int pipe = to_intel_crtc(crtc)->pipe;
	u32 surf;
	u32 surflive;
	u32 dsl;
	u32 iir;
	u32 vbl_count;

	if (intel_flip->plane) {
		surf = I915_READ(SPRSURF(pipe));
		surflive = I915_READ(SPRSURFLIVE(pipe));
	} else {
		surf = I915_READ(DSPSURF(pipe));
		surflive = I915_READ(DSPSURFLIVE(pipe));
	}
	dsl = I915_READ(PIPEDSL(pipe));
	iir = I915_READ(DEIIR);
	vbl_count = get_vbl_count(crtc);

	trace_i915_atomic_flip(intel_flip->plane != NULL, pipe, action,
			       intel_flip->commit_surf, intel_flip->commit_surflive,
			       surf, surflive, iir, intel_flip->commit_dsl, dsl,
			       intel_flip->vbl_count, vbl_count);
}
#endif

#ifdef SURFLIVE_DEBUG
static unsigned int missed_flips;
#endif

static bool intel_flip_flip(struct drm_flip *flip,
			    struct drm_flip *pending_flip)
{
	struct intel_flip *intel_flip = container_of(flip, struct intel_flip, base);
	struct drm_crtc *crtc = intel_flip->crtc;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	int pipe = intel_crtc->pipe;
	u32 vbl_count;
#ifdef SURFLIVE_DEBUG
	struct drm_i915_private *dev_priv = dev->dev_private;
#endif

	intel_flip->vblank_ref = drm_vblank_get(dev, pipe) == 0;

	vbl_count = get_vbl_count(crtc);

	/* arm all the double buffer registers */
	if (intel_flip->plane) {
		struct drm_plane *plane = intel_flip->plane;
		struct intel_plane *intel_plane = to_intel_plane(plane);

		intel_plane->commit(plane, &intel_flip->regs);

#ifdef SURFLIVE_DEBUG
		intel_flip->commit_dsl = I915_READ(PIPEDSL(pipe));
		intel_flip->commit_surf = I915_READ(SPRSURF(pipe));
		intel_flip->commit_surflive = I915_READ(SPRSURFLIVE(pipe));
		if (intel_flip->commit_surf != intel_flip->regs.surf)
			pr_err("SPRITE SURF MISMATCH\n");
#endif
	} else {
		struct drm_i915_private *dev_priv = dev->dev_private;

		dev_priv->display.commit_plane(crtc, &intel_flip->regs);

#ifdef SURFLIVE_DEBUG
		intel_flip->commit_dsl = I915_READ(PIPEDSL(pipe));
		intel_flip->commit_surf = I915_READ(DSPSURF(pipe));
		intel_flip->commit_surflive = I915_READ(DSPSURFLIVE(pipe));
		if (intel_flip->commit_surf != intel_flip->regs.surf)
			pr_err("PRIMARY PLANE SURF MISMATCH\n");
#endif
	}

	if (intel_flip->has_cursor)
		intel_crtc_cursor_commit(crtc,
					 intel_crtc->cursor_handle,
					 intel_crtc->cursor_width,
					 intel_crtc->cursor_height,
					 intel_crtc->cursor_bo,
					 intel_crtc->cursor_addr);

	/* This flip will happen on the next vblank */
	if (intel_have_new_frmcount(dev))
		intel_flip->vbl_count = vbl_count + 1;
	else
		intel_flip->vbl_count = (vbl_count + 1) & 0xffffff;

#ifdef SURFLIVE_DEBUG
	trace_flip(intel_flip, _NEW);
#endif

	if (pending_flip) {
		struct intel_flip *old_intel_flip =
			container_of(pending_flip, struct intel_flip, base);
		bool flipped = intel_vbl_check(pending_flip, vbl_count);

		if (!flipped) {
#ifdef SURFLIVE_DEBUG
			u32 surflive = I915_READ(old_intel_flip->plane ? SPRSURFLIVE(pipe) : DSPSURFLIVE(pipe));
			if (old_intel_flip->commit_surflive != surflive)
				trace_flip(old_intel_flip, _NOT_FLIPPED);
#endif
			swap(intel_flip->old_fb_id, old_intel_flip->old_fb_id);
			swap(intel_flip->old_bo, old_intel_flip->old_bo);
			swap(intel_flip->old_cursor_bo, old_intel_flip->old_cursor_bo);
		}
#ifdef SURFLIVE_DEBUG
		else {
			u32 surflive = I915_READ(old_intel_flip->plane ? SPRSURFLIVE(pipe) : DSPSURFLIVE(pipe));
			if (old_intel_flip->commit_surf != surflive) {
				trace_flip(old_intel_flip, _FLIPPED);
				missed_flips++;
				return false;
			}
			if (missed_flips)
				trace_flip(old_intel_flip, _MISSED_FLIPPED);
			missed_flips = 0;
		}
#endif

		return flipped;
	}

	return false;
}

static bool intel_flip_vblank(struct drm_flip *pending_flip)
{
	struct intel_flip *old_intel_flip =
		container_of(pending_flip, struct intel_flip, base);
	u32 vbl_count = get_vbl_count(old_intel_flip->crtc);

#ifdef SURFLIVE_DEBUG
	struct drm_i915_private *dev_priv = old_intel_flip->crtc->dev->dev_private;
	int pipe = to_intel_crtc(old_intel_flip->crtc)->pipe;
	bool flipped;
	flipped = intel_vbl_check(pending_flip, vbl_count);
	if (flipped) {
		u32 surflive = I915_READ(old_intel_flip->plane ? SPRSURFLIVE(pipe) : DSPSURFLIVE(pipe));
		if (old_intel_flip->commit_surf != surflive) {
			trace_flip(old_intel_flip, _FLIPPED);
			missed_flips++;
			return false;
		}
		if (missed_flips)
			trace_flip(old_intel_flip, _MISSED_FLIPPED);
		missed_flips = 0;
	}
	return flipped;
#else
	return intel_vbl_check(pending_flip, vbl_count);
#endif
}

static const struct drm_flip_helper_funcs intel_flip_funcs = {
	.prepare = intel_flip_prepare,
	.flip = intel_flip_flip,
	.vblank = intel_flip_vblank,
	.complete = intel_flip_complete,
	.finish = intel_flip_finish,
	.cleanup = intel_flip_cleanup,
};

static const struct drm_flip_driver_funcs intel_flip_driver_funcs = {
	.flush = intel_flip_driver_flush,
};

static void intel_atomic_process_flips_work(struct work_struct *work);

static void intel_flip_init(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc;
	struct intel_plane *intel_plane;

	drm_flip_driver_init(&dev_priv->flip_driver, &intel_flip_driver_funcs);

	list_for_each_entry(intel_crtc, &dev->mode_config.crtc_list, base.head) {
		init_waitqueue_head(&intel_crtc->vbl_wait);

		drm_flip_helper_init(&intel_crtc->flip_helper,
				     &dev_priv->flip_driver, &intel_flip_funcs);
	}

	list_for_each_entry(intel_plane, &dev->mode_config.plane_list, base.head)
		drm_flip_helper_init(&intel_plane->flip_helper,
				     &dev_priv->flip_driver, &intel_flip_funcs);

	INIT_LIST_HEAD(&dev_priv->flip.list);
	spin_lock_init(&dev_priv->flip.lock);
	INIT_WORK(&dev_priv->flip.work, intel_atomic_process_flips_work);
	dev_priv->flip.wq = create_singlethread_workqueue("intel_flip");
}

static void intel_flip_fini(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc;
	struct intel_plane *intel_plane;

	list_for_each_entry(intel_plane, &dev->mode_config.plane_list, base.head)
		drm_flip_helper_fini(&intel_plane->flip_helper);

	list_for_each_entry(intel_crtc, &dev->mode_config.crtc_list, base.head)
		drm_flip_helper_fini(&intel_crtc->flip_helper);

	drm_flip_driver_fini(&dev_priv->flip_driver);
}

static bool intel_atomic_postpone_flip(struct intel_flip *intel_flip)
{
	struct intel_ring_buffer *ring = intel_flip->ring;
	int ret;

	ret = i915_gem_check_olr(ring, intel_flip->seqno);
	if (WARN_ON(ret)) {
		intel_flip->ring = NULL;
		return false;
	}

	if (i915_seqno_passed(ring->get_seqno(ring, true), intel_flip->seqno)) {
		intel_flip->ring = NULL;
		return false;
	}

	if (WARN_ON(!ring->irq_get(ring))) {
		intel_flip->ring = NULL;
		return false;
	}

	return true;
}

static void intel_atomic_schedule_flips(struct drm_i915_private *dev_priv,
					struct intel_crtc *intel_crtc,
					struct list_head *flips)
{
	if (!intel_crtc->active) {
		drm_flip_driver_complete_flips(&dev_priv->flip_driver, flips);
		return;
	}

	drm_flip_driver_prepare_flips(&dev_priv->flip_driver, flips);

	local_irq_disable();

	intel_pipe_vblank_evade(&intel_crtc->base);

	drm_flip_driver_schedule_flips(&dev_priv->flip_driver, flips);

	local_irq_enable();
}

static  bool intel_atomic_flips_ready(struct drm_device *dev, unsigned int flip_seq)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_flip *intel_flip;

	/* check if all flips w/ same flip_seq are ready */
	list_for_each_entry(intel_flip, &dev_priv->flip.list, base.list) {
		if (intel_flip->flip_seq != flip_seq)
			break;

		if (intel_flip->ring)
			return false;
	}

	return true;
}

static void intel_atomic_process_flips_work(struct work_struct *work)
{
	struct drm_i915_private *dev_priv = container_of(work, struct drm_i915_private, flip.work);
	struct drm_device *dev = dev_priv->dev;

	for (;;) {
		struct intel_flip *intel_flip, *next;
		unsigned int flip_seq;
		struct intel_crtc *intel_crtc;
		LIST_HEAD(flips);
		unsigned long flags;

		if (list_empty(&dev_priv->flip.list))
			return;

		spin_lock_irqsave(&dev_priv->flip.lock, flags);

		intel_flip = list_first_entry(&dev_priv->flip.list, struct intel_flip, base.list);
		flip_seq = intel_flip->flip_seq;
		intel_crtc = to_intel_crtc(intel_flip->crtc);

		if (intel_atomic_flips_ready(dev, flip_seq)) {
			list_for_each_entry_safe(intel_flip, next, &dev_priv->flip.list, base.list) {
				if (intel_flip->flip_seq != flip_seq)
					break;
				list_move_tail(&intel_flip->base.list, &flips);
				dev_priv->flip.queue_len--;
			}
		}

		trace_i915_flip_queue_len(dev_priv->flip.queue_len);

		spin_unlock_irqrestore(&dev_priv->flip.lock, flags);

		if (list_empty(&flips))
			return;

		mutex_lock(&dev->mode_config.mutex);
		intel_atomic_schedule_flips(dev_priv, intel_crtc, &flips);
		mutex_unlock(&dev->mode_config.mutex);
	}
}

static void intel_atomic_check_flips_ready(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_flip *intel_flip;

	if (list_empty(&dev_priv->flip.list))
		return;

	intel_flip = list_first_entry(&dev_priv->flip.list, struct intel_flip, base.list);
	if (intel_atomic_flips_ready(dev, intel_flip->flip_seq))
		queue_work(dev_priv->flip.wq, &dev_priv->flip.work);
}

void intel_atomic_notify_ring(struct drm_device *dev,
			      struct intel_ring_buffer *ring)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_flip *intel_flip;
	unsigned long flags;
	u32 seqno;

	if (list_empty(&dev_priv->flip.list))
		return;

	seqno = ring->get_seqno(ring, false);

	spin_lock_irqsave(&dev_priv->flip.lock, flags);

	list_for_each_entry(intel_flip, &dev_priv->flip.list, base.list) {
		if (ring != intel_flip->ring)
			continue;

		if (i915_seqno_passed(seqno, intel_flip->seqno)) {
			intel_flip->ring = NULL;
			ring->irq_put(ring);
		}
	}

	intel_atomic_check_flips_ready(dev);

	spin_unlock_irqrestore(&dev_priv->flip.lock, flags);
}

void intel_atomic_wedged(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_flip *intel_flip;
	unsigned long flags;

	spin_lock_irqsave(&dev_priv->flip.lock, flags);

	list_for_each_entry(intel_flip, &dev_priv->flip.list, base.list) {
		struct intel_ring_buffer *ring = intel_flip->ring;

		if (ring) {
			intel_flip->ring = NULL;
			ring->irq_put(ring);
		}
	}

	/* all flips are "ready" so no need to check with intel_atomic_flips_ready() */
	if (!list_empty(&dev_priv->flip.list))
		queue_work(dev_priv->flip.wq, &dev_priv->flip.work);

	spin_unlock_irqrestore(&dev_priv->flip.lock, flags);
}

static void intel_atomic_flip_init(struct intel_flip *intel_flip,
				   struct drm_device *dev,
				   u32 flip_seq,
				   struct drm_i915_file_private *file_priv,
				   struct drm_pending_atomic_event *event,
				   struct drm_framebuffer *fb,
				   bool unpin_old_fb, struct drm_framebuffer *old_fb)
{
	intel_flip->flip_seq = flip_seq;

	if (event) {
		intel_flip->event = event;

		/* need to keep track of it in case process exits */
		spin_lock_irq(&dev->event_lock);
		list_add_tail(&intel_flip->pending_head, &file_priv->pending_flips);
		spin_unlock_irq(&dev->event_lock);
	}

	if (fb) {
		struct drm_i915_gem_object *obj = to_intel_framebuffer(fb)->obj;

		mutex_lock(&dev->struct_mutex);
#ifdef USE_WRITE_SEQNO
		intel_flip->seqno = obj->last_write_seqno;
#else
		intel_flip->seqno = obj->last_read_seqno;
#endif
		intel_flip->ring = obj->ring;
		mutex_unlock(&dev->struct_mutex);
	}

	if (old_fb)
		intel_flip->old_fb_id = old_fb->base.id;

	if (unpin_old_fb && old_fb) {
		intel_flip->old_bo = to_intel_framebuffer(old_fb)->obj;

		mutex_lock(&dev->struct_mutex);
		drm_gem_object_reference(&intel_flip->old_bo->base);
		mutex_unlock(&dev->struct_mutex);
	}
}

static void atomic_pipe_commit(struct drm_device *dev,
			       struct intel_atomic_state *state,
			       int pipe)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_file_private *file_priv = state->file->driver_priv;
	LIST_HEAD(flips);
	int i;
	/* FIXME treat flips for all pipes as one set for GPU sync */
	unsigned int flip_seq = dev_priv->flip.next_flip_seq++;
	struct intel_flip *intel_flip, *next;
	unsigned long flags;
	struct intel_ring_buffer *ring;
	unsigned int rings_mask = 0;

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct intel_crtc_state *st = &state->crtc[i];
		struct drm_crtc *crtc = st->crtc;
		struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
		struct intel_flip *intel_flip;

		if (!st->fb_dirty && !st->cursor_dirty)
			continue;

		if (intel_crtc->pipe != pipe)
			continue;

		intel_flip = st->flip;
		st->flip = NULL;

		drm_flip_init(&intel_flip->base, &intel_crtc->flip_helper);

		intel_atomic_flip_init(intel_flip, dev, flip_seq,
				       file_priv, st->event, crtc->fb,
				       st->fb_dirty, st->old.fb);
		st->event = NULL;

		intel_flip->crtc = crtc;

		/* update primary_disabled befoer calc_plane() */
		intel_crtc->primary_disabled = st->primary_disabled;

		/* should already be checked so can't fail */
		/* FIXME refactor the failing parts? */
		dev_priv->display.calc_plane(crtc, crtc->fb, crtc->x, crtc->y);
		intel_flip->regs = intel_crtc->primary_regs;

		if (st->cursor_dirty) {
			intel_flip->has_cursor = true;
			intel_flip->old_cursor_bo = st->old.cursor_bo;
		}

		list_add_tail(&intel_flip->base.list, &flips);
	}

	for (i = 0; i < dev->mode_config.num_plane; i++) {
		struct intel_plane_state *st = &state->plane[i];
		struct drm_plane *plane = st->plane;
		struct intel_plane *intel_plane = to_intel_plane(plane);
		struct intel_flip *intel_flip;

		if (!st->dirty)
			continue;

		if (intel_plane->pipe != pipe)
			continue;

		intel_flip = st->flip;
		st->flip = NULL;

		drm_flip_init(&intel_flip->base, &intel_plane->flip_helper);

		intel_atomic_flip_init(intel_flip, dev, flip_seq,
				       file_priv, st->event, plane->fb,
				       st->dirty, st->old.fb);
		st->event = NULL;

		intel_flip->crtc = intel_get_crtc_for_pipe(dev, pipe);
		intel_flip->plane = plane;

		intel_plane->calc(plane, plane->fb, &st->coords);
		intel_flip->regs = intel_plane->regs;

		list_add_tail(&intel_flip->base.list, &flips);
	}

	if (list_empty(&flips))
		return;

	if (!drm_async_gpu) {
		struct intel_crtc *intel_crtc = to_intel_crtc(intel_get_crtc_for_pipe(dev, pipe));
		intel_atomic_schedule_flips(dev_priv, intel_crtc, &flips);
		return;
	}

	mutex_lock(&dev->struct_mutex);

	list_for_each_entry(intel_flip, &flips, base.list) {
		struct intel_ring_buffer *ring = intel_flip->ring;

		if (!ring)
			continue;

		if (intel_atomic_postpone_flip(intel_flip))
			rings_mask |= intel_ring_flag(ring);
	}

	spin_lock_irqsave(&dev_priv->flip.lock, flags);

	list_for_each_entry_safe(intel_flip, next, &flips, base.list) {
		list_move_tail(&intel_flip->base.list, &dev_priv->flip.list);
		dev_priv->flip.queue_len++;
	}

	trace_i915_flip_queue_len(dev_priv->flip.queue_len);

	/* if no rings are involved, we can avoid checking seqnos */
	if (rings_mask == 0)
		intel_atomic_check_flips_ready(dev);

	spin_unlock_irqrestore(&dev_priv->flip.lock, flags);

	mutex_unlock(&dev->struct_mutex);

	if (rings_mask == 0)
		return;

	if (atomic_read(&dev_priv->mm.wedged)) {
		intel_atomic_wedged(dev);
		return;
	}

	/*
	 * Double check to catch cases where the irq
	 * fired before the flip was placed onto flip.list.
	 */
	for_each_ring(ring, dev_priv, i) {
		if (rings_mask & intel_ring_flag(ring))
			intel_atomic_notify_ring(dev, ring);
	}
}

void intel_atomic_handle_vblank(struct drm_device *dev, int pipe)
{
	struct intel_crtc *intel_crtc = to_intel_crtc(intel_get_crtc_for_pipe(dev, pipe));
	struct intel_plane *intel_plane;

	intel_crtc->vbl_received = true;
	wake_up(&intel_crtc->vbl_wait);

	drm_flip_helper_vblank(&intel_crtc->flip_helper);

	list_for_each_entry(intel_plane, &dev->mode_config.plane_list, base.head) {
		if (intel_plane->pipe == pipe)
			drm_flip_helper_vblank(&intel_plane->flip_helper);
	}
}

void intel_atomic_clear_flips(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct intel_plane *intel_plane;
	struct intel_flip *intel_flip, *next;
	int pipe = intel_crtc->pipe;
	unsigned long flags;
	LIST_HEAD(flips);

	/*
	 * If there are flips still waiting for the GPU, remove them
	 * from the list, so that they won't be able to move over to
	 * drm_flip_helpers' possession after we've called
	 * drm_flip_helper_clear().
	 */
	spin_lock_irqsave(&dev_priv->flip.lock, flags);
	list_cut_position(&flips, &dev_priv->flip.list, dev_priv->flip.list.prev);
	spin_unlock_irqrestore(&dev_priv->flip.lock, flags);

	drm_flip_helper_clear(&intel_crtc->flip_helper);

	list_for_each_entry(intel_plane, &dev->mode_config.plane_list, base.head) {
		if (intel_plane->pipe == pipe)
			drm_flip_helper_clear(&intel_plane->flip_helper);
	}

	/*
	 * Drop all non-ready flips. Doing this after calling
	 * drm_flip_helper_clear() maintaines the correct order
	 * of completion events.
	 */
	list_for_each_entry_safe(intel_flip, next, &flips, base.list) {
		struct intel_ring_buffer *ring = intel_flip->ring;

		if (ring) {
			intel_flip->ring = NULL;
			ring->irq_put(ring);
		}

		intel_flip_complete(&intel_flip->base);
		/*
		 * FIXME drm_flip_helper calls the following functions
		 * from a workqueue. Perhaps we should do the same here?
		 */
		intel_flip_finish(&intel_flip->base);
		intel_flip_cleanup(&intel_flip->base);
	}
}
