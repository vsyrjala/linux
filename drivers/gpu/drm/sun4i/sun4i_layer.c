/*
 * Copyright (C) 2015 Free Electrons
 * Copyright (C) 2015 NextThing Co
 *
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drmP.h>

#include "sun4i_backend.h"
#include "sun4i_frontend.h"
#include "sun4i_layer.h"
#include "sunxi_engine.h"

struct sun4i_plane_desc {
	enum drm_plane_type     type;
	u8                      pipe;
	const uint32_t          *formats;
	uint32_t                nformats;
};

static void sun4i_backend_layer_reset(struct drm_plane *plane)
{
	struct sun4i_layer *layer = plane_to_sun4i_layer(plane);
	struct sun4i_layer_state *state;

	if (plane->state) {
		state = state_to_sun4i_layer_state(plane->state);

		__drm_atomic_helper_plane_destroy_state(&state->state);

		kfree(state);
		plane->state = NULL;
	}

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state) {
		plane->state = &state->state;
		plane->state->plane = plane;
		plane->state->zpos = layer->id;
	}
}

static struct drm_plane_state *
sun4i_backend_layer_duplicate_state(struct drm_plane *plane)
{
	struct sun4i_layer_state *orig = state_to_sun4i_layer_state(plane->state);
	struct sun4i_layer_state *copy;

	copy = kzalloc(sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &copy->state);
	copy->uses_frontend = orig->uses_frontend;

	return &copy->state;
}

static void sun4i_backend_layer_destroy_state(struct drm_plane *plane,
					      struct drm_plane_state *state)
{
	struct sun4i_layer_state *s_state = state_to_sun4i_layer_state(state);

	__drm_atomic_helper_plane_destroy_state(state);

	kfree(s_state);
}

static void sun4i_backend_layer_atomic_disable(struct drm_plane *plane,
					       struct drm_plane_state *old_state)
{
	struct sun4i_layer_state *layer_state = state_to_sun4i_layer_state(old_state);
	struct sun4i_layer *layer = plane_to_sun4i_layer(plane);
	struct sun4i_backend *backend = layer->backend;

	sun4i_backend_layer_enable(backend, layer->id, false);

	if (layer_state->uses_frontend) {
		unsigned long flags;

		spin_lock_irqsave(&backend->frontend_lock, flags);
		backend->frontend_teardown = true;
		spin_unlock_irqrestore(&backend->frontend_lock, flags);
	}
}

static void sun4i_backend_layer_atomic_update(struct drm_plane *plane,
					      struct drm_plane_state *old_state)
{
	struct sun4i_layer_state *layer_state = state_to_sun4i_layer_state(plane->state);
	struct sun4i_layer *layer = plane_to_sun4i_layer(plane);
	struct sun4i_backend *backend = layer->backend;
	struct sun4i_frontend *frontend = backend->frontend;

	if (layer_state->uses_frontend) {
		sun4i_frontend_init(frontend);
		sun4i_frontend_update_coord(frontend, plane);
		sun4i_frontend_update_buffer(frontend, plane);
		sun4i_frontend_update_formats(frontend, plane,
					      DRM_FORMAT_ARGB8888);
		sun4i_backend_update_layer_frontend(backend, layer->id,
						    DRM_FORMAT_ARGB8888);
		sun4i_frontend_enable(frontend);
	} else {
		sun4i_backend_update_layer_formats(backend, layer->id, plane);
		sun4i_backend_update_layer_buffer(backend, layer->id, plane);
	}

	sun4i_backend_update_layer_coord(backend, layer->id, plane);
	sun4i_backend_update_layer_zpos(backend, layer->id, plane);
	sun4i_backend_layer_enable(backend, layer->id, true);
}

static const struct drm_plane_helper_funcs sun4i_backend_layer_helper_funcs = {
	.atomic_disable	= sun4i_backend_layer_atomic_disable,
	.atomic_update	= sun4i_backend_layer_atomic_update,
};

static const struct drm_plane_funcs sun4i_backend_layer_funcs = {
	.atomic_destroy_state	= sun4i_backend_layer_destroy_state,
	.atomic_duplicate_state	= sun4i_backend_layer_duplicate_state,
	.destroy		= drm_plane_cleanup,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.reset			= sun4i_backend_layer_reset,
	.update_plane		= drm_atomic_helper_update_plane,
};

static const uint32_t sun4i_backend_layer_formats_primary[] = {
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const uint32_t sun4i_backend_layer_formats_overlay[] = {
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_ARGB4444,
	DRM_FORMAT_ARGB1555,
	DRM_FORMAT_RGBA5551,
	DRM_FORMAT_RGBA4444,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const struct sun4i_plane_desc sun4i_backend_planes[] = {
	{
		.type = DRM_PLANE_TYPE_PRIMARY,
		.pipe = 0,
		.formats = sun4i_backend_layer_formats_primary,
		.nformats = ARRAY_SIZE(sun4i_backend_layer_formats_primary),
	},
	{
		.type = DRM_PLANE_TYPE_OVERLAY,
		.pipe = 1,
		.formats = sun4i_backend_layer_formats_overlay,
		.nformats = ARRAY_SIZE(sun4i_backend_layer_formats_overlay),
	},
};

static struct sun4i_layer *sun4i_layer_init_one(struct drm_device *drm,
						struct sun4i_backend *backend,
						const struct sun4i_plane_desc *plane)
{
	struct sun4i_layer *layer;
	int ret;

	layer = devm_kzalloc(drm->dev, sizeof(*layer), GFP_KERNEL);
	if (!layer)
		return ERR_PTR(-ENOMEM);

	/* possible crtcs are set later */
	ret = drm_universal_plane_init(drm, &layer->plane, 0,
				       &sun4i_backend_layer_funcs,
				       plane->formats, plane->nformats,
				       NULL, plane->type, NULL);
	if (ret) {
		dev_err(drm->dev, "Couldn't initialize layer\n");
		return ERR_PTR(ret);
	}

	drm_plane_helper_add(&layer->plane,
			     &sun4i_backend_layer_helper_funcs);
	layer->backend = backend;

	return layer;
}

struct drm_plane **sun4i_layers_init(struct drm_device *drm,
				     struct sunxi_engine *engine)
{
	struct drm_plane **planes;
	struct sun4i_backend *backend = engine_to_sun4i_backend(engine);
	int i;

	/* We need to have a sentinel at the need, hence the overallocation */
	planes = devm_kcalloc(drm->dev, SUN4I_BACKEND_NUM_LAYERS + 1,
			      sizeof(*planes), GFP_KERNEL);
	if (!planes)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < ARRAY_SIZE(sun4i_backend_planes); i++) {
		const struct sun4i_plane_desc *plane = &sun4i_backend_planes[i];
		struct sun4i_layer *layer;

		layer = sun4i_layer_init_one(drm, backend, plane);
		if (IS_ERR(layer)) {
			dev_err(drm->dev, "Couldn't initialize %s plane\n",
				i ? "overlay" : "primary");
			return ERR_CAST(layer);
		};

		drm_plane_create_zpos_immutable_property(&layer->plane, i);

		DRM_DEBUG_DRIVER("Assigning %s plane to pipe %d\n",
				 i ? "overlay" : "primary", plane->pipe);
		regmap_update_bits(engine->regs, SUN4I_BACKEND_ATTCTL_REG0(i),
				   SUN4I_BACKEND_ATTCTL_REG0_LAY_PIPESEL_MASK,
				   SUN4I_BACKEND_ATTCTL_REG0_LAY_PIPESEL(plane->pipe));

		layer->id = i;
		planes[i] = &layer->plane;
	};

	return planes;
}
