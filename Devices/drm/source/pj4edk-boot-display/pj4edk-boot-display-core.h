/* SPDX-License-Identifier: GPL-2.0-only */
/* Small allocation-free terminal. Coordinates are native landscape pixels;
 * LK scans a 1200x1920 RGB565 buffer with its existing 180-degree DPU mount.
 * Kept independent of printk/MMIO so the exact raster code can be exercised
 * against guarded RAM before a kernel is booted on the board.
 */
#ifndef PJ4EDK_BOOT_DISPLAY_CORE_H
#define PJ4EDK_BOOT_DISPLAY_CORE_H

#include "common/include/pjb_boot_display.h"

#define PJ4EDK_COLS 160
#define PJ4EDK_ROWS 50
#define PJ4EDK_CELL_W 12
#define PJ4EDK_CELL_H 24

/* Pre-rotated RGB565 glyphs: 55 KiB built once, then contiguous 48-byte
 * copies per glyph column instead of per-pixel scale/bit/color calculation.
 */
struct pj4edk_glyphs {
	unsigned short pixels[96][PJ4EDK_CELL_W][PJ4EDK_CELL_H];
};

static void pj4edk_prepare_font(struct pj4edk_glyphs *glyphs, const unsigned char *font)
{
	unsigned int ch, x, y;

	for (ch = 32; ch < 128; ch++)
		for (x = 0; x < PJ4EDK_CELL_W; x++)
			for (y = 0; y < PJ4EDK_CELL_H; y++) {
				unsigned int bits = font[ch * 16 + y * 16 / PJ4EDK_CELL_H];

				glyphs->pixels[ch - 32][x][PJ4EDK_CELL_H - 1 - y] =
					bits & (0x80 >> (x * 8 / PJ4EDK_CELL_W)) ? 0xdedb : 0;
			}
}

struct pj4edk_terminal {
	unsigned short *pixels;
	const unsigned char *font;
	const struct pj4edk_glyphs *glyphs;
	unsigned int left, top, cols, rows, col, row;
	unsigned long long dirty;
	unsigned char text[PJ4EDK_ROWS][PJ4EDK_COLS];
};

static int pj4edk_terminal_init(struct pj4edk_terminal *t, const unsigned int *v,
			     unsigned short *pixels, const unsigned char *font)
{
	unsigned int right, bottom, i, j;

	if (!pjb_display_validate_cells(v, PJB_DISPLAY_CELLS) ||
	    v[2] || v[3] != 0xb0000000 ||
	    v[4] != 0x9e4000 || v[5] != 4608000 || v[6] != 1200 ||
	    v[7] != 1920 || v[8] != 2400 ||
	    !((v[10] == 1280 && v[11] == 800) ||
	      (v[10] == 1920 && v[11] == 1200)) || v[16] != 90 ||
	    v[17] != 180 || v[14] < 128 || v[15] < 80)
		return -1;
	/* Inset eight native pixels, leaving the PJ4EDK frame and its border. */
	t->left = (v[12] * 1920 + v[10] - 1) / v[10] + 8;
	t->top = (v[13] * 1200 + v[11] - 1) / v[11] + 8;
	right = (v[12] + v[14]) * 1920 / v[10] - 8;
	bottom = (v[13] + v[15]) * 1200 / v[11] - 8;
	t->cols = (right - t->left) / PJ4EDK_CELL_W;
	t->rows = (bottom - t->top) / PJ4EDK_CELL_H;
	if (!t->cols || t->cols > PJ4EDK_COLS || !t->rows || t->rows > PJ4EDK_ROWS)
		return -1;
	t->pixels = pixels;
	t->font = font;
	t->glyphs = 0;
	t->col = t->row = 0;
	t->dirty = (1ULL << t->rows) - 1;
	for (i = 0; i < t->rows; i++)
		for (j = 0; j < t->cols; j++)
			t->text[i][j] = ' ';
	return 0;
}

static void pj4edk_newline(struct pj4edk_terminal *t)
{
	unsigned int i, j;

	t->col = 0;
	if (++t->row < t->rows)
		return;
	t->row = t->rows - 1;
	for (i = 1; i < t->rows; i++)
		for (j = 0; j < t->cols; j++)
			t->text[i - 1][j] = t->text[i][j];
	for (j = 0; j < t->cols; j++)
		t->text[t->row][j] = ' ';
	t->dirty = (1ULL << t->rows) - 1;
}

static void pj4edk_character(struct pj4edk_terminal *t, unsigned char ch)
{
	if (ch == '\r') {
		t->col = 0;
		return;
	}
	if (ch == '\n') {
		pj4edk_newline(t);
		return;
	}
	if (ch == '\t') {
		unsigned int n = 4 - (t->col % 4);

		while (n--)
			pj4edk_character(t, ' ');
		return;
	}
	if (ch < 32)
		return;
	if (t->col == t->cols)
		pj4edk_newline(t);
	t->text[t->row][t->col++] = ch < 127 ? ch : '?';
	t->dirty |= 1ULL << t->row;
}

/* Each glyph column is contiguous in the inherited rotated scanout. */
static void pj4edk_draw_cell(struct pj4edk_terminal *t, unsigned int row, unsigned int col)
{
	unsigned int x, y, ch = t->text[row][col];

	for (x = 0; x < PJ4EDK_CELL_W; x++) {
		if (t->glyphs) {
			unsigned int nx = t->left + col * PJ4EDK_CELL_W + x;
			unsigned int py = 1200 - t->top - (row + 1) * PJ4EDK_CELL_H;

			__builtin_memcpy(t->pixels + nx * 1200 + py,
					t->glyphs->pixels[ch - 32][x],
					PJ4EDK_CELL_H * sizeof(unsigned short));
			continue;
		}
		for (y = 0; y < PJ4EDK_CELL_H; y++) {
			unsigned int bits = t->font[ch * 16 + y * 16 / PJ4EDK_CELL_H];
			unsigned short color = bits & (0x80 >> (x * 8 / PJ4EDK_CELL_W)) ? 0xdedb : 0;
			unsigned int nx = t->left + col * PJ4EDK_CELL_W + x;
			unsigned int ny = t->top + row * PJ4EDK_CELL_H + y;

			t->pixels[nx * 1200 + 1199 - ny] = color;
		}
	}
}

struct pj4edk_damage {
	/* Half-open changed row interval for each glyph column. */
	unsigned char first[PJ4EDK_COLS], end[PJ4EDK_COLS];
	unsigned int cells;
};

struct pj4edk_shadow {
	unsigned long long valid;
	unsigned char text[PJ4EDK_ROWS][PJ4EDK_COLS];
};

/* Compare the final batched text with the last drawn text. Scrolling need not
 * redraw identical spaces/glyphs. Cache cleaning merges adjacent dirty rows
 * into one interval per physical scanline (at most cols * 12 calls/frame).
 */
static void pj4edk_draw_damage(struct pj4edk_terminal *t, struct pj4edk_shadow *shadow,
			    struct pj4edk_damage *damage)
{
	unsigned int row, col;

	damage->cells = 0;
	for (col = 0; col < t->cols; col++) {
		damage->first[col] = t->rows;
		damage->end[col] = 0;
	}
	for (row = 0; row < t->rows; row++) {
		if (!(t->dirty & (1ULL << row)))
			continue;
		for (col = 0; col < t->cols; col++) {
			if ((shadow->valid & (1ULL << row)) &&
			    shadow->text[row][col] == t->text[row][col])
				continue;
			pj4edk_draw_cell(t, row, col);
			shadow->text[row][col] = t->text[row][col];
			if (damage->first[col] > row)
				damage->first[col] = row;
			damage->end[col] = row + 1;
			damage->cells++;
		}
		shadow->valid |= 1ULL << row;
	}
}
#endif
