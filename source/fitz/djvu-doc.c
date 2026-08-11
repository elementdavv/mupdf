// Copyright (C) 2026 Artifex Software, Inc.
//
// This file is part of MuPDF.
//
// MuPDF is free software: you can redistribute it and/or modify it under the
// terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// MuPDF is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
// details.
//
// You should have received a copy of the GNU Affero General Public License
// along with MuPDF. If not, see <https://www.gnu.org/licenses/agpl-3.0.en.html>
//
// Alternative licensing terms are available from the licensor.
// For commercial licensing, see <https://www.artifex.com/> or contact
// Artifex Software, Inc., 39 Mesa Street, Suite 108A, San Francisco,
// CA 94129, USA, for further information.

#include "djvu-archive.h"

#include <math.h>

#define DPI 72.0f

typedef struct
{
	fz_page super;
	fz_image *image;
	miniexp_t pagetext;
	miniexp_t *hyperlinks;
	fz_link *link;
} djvu_page;

typedef struct
{
	fz_document super;
	fz_archive *arch;
	fz_outline *outline;
	int page_count;
	const char **page;
} djvu_document;

static void
djvu_create_page_list(fz_context *ctx, djvu_document *doc)
{
	fz_archive *arch = doc->arch;
	doc->page_count = fz_count_archive_entries(ctx, arch);
	doc->page = fz_malloc_array(ctx, doc->page_count, const char *);

	for (int i = 0; i < doc->page_count; i++)
	{
		const char *name = fz_list_archive_entry(ctx, arch, i);
		doc->page[i] = name;
	}
}

static fz_outline *
djvu_create_outline_imp(fz_context *ctx, miniexp_t bookmark)
{
	fz_outline *outline, *head, **tailp;
	head = NULL;
	tailp = &head;

	for (int i = 0; i < miniexp_length(bookmark); i++) {
		miniexp_t bm = miniexp_nth(i, bookmark);

		if (miniexp_consp(bm)) {
			miniexp_t name = miniexp_car(bm);
			const char *text = miniexp_to_str(name);
			miniexp_t cdr = miniexp_cdr(bm);
			miniexp_t value = miniexp_car(cdr);
			const char *path = miniexp_to_str(value);
			miniexp_t subbookmark = miniexp_cdr(cdr);

			fz_try(ctx)
			{
				*tailp = outline = fz_new_outline(ctx);
				tailp = &(*tailp)->next;
				outline->title = Memento_label(fz_strdup(ctx, text), "outline_title");
				outline->uri = Memento_label(fz_strdup(ctx, path), "outline_uri");
				outline->page = fz_make_location(-1, -1);
				outline->down = djvu_create_outline_imp(ctx, subbookmark);
				outline->is_open = 1;
			}
			fz_catch(ctx)
			{
				fz_drop_outline(ctx, head);
				fz_rethrow(ctx);
			}
		}
	}
	return head;
}

static void
djvu_create_outline(fz_context *ctx, djvu_document *doc)
{
	fz_djvu_archive *arch = (fz_djvu_archive*)doc->arch;
	miniexp_t outline = arch->outline;

	if (miniexp_consp(outline)) {
		miniexp_t bme = miniexp_car(outline);
		const char *name = miniexp_to_name(bme);

		if (!strcmp(name, "bookmarks")) {
			doc->outline = djvu_create_outline_imp(ctx, miniexp_cdr(outline));
			return;
		}
	}
	doc->outline = NULL;
}

static fz_outline *
djvu_load_outline(fz_context *ctx, fz_document *doc_)
{
	djvu_document *doc = (djvu_document*)doc_;
	return fz_keep_outline(ctx, doc->outline);
}

static fz_link_dest
djvu_resolve_link(fz_context *ctx, fz_document *doc_, const char *uri)
{
	const char *p = uri;

	if (*p == '#') {
		p++;

		if (*p >= '1' && *p <= '9') {
			return fz_make_link_dest_xyz(0, fz_atoi(p) - 1, 0, 0, 0);
		}
		else {
			djvu_document *doc = (djvu_document*)doc_;

			for (int i = 0; i < doc->page_count; i++) {
				if (!strcmp(p, doc->page[i])) {
					return fz_make_link_dest_xyz(0, i, 0, 0, 0);
				}
			}
		}
	}
	return fz_make_link_dest_none();
}

static void
djvu_drop_document(fz_context *ctx, fz_document *doc_)
{
	djvu_document *doc = (djvu_document*)doc_;
	fz_drop_archive(ctx, doc->arch);
	fz_drop_outline(ctx, doc->outline);
	fz_free(ctx, (char **)doc->page);
}

static int
djvu_count_pages(fz_context *ctx, fz_document *doc_, int chapter)
{
	djvu_document *doc = (djvu_document*)doc_;
	return doc->page_count;
}

static void
djvu_run_text_word(fz_context *ctx, char *s, fz_device *dev, fz_matrix ctm, int fontsize, int x, int x2, int y, float xscale, float yscale)
{
	float color[3] = { 0, 0, 0 };
	fz_text *text = fz_new_text(ctx);
	fz_font *font = fz_new_base14_font(ctx, "Times-Roman");

	// adjust fontsize and y
	int w = x2 - x;
	fz_matrix trm1 = fz_scale(fontsize, -fontsize);
	fz_matrix trm2 = fz_measure_string(ctx, font, trm1, s, 0, 0, FZ_BIDI_LTR, FZ_LANG_UNSET);
	int e = trm2.e;

	float ffs = (float)fontsize, fx = (float)x, fy = (float)y, delta;
	if ((w - e) > (e / 10) || (e - w) > (w / 10)) {
		delta = ffs * (w - e) / e;
		ffs += delta;
		fy += delta / 2;
	}

	fy -= ffs / 10;
	fontsize = roundf(ffs * yscale);
	x = roundf(fx * xscale);
	y = roundf(fy * yscale);
	fz_matrix trm = fz_scale(fontsize, -fontsize);
	trm.e = x;
	trm.f = y;
	fz_show_string(ctx, text, font, trm, s, 0, 0, FZ_BIDI_LTR, FZ_LANG_UNSET);
	fz_fill_text(ctx, dev, text, ctm, fz_device_rgb(ctx), color, 1, fz_default_color_params);
}

static void
djvu_run_text_print_words(fz_context *ctx, miniexp_t txt, int wstart, int wend, fz_device *dev, fz_matrix ctm, int y, int fontsize, float xscale, float yscale)
{
	for (int i = wstart; i < wend; i++) {
		miniexp_t subtxt = miniexp_nth(i, txt);
		miniexp_t exp5 = miniexp_nth(5, subtxt);

		if (miniexp_stringp(exp5)) {
			int x = miniexp_to_int(miniexp_nth(1, subtxt));
			int x2 = miniexp_to_int(miniexp_nth(3, subtxt));
			const char *s = miniexp_to_str(exp5);
			djvu_run_text_word(ctx, (char*)s, dev, ctm, fontsize, x, x2, y, xscale, yscale);
		}
	}
}

static void
djvu_run_text_print_lines(fz_context *ctx, miniexp_t txt, int lstart, int wstart, int lend, int wend, fz_device *dev, fz_matrix ctm, int y, int fontsize, float xscale, float yscale)
{
	for (int i = lstart; i <= lend && i < miniexp_length(txt); i++) {
		miniexp_t subtxt = miniexp_nth(i, txt);
		const char *detail = miniexp_to_name(miniexp_car(subtxt));

		if (!strcmp(detail, "line")) {
			miniexp_t exp5 = miniexp_nth(5, subtxt);

			if (miniexp_stringp(exp5)) {
				if (i < lend) {
					int x = miniexp_to_int(miniexp_nth(1, subtxt));
					int x2 = miniexp_to_int(miniexp_nth(3, subtxt));
					const char *s = miniexp_to_str(exp5);
					djvu_run_text_word(ctx, (char*)s, dev, ctm, fontsize, x, x2, y, xscale, yscale);
				}
			}
			else {
				int tstart = (i == lstart) ? wstart : 5;
				int tend = (i == lend) ? wend : miniexp_length(subtxt);
				djvu_run_text_print_words(ctx, subtxt, tstart, tend, dev, ctm, y, fontsize, xscale, yscale);
			}
		}
		else
			break;
	}
}

static int
djvu_run_text_cross(miniexp_t txt, int *y1, int *y2, int *h)
{
	int _y1 = miniexp_to_int(miniexp_nth(2, txt));
	int _y2 = miniexp_to_int(miniexp_nth(4, txt));
	if (*y1 == 0) *y1 = _y1;
	if (*y2 == 0) *y2 = _y2;

	if (!(*y1 > _y2 || *y2 < _y1)) {	// overlapped, same line
		if (*y1 > _y1) *y1 = _y1;
		if (*y2 < _y2) *y2 = _y2;
		int _h = _y2 - _y1;
		if (*h < _h) *h = _h;
		return 1;
	}
	return 0;
}

static void
djvu_run_text_check_word(miniexp_t txt, int wstart, int *wend, int *y1, int *y2, int *h)
{
	int i;
	for (i = wstart; i < miniexp_length(txt); i++) {
		miniexp_t subtxt = miniexp_nth(i, txt);
		const char *detail = miniexp_to_name(miniexp_car(subtxt));

		if (!strcmp(detail, "word")) {
			if (miniexp_stringp(miniexp_nth(5, subtxt))) {
				if (!djvu_run_text_cross(subtxt, y1, y2, h))
					break;
			}
		}
	}
	*wend = i;
}

static void
djvu_run_text_check_line(miniexp_t txt, int lstart, int wstart, int *lend, int *wend, int *y, int *fontsize)
{
	int y1 = 0, y2 = 0, h = 0, i;
	for (i = lstart; i < miniexp_length(txt); i++) {
		miniexp_t subtxt = miniexp_nth(i, txt);
		const char *detail = miniexp_to_name(miniexp_car(subtxt));

		if (!strcmp(detail, "line")) {
			if (miniexp_stringp(miniexp_nth(5, subtxt))) {
				if (!djvu_run_text_cross(subtxt, &y1, &y2, &h))
					break;
			}
			else {
				djvu_run_text_check_word(subtxt, wstart, wend, &y1, &y2, &h);

				if (*wend < miniexp_length(subtxt))
					break;
				else
					*wend = 5;
			}
			wstart = 5;
		}
		else
			break;
	}
	*y = y1;
	*fontsize = h;
	*lend = i;
}

static void
djvu_run_text_sub1(fz_context *ctx, miniexp_t txt, fz_device *dev, fz_matrix ctm, int ymax, float xscale, float yscale)
{
	if (miniexp_length(txt) < 6) return;

	for (int i = 5; i < miniexp_length(txt); i++) {
		miniexp_t subtxt = miniexp_nth(i, txt);
		const char *detail = miniexp_to_name(miniexp_car(subtxt));

		if (!strcmp(detail, "line")) {
			int first = 1;
			int lstart, wstart, lend = 5, wend = 5;
			int y, fontsize;

			while (wend > 5 || first) {
				if (first)
					lstart = i;
				else
					lstart = lend;

				first = 0;
				wstart = wend;
				djvu_run_text_check_line(txt, lstart, wstart, &lend, &wend, &y, &fontsize);
				djvu_run_text_print_lines(ctx, txt, lstart, wstart, lend, wend, dev, ctm, ymax - y, fontsize, xscale, yscale);
			}
			i = lend - 1;
		}
		else {
			djvu_run_text_sub1(ctx, subtxt, dev, ctm, ymax, xscale, yscale);
		}
	}
}

static void
djvu_run_text_sub(fz_context *ctx, miniexp_t txt, fz_device *dev, fz_matrix ctm, int ymax, float xscale, float yscale)
{
	if (miniexp_length(txt) > 5) {
		miniexp_t exp5 = miniexp_nth(5, txt);

		if (miniexp_stringp(exp5)) {
			int x = miniexp_to_int(miniexp_nth(1, txt));
			int y1 = miniexp_to_int(miniexp_nth(2, txt));
			int x2 = miniexp_to_int(miniexp_nth(3, txt));
			int y2 = miniexp_to_int(miniexp_nth(4, txt));
			const char *s = miniexp_to_str(exp5);
			int fontsize = y2 - y1;
			int y = ymax - y1;
			djvu_run_text_word(ctx, (char*)s, dev, ctm, fontsize, x, x2, y, xscale, yscale);
			return;
		}
		for (int i = 5; i < miniexp_length(txt); i++) {
			miniexp_t subtxt = miniexp_nth(i, txt);
			djvu_run_text_sub(ctx, subtxt, dev, ctm, ymax, xscale, yscale);
		}
	}
}

static void
djvu_run_text(fz_context *ctx, djvu_page *page, fz_device *dev, fz_matrix ctm, float xscale, float yscale)
{
	miniexp_t pagetext = page->pagetext;

	if (miniexp_length(pagetext) > 5) {
		const char *detail = miniexp_to_name(miniexp_car(pagetext));

		if (!strcmp(detail, "page")) {
			int ymax = miniexp_to_int(miniexp_nth(4, pagetext));

			// respect every word's fontsize and y position
			for (int i = 5; i < miniexp_length(pagetext); i++) {
				miniexp_t txt = miniexp_nth(i, pagetext);
				djvu_run_text_sub(ctx, txt, dev, ctm, ymax, xscale, yscale);
			}

			// traverse to find words in a line and make them share fontsize and y position
			// djvu_run_text_sub1(ctx, pagetext, dev, ctm, ymax, xscale, yscale);
		}
	}
}

static fz_rect
djvu_bound_page(fz_context *ctx, fz_page *page_, fz_box_type box)
{
	djvu_page *page = (djvu_page*)page_;
	fz_image *image = page->image;
	int xres, yres;
	fz_rect bbox = fz_empty_rect;
	uint8_t orientation;

	if (image)
	{
		fz_image_resolution(image, &xres, &yres);
		bbox.x0 = bbox.y0 = 0;
		orientation = fz_image_orientation(ctx, image);
		if (orientation == 0 || (orientation & 1) == 1)
		{
			bbox.x1 = image->w * DPI / xres;
			bbox.y1 = image->h * DPI / yres;
		}
		else
		{
			bbox.y1 = image->w * DPI / xres;
			bbox.x1 = image->h * DPI / yres;
		}
	}
	return bbox;
}

static void
djvu_run_page(fz_context *ctx, fz_page *page_, fz_device *dev, fz_matrix ctm, fz_cookie *cookie)
{
	djvu_page *page = (djvu_page*)page_;
	fz_image *image = page->image;
	int xres, yres;
	float w, h;
	uint8_t orientation;
	fz_matrix immat;

	// show hidden text
	// dev->hints |= FZ_DONT_DECODE_IMAGES;

	if (image)
	{
		fz_try(ctx)
		{
			fz_image_resolution(image, &xres, &yres);
			orientation = fz_image_orientation(ctx, image);
			if (orientation == 0 || (orientation & 1) == 1)
			{
				w = image->w * DPI / xres;
				h = image->h * DPI / yres;
			}
			else
			{
				h = image->w * DPI / xres;
				w = image->h * DPI / yres;
			}
			if ((dev->hints & FZ_DONT_DECODE_IMAGES) == 0) {
				immat = fz_image_orientation_matrix(ctx, image);
				immat = fz_post_scale(immat, w, h);
				ctm = fz_concat(immat, ctm);
				fz_fill_image(ctx, dev, image, ctm, 1, fz_default_color_params);
			}
			else {
				djvu_run_text(ctx, page, dev, ctm, DPI / xres, DPI / yres);
			}
		}
		fz_catch(ctx)
		{
			fz_report_error(ctx);
			fz_warn(ctx, "cannot render image on page");
		}
	}
}

static fz_link *
djvu_create_links(fz_context *ctx, djvu_page *page)
{
	fz_link *link, *head, **tailp;
	head = NULL;
	tailp = &head;

	fz_image *image;
	int xres, yres, ymax = 0;
	float xscale, yscale;

	miniexp_t s_rect = miniexp_symbol("rect");
	miniexp_t s_text = miniexp_symbol("text");
	int i = 0;

	while (1) {
		miniexp_t hlink = page->hyperlinks[i];
		if (!hlink) break;

		if (!ymax) {
			image = page->image;
			if (!image) break;

			fz_image_resolution(image, &xres, &yres);
			ymax = image->h;
			xscale = DPI / xres;
			yscale = DPI / yres;
		}
		miniexp_t trect = miniexp_nth(3, hlink);

		if (miniexp_car(trect) == s_rect || miniexp_car(trect) == s_text) {
			int x = miniexp_to_int(miniexp_nth(1, trect));
			int y = miniexp_to_int(miniexp_nth(2, trect));
			int w = miniexp_to_int(miniexp_nth(3, trect));
			int h = miniexp_to_int(miniexp_nth(4, trect));
			const char *uri = miniexp_to_str(miniexp_nth(1, hlink));

			fz_try(ctx) {
				fz_rect area = fz_make_rect(x * xscale, (ymax - y - h) * yscale, (x + w) * xscale, (ymax - y) * yscale);
				*tailp = link = fz_new_derived_link(ctx, fz_link, area, uri);
				tailp = &(*tailp)->next;
			}
			fz_catch(ctx) {
				fz_drop_link(ctx, head);
				fz_rethrow(ctx);
			}
		}
		i++;
	}
	return head;
}

static fz_link *
djvu_load_links(fz_context *ctx, fz_page *page_)
{
	djvu_page *page = (djvu_page*)page_;

	if (!page->link) {
		page->link = djvu_create_links(ctx, page);
	}
	return fz_keep_link(ctx, page->link);
}

static void
djvu_drop_page(fz_context *ctx, fz_page *page_)
{
	djvu_page *page = (djvu_page*)page_;
	fz_drop_image(ctx, page->image);
	fz_drop_link(ctx, page->link);
}

static fz_page *
djvu_load_page(fz_context *ctx, fz_document *doc_, int chapter, int number)
{
	djvu_document *doc = (djvu_document*)doc_;
	djvu_page *page = NULL;
	fz_buffer *buf = NULL;

	if (number < 0 || number >= doc->page_count)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "invalid page number %d", number);

	fz_var(page);

	page = fz_new_derived_page(ctx, djvu_page, doc_);
	page->super.bound_page = djvu_bound_page;
	page->super.run_page_contents = djvu_run_page;
	page->super.load_links = djvu_load_links;
	page->super.drop_page = djvu_drop_page;

	fz_try(ctx)
	{
		buf = fz_read_archive_entry(ctx, doc->arch, doc->page[number]);
		fz_keep_buffer(ctx, buf);
		page->image = fz_new_image_from_buffer(ctx, buf);
		fz_djvu_archive *arch = (fz_djvu_archive*)doc->arch;
		page->pagetext = arch->entries[number].pagetext;
		page->hyperlinks = arch->entries[number].hyperlinks;
	}
	fz_always(ctx)
	{
		fz_drop_buffer(ctx, buf);
	}
	fz_catch(ctx)
	{
		fz_report_error(ctx);
		fz_warn(ctx, "cannot decode image on page, leaving it blank");
	}

	return (fz_page*)page;
}

static int
djvu_lookup_metadata(fz_context *ctx, fz_document *doc_, const char *key, char *buf, size_t size)
{
	djvu_document *doc = (djvu_document*)doc_;
	if (!strcmp(key, FZ_META_FORMAT))
		return 1 + (int) fz_strlcpy(buf, fz_archive_format(ctx, doc->arch), size);
	return -1;
}

static fz_document *
djvu_open_document(fz_context *ctx, const fz_document_handler *handler, fz_stream *file, fz_stream *accel, fz_archive *dir, void *state)
{
	djvu_document *doc = fz_new_derived_document(ctx, djvu_document);

	doc->super.drop_document = djvu_drop_document;
	doc->super.load_outline = djvu_load_outline;
	doc->super.resolve_link_dest = djvu_resolve_link;
	doc->super.count_pages = djvu_count_pages;
	doc->super.load_page = djvu_load_page;
	doc->super.lookup_metadata = djvu_lookup_metadata;

	fz_try(ctx)
	{
		if (file)
			doc->arch = fz_open_archive_with_stream(ctx, file);
		else
			doc->arch = fz_keep_archive(ctx, dir);
		djvu_create_page_list(ctx, doc);
		djvu_create_outline(ctx, doc);
	}
	fz_catch(ctx)
	{
		fz_drop_document(ctx, (fz_document*)doc);
		fz_rethrow(ctx);
	}
	return (fz_document*)doc;
}

static const char *djvu_extensions[] =
{
	"djvu",
	"djv",
	NULL
};

static const char *djvu_mimetypes[] =
{
	"image/vnd.djvu",
	"image/x.djvu",
	"image/x-djvu",
	NULL
};

static int
djvu_recognize_doc_content(fz_context *ctx, const fz_document_handler *handler, fz_stream *stream, fz_archive *dir, void **state, fz_document_recognize_state_free_fn **freestate)
{
	if (fz_is_djvu_archive(ctx, stream))
		return 100;
	return 0;
}

fz_document_handler djvu_document_handler =
{
	NULL,
	djvu_open_document,
	djvu_extensions,
	djvu_mimetypes,
	djvu_recognize_doc_content
};
