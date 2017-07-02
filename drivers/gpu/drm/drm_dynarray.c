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

#include <linux/slab.h>
#include <linux/string.h>
#include <drm/drm_dynarray.h>

/**
 * DOC: Dynamic arrays
 *
 * Helper that provides dynamically growing arrays. The array
 * must be initilaized to specify the size of each element, and
 * space can be reserved in the array by specifying the element
 * index to be used.
 */

/**
 * drm_dynarray_init - Initialize the dynamic array
 * @dynarr: the dynamic array
 * @elem_size: size of each element in bytes
 *
 * Initialize the dynamic array and specify the size of
 * each element of the array.
 */
void drm_dynarray_init(struct drm_dynarray *dynarr,
		       unsigned int elem_size)
{
	memset(dynarr, 0, sizeof(*dynarr));
	dynarr->elem_size = elem_size;
}
EXPORT_SYMBOL(drm_dynarray_init);

/**
 * drm_dynarray_fini - Finalize the dynamic array
 * @dynarr: the dynamic array
 *
 * Finalize the dynamic array, ie. free the memory
 * used by the array.
 */
void drm_dynarray_fini(struct drm_dynarray *dynarr)
{
	kfree(dynarr->elems);
	memset(dynarr, 0, sizeof(*dynarr));
}
EXPORT_SYMBOL(drm_dynarray_fini);

/**
 * drm_dynarray_reserve - Reserve space in the dynamic array
 * @dynarr: the dynamic array
 * @index: the index of the element to reserve
 *
 * Grow the array sufficiently to make sure @index points
 * to a valid memory location within the array.
 */
int drm_dynarray_reserve(struct drm_dynarray *dynarr,
			 unsigned int index)
{
	unsigned int num_elems = index + 1;
	unsigned int old_num_elems = dynarr->num_elems;
	void *elems;

	if (num_elems <= old_num_elems)
		return 0;

	elems = krealloc(dynarr->elems,
			 num_elems * dynarr->elem_size, GFP_KERNEL);
	if (!elems)
		return -ENOMEM;

	dynarr->elems = elems;
	dynarr->num_elems = num_elems;

	memset(drm_dynarray_elem(dynarr, old_num_elems), 0,
	       (num_elems - old_num_elems) * dynarr->elem_size);

	return 0;
}
EXPORT_SYMBOL(drm_dynarray_reserve);
