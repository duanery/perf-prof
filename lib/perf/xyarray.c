// SPDX-License-Identifier: GPL-2.0
#include <internal/xyarray.h>
#include <linux/zalloc.h>
#include <stdlib.h>
#include <string.h>

struct xyarray *xyarray__new(int xlen, int ylen, size_t entry_size)
{
	size_t row_size = ylen * entry_size;
	struct xyarray *xy = zalloc(sizeof(*xy) + xlen * row_size);

	if (xy != NULL) {
		xy->entry_size = entry_size;
		xy->row_size   = row_size;
		xy->entries    = xlen * ylen;
		xy->max_x      = xlen;
		xy->max_y      = ylen;
	}

	return xy;
}

/*
 * Grow the y dimension, keeping the contents. Rows are laid out contiguously,
 * so every row has to be moved to its new offset; walk backwards to avoid
 * clobbering rows that have not been moved yet.
 */
struct xyarray *xyarray__grow_y(struct xyarray *xy, int ylen)
{
	size_t old_row_size, new_row_size;
	struct xyarray *tmp;
	int x;

	if (!xy || ylen <= (int)xy->max_y)
		return xy;

	old_row_size = xy->row_size;
	new_row_size = ylen * xy->entry_size;

	tmp = realloc(xy, sizeof(*xy) + xy->max_x * new_row_size);
	if (!tmp)
		return NULL;
	xy = tmp;

	for (x = xy->max_x - 1; x >= 0; x--) {
		char *from = &xy->contents[x * old_row_size];
		char *to   = &xy->contents[x * new_row_size];

		if (x)
			memmove(to, from, old_row_size);
		memset(to + old_row_size, 0, new_row_size - old_row_size);
	}

	xy->row_size = new_row_size;
	xy->entries  = xy->max_x * ylen;
	xy->max_y    = ylen;

	return xy;
}

void xyarray__reset(struct xyarray *xy)
{
	size_t n = xy->entries * xy->entry_size;

	memset(xy->contents, 0, n);
}

void xyarray__delete(struct xyarray *xy)
{
	free(xy);
}
