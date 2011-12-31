/*
 * kmscon - Console Characters
 *
 * Copyright (c) 2011 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011 University of Tuebingen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Console Characters
 * A console always has a fixed width and height measured in number of
 * characters. This interfaces describes a single character.
 *
 * To be Unicode compatible, the most straightforward way would be using a UCS
 * number for each character and printing them. However, Unicode allows
 * combining marks, that is, a single printable character is constructed of
 * multiple characters. We support this by allowing to append characters to an
 * existing character. This should only be used with combining chars, though.
 * Otherwise you end up with multiple printable characters in a cell and the
 * output may get corrupted.
 *
 * We store each character (sequence) as UTF8 string because the pango library
 * accepts only UTF8. Hence, we avoid conversion to UCS or wide-characters.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <cairo.h>
#include <glib.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include "console.h"
#include "log.h"

/* maximum size of a single character */
#define KMSCON_CHAR_SIZE 6

struct kmscon_char {
	char *buf;
	size_t size;
	size_t len;
};

enum glyph_type {
	GLYPH_NONE,
	GLYPH_LAYOUT,
	GLYPH_STR,
};

struct kmscon_glyph {
	size_t ref;
	struct kmscon_char *ch;
	unsigned int width;

	int type;

	union {
		struct layout {
			PangoLayout *layout;
		} layout;
		struct str {
			PangoFont *font;
			PangoGlyphString *str;
			uint32_t ascent;
		} str;
	} src;
};

struct kmscon_font {
	size_t ref;

	unsigned int width;
	unsigned int height;
	GHashTable *glyphs;
	PangoContext *ctx;
};

static int kmscon_font_lookup(struct kmscon_font *font,
		const struct kmscon_char *key, struct kmscon_glyph **out);

static int new_char(struct kmscon_char **out, size_t size)
{
	struct kmscon_char *ch;

	if (!out)
		return -EINVAL;

	if (!size)
		size = KMSCON_CHAR_SIZE;

	ch = malloc(sizeof(*ch));
	if (!ch)
		return -ENOMEM;

	memset(ch, 0, sizeof(*ch));

	ch->size = size;
	ch->buf = malloc(ch->size);
	if (!ch->buf) {
		free(ch);
		return -ENOMEM;
	}

	memset(ch->buf, 0, ch->size);

	*out = ch;
	return 0;
}

int kmscon_char_new(struct kmscon_char **out)
{
	return new_char(out, 0);
}

int kmscon_char_new_u8(struct kmscon_char **out, const char *str, size_t len)
{
	int ret;
	struct kmscon_char *ch;

	if (!len)
		return kmscon_char_new(out);

	if (!out || !str)
		return -EINVAL;

	ret = new_char(&ch, len);
	if (ret)
		return ret;

	ret = kmscon_char_set_u8(ch, str, len);
	if (ret) {
		kmscon_char_free(ch);
		return ret;
	}

	*out = ch;
	return 0;
}

int kmscon_char_new_ucs4(struct kmscon_char **out, const uint32_t *str,
								size_t len)
{
	int ret;
	struct kmscon_char *ch;
	char *str8;
	glong siz;

	if (!len)
		return kmscon_char_new(out);

	if (!out || !str)
		return -EINVAL;

	str8 = g_ucs4_to_utf8(str, len, NULL, &siz, NULL);
	if (!str8 || siz < 0)
		return -EFAULT;

	ret = new_char(&ch, siz);
	if (ret)
		goto err_free;

	ret = kmscon_char_set_u8(ch, str8, siz);
	if (ret)
		goto err_char;

	g_free(str8);
	*out = ch;

	return 0;

err_char:
	kmscon_char_free(ch);
err_free:
	g_free(str8);
	return ret;
}

int kmscon_char_dup(struct kmscon_char **out, const struct kmscon_char *orig)
{
	struct kmscon_char *ch;
	int ret;

	if (!out || !orig)
		return -EINVAL;

	ret = new_char(&ch, orig->size);
	if (ret)
		return ret;

	ret = kmscon_char_set_u8(ch, orig->buf, orig->len);
	if (ret) {
		kmscon_char_free(ch);
		return ret;
	}

	*out = ch;
	return 0;
}

void kmscon_char_free(struct kmscon_char *ch)
{
	if (!ch)
		return;

	free(ch->buf);
	free(ch);
}

void kmscon_char_reset(struct kmscon_char *ch)
{
	if (!ch)
		return;

	ch->len = 0;
}

int kmscon_char_set(struct kmscon_char *ch, const struct kmscon_char *orig)
{
	return kmscon_char_set_u8(ch, orig->buf, orig->len);
}

int kmscon_char_set_u8(struct kmscon_char *ch, const char *str, size_t len)
{
	char *buf;

	if (!ch)
		return -EINVAL;

	if (ch->size < len) {
		buf = realloc(ch->buf, len);
		if (!buf)
			return -ENOMEM;
		ch->buf = buf;
		ch->size = len;
	}

	memcpy(ch->buf, str, len);
	ch->len = len;

	return 0;
}

int kmscon_char_set_ucs4(struct kmscon_char *ch, const uint32_t *str,
								size_t len)
{
	char *str8;
	glong siz;
	int ret;

	if (!ch)
		return -EINVAL;

	str8 = g_ucs4_to_utf8(str, len, NULL, &siz, NULL);
	if (!str8 || siz < 0) {
		g_free(str8);
		return -EFAULT;
	}

	ret = kmscon_char_set_u8(ch, str8, siz);
	g_free(str8);

	return ret;
}

const char *kmscon_char_get_u8(const struct kmscon_char *ch)
{
	if (!ch)
		return NULL;

	return ch->buf;
}

size_t kmscon_char_get_len(const struct kmscon_char *ch)
{
	if (!ch)
		return 0;

	return ch->len;
}

int kmscon_char_append_u8(struct kmscon_char *ch, const char *str, size_t len)
{
	char *buf;
	size_t nlen;

	if (!ch)
		return -EINVAL;

	nlen = ch->len + len;

	if (ch->size < nlen) {
		buf = realloc(ch->buf, nlen);
		if (!buf)
			return -EINVAL;
		ch->buf = buf;
		ch->size = nlen;
	}

	memcpy(&ch->buf[ch->len], str, len);
	ch->len += len;

	return 0;
}

/*
 * Create a hash for a kmscon_char. This uses a simple hash technique described
 * by Daniel J. Bernstein.
 */
static guint kmscon_char_hash(gconstpointer key)
{
	guint val = 5381;
	size_t i;
	const struct kmscon_char *ch = (void*)key;

	for (i = 0; i < ch->len; ++i)
		val = val * 33 + ch->buf[i];

	return val;
}

/* compare two kmscon_char for equality */
static gboolean kmscon_char_equal(gconstpointer a, gconstpointer b)
{
	const struct kmscon_char *ch1 = (void*)a;
	const struct kmscon_char *ch2 = (void*)b;

	if (ch1->len != ch2->len)
		return FALSE;

	return (memcmp(ch1->buf, ch2->buf, ch1->len) == 0);
}

/*
 * Glyphs
 * Glyphs are for internal use only! The outside world uses kmscon_char
 * objects in combination with kmscon_font to draw characters. Internally, we
 * cache a kmscon_glyph for every character that is drawn.
 * This allows us to speed up the drawing operations because most characters are
 * already cached.
 *
 * Glyphs are cached in a hash-table by each font. If a character is drawn, we
 * look it up in the hash-table (or create a new one if none is found) and draw
 * it to the framebuffer.
 * A glyph may use several ways to cache the glyph description:
 *   GLYPH_NONE:
 *     No information is currently attached so the glyph cannot be drawn.
 *   GLYPH_LAYOUT:
 *     The most basic drawing operation. This is the slowest of all but can draw
 *     any text you want. It uses a PangoLayout internally and recalculates the
 *     character sizes each time we draw them.
 */
static int kmscon_glyph_new(struct kmscon_glyph **out,
						const struct kmscon_char *ch)
{
	struct kmscon_glyph *glyph;
	int ret;

	if (!out || !ch || !ch->len)
		return -EINVAL;

	glyph = malloc(sizeof(*glyph));
	if (!glyph)
		return -ENOMEM;
	glyph->ref = 1;
	glyph->type = GLYPH_NONE;

	ret = kmscon_char_dup(&glyph->ch, ch);
	if (ret)
		goto err_free;

	*out = glyph;
	return 0;

err_free:
	free(glyph);
	return ret;
}

/*
 * Reset internal glyph description. You must use kmscon_glyph_set() again to
 * attach new glyph descriptions.
 */
static void kmscon_glyph_reset(struct kmscon_glyph *glyph)
{
	if (!glyph)
		return;

	switch (glyph->type) {
	case GLYPH_LAYOUT:
		g_object_unref(glyph->src.layout.layout);
		break;
	case GLYPH_STR:
		g_object_unref(glyph->src.str.font);
		pango_glyph_string_free(glyph->src.str.str);
		break;
	}

	glyph->type = GLYPH_NONE;
	glyph->width = 0;
}

static void kmscon_glyph_ref(struct kmscon_glyph *glyph)
{
	if (!glyph)
		return;

	++glyph->ref;
}

static void kmscon_glyph_unref(struct kmscon_glyph *glyph)
{
	if (!glyph || !glyph->ref)
		return;

	if (--glyph->ref)
		return;

	kmscon_glyph_reset(glyph);
	kmscon_char_free(glyph->ch);
	free(glyph);
}

/*
 * Generate glyph description.
 * This connects the glyph with the given font an generates the fastest glyph
 * description.
 * Returns 0 on success.
 */
static int kmscon_glyph_set(struct kmscon_glyph *glyph,
						struct kmscon_font *font)
{
	PangoLayout *layout;
	PangoLayoutLine *line;
	PangoGlyphItem *tmp;
	PangoGlyphString *str;
	PangoRectangle rec;

	if (!glyph || !font)
		return -EINVAL;

	layout = pango_layout_new(font->ctx);
	if (!layout)
		return -EINVAL;

	pango_layout_set_text(layout, glyph->ch->buf, glyph->ch->len);
	pango_layout_get_extents(layout, NULL, &rec);
	line = pango_layout_get_line_readonly(layout, 0);

	if (!line || !line->runs || line->runs->next) {
		kmscon_glyph_reset(glyph);
		glyph->type = GLYPH_LAYOUT;
		glyph->src.layout.layout = layout;
	} else {
		tmp = line->runs->data;
		str = pango_glyph_string_copy(tmp->glyphs);
		if (!str) {
			g_object_unref(layout);
			return -ENOMEM;
		}

		kmscon_glyph_reset(glyph);
		glyph->type = GLYPH_STR;

		glyph->src.str.str = str;
		glyph->src.str.font =
			g_object_ref(tmp->item->analysis.font);
		glyph->src.str.ascent =
			PANGO_PIXELS_CEIL(pango_layout_get_baseline(layout));

		g_object_unref(layout);
	}

	glyph->width = PANGO_PIXELS(rec.width);
	return 0;
}

/*
 * Measure font width
 * We simply draw all ASCII characters and use the average width as default
 * character width.
 * This has the side effect that all ASCII characters are already cached and the
 * console will speed up.
 */
static int measure_width(struct kmscon_font *font)
{
	unsigned int i, num, width;
	int ret;
	struct kmscon_char *ch;
	char buf;
	struct kmscon_glyph *glyph;

	if (!font)
		return -EINVAL;

	ret = kmscon_char_new(&ch);
	if (ret)
		return ret;

	num = 0;
	for (i = 0; i < 127; ++i) {
		buf = i;
		ret = kmscon_char_set_u8(ch, &buf, 1);
		if (ret)
			continue;

		ret = kmscon_font_lookup(font, ch, &glyph);
		if (ret)
			continue;

		if (glyph->width > 0) {
			width += glyph->width;
			num++;
		}

		kmscon_glyph_unref(glyph);
	}

	kmscon_char_free(ch);

	if (!num)
		return -EFAULT;

	font->width = width / num;
	log_debug("font: width is %u\n", font->width);

	return 0;
}

/*
 * Creates a new font
 * \height is the height in pixel that we have for each character.
 * Returns 0 on success and stores the new font in \out.
 */
int kmscon_font_new(struct kmscon_font **out, unsigned int height)
{
	struct kmscon_font *font;
	int ret;
	PangoFontDescription *desc;
	PangoFontMap *map;
	PangoLanguage *lang;
	cairo_font_options_t *opt;

	if (!out || !height)
		return -EINVAL;

	log_debug("font: new font (height %u)\n", height);

	font = malloc(sizeof(*font));
	if (!font)
		return -ENOMEM;
	font->ref = 1;
	font->height = height;

	map = pango_cairo_font_map_get_default();
	if (!map) {
		ret = -EFAULT;
		goto err_free;
	}

	font->ctx = pango_font_map_create_context(map);
	if (!font->ctx) {
		ret = -EFAULT;
		goto err_free;
	}

	pango_context_set_base_dir(font->ctx, PANGO_DIRECTION_LTR);

	desc = pango_font_description_from_string("monospace");
	if (!desc) {
		ret = -EFAULT;
		goto err_ctx;
	}

	pango_font_description_set_absolute_size(desc, PANGO_SCALE * height);
	pango_context_set_font_description(font->ctx, desc);
	pango_font_description_free(desc);

	lang = pango_language_get_default();
	if (!lang) {
		ret = -EFAULT;
		goto err_ctx;
	}

	pango_context_set_language(font->ctx, lang);

	if (!pango_cairo_context_get_font_options(font->ctx)) {
		opt = cairo_font_options_create();
		if (!opt) {
			ret = -EFAULT;
			goto err_ctx;
		}

		pango_cairo_context_set_font_options(font->ctx, opt);
		cairo_font_options_destroy(opt);
	}

	font->glyphs = g_hash_table_new_full(kmscon_char_hash,
			kmscon_char_equal, (GDestroyNotify) kmscon_char_free,
					(GDestroyNotify) kmscon_glyph_unref);
	if (!font->glyphs) {
		ret = -ENOMEM;
		goto err_ctx;
	}

	ret = measure_width(font);
	if (ret)
		goto err_hash;

	*out = font;
	return 0;

err_hash:
	g_hash_table_unref(font->glyphs);
err_ctx:
	g_object_unref(font->ctx);
err_free:
	free(font);
	return ret;
}

void kmscon_font_ref(struct kmscon_font *font)
{
	if (!font)
		return;

	++font->ref;
}

void kmscon_font_unref(struct kmscon_font *font)
{
	if (!font || !font->ref)
		return;

	if (--font->ref)
		return;

	g_hash_table_unref(font->glyphs);
	g_object_unref(font->ctx);
	free(font);
	log_debug("font: destroying font\n");
}

unsigned int kmscon_font_get_width(struct kmscon_font *font)
{
	if (!font)
		return 0;

	return font->width;
}

unsigned int kmscon_font_get_height(struct kmscon_font *font)
{
	if (!font)
		return 0;

	return font->height;
}

/*
 * Get glyph for given key. If no glyph can be found in the hash-table, then a
 * new glyph is created and added to the hash-table.
 * Returns 0 on success and stores the glyph with a new reference in \out.
 */
static int kmscon_font_lookup(struct kmscon_font *font,
		const struct kmscon_char *key, struct kmscon_glyph **out)
{
	struct kmscon_glyph *glyph;
	struct kmscon_char *ch;
	int ret;

	if (!font || !key || !out)
		return -EINVAL;

	glyph = g_hash_table_lookup(font->glyphs, key);
	if (!glyph) {
		ret = kmscon_char_dup(&ch, key);
		if (ret)
			return ret;

		ret = kmscon_glyph_new(&glyph, key);
		if (ret)
			goto err_char;

		ret = kmscon_glyph_set(glyph, font);
		if (ret)
			goto err_glyph;

		g_hash_table_insert(font->glyphs, ch, glyph);
	}

	kmscon_glyph_ref(glyph);
	*out = glyph;
	return 0;

err_glyph:
	kmscon_glyph_unref(glyph);
err_char:
	kmscon_char_free(ch);
	return ret;
}

/*
 * This draws a glyph for characters \ch into the given cairo context \cr.
 * The glyph will be drawn with the upper-left corner at x/y.
 * Returns 0 on success.
 */
int kmscon_font_draw(struct kmscon_font *font, const struct kmscon_char *ch,
					void *dcr, uint32_t x, uint32_t y)
{
	struct kmscon_glyph *glyph;
	int ret;
	cairo_t *cr = dcr;

	if (!font || !ch || !cr)
		return -EINVAL;

	ret = kmscon_font_lookup(font, ch, &glyph);
	if (ret)
		return ret;

	switch (glyph->type) {
	case GLYPH_LAYOUT:
		cairo_move_to(cr, x, y);
		pango_cairo_update_layout(cr, glyph->src.layout.layout);
		pango_cairo_show_layout(cr, glyph->src.layout.layout);
		break;
	case GLYPH_STR:
		cairo_move_to(cr, x, y + glyph->src.str.ascent);
		pango_cairo_show_glyph_string(cr, glyph->src.str.font,
							glyph->src.str.str);
		break;
	default:
		ret = -EFAULT;
		break;
	}

	kmscon_glyph_unref(glyph);

	return 0;
}
