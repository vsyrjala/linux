/*
 * Copyright (C) 2017 Intel Corp.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef DRM_DYNARRAY_H
#define DRM_DYNARRAY_H

struct drm_dynarray {
	void *elems;
	unsigned int elem_size, num_elems;
};

/**
 * drm_dynarray_elem - Return a pointer to an element
 * @dynarr: the dynamic array
 * @index: the index of the element
 *
 * Returns:
 * A pointer to the element at @index in the array.
 */
static inline void *drm_dynarray_elem(const struct drm_dynarray *dynarr,
				      unsigned int index)
{
	if (index >= dynarr->num_elems)
		return NULL;
	return dynarr->elems + index * dynarr->elem_size;
}

int drm_dynarray_reserve(struct drm_dynarray *dynarr,
			 unsigned int index);

void drm_dynarray_init(struct drm_dynarray *dynarr,
		       unsigned int elem_size);
void drm_dynarray_fini(struct drm_dynarray *dynarr);

#endif
