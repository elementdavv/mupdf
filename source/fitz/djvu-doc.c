// Copyright (C) 2004-2021 Artifex Software, Inc.
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

#include "mupdf/fitz.h"
#include "mupdf/fitz/buffer.h"

#include <string.h>
#include <stdlib.h>

#define DPI 72.0f

static const char *djvu_ext_list[] = {
	".tif",
	".tiff",
	NULL
};

typedef struct
{
	fz_page super;
	fz_image *image;
} djvu_page;

typedef struct
{
	fz_document super;
	fz_archive *arch;
	int page_count;
	const char **page;
} djvu_document;

static void
djvu_create_page_list(fz_context *ctx, djvu_document *doc)
{
	fz_archive *arch = doc->arch;
	int i, k, count;

	count = fz_count_archive_entries(ctx, arch);

	doc->page_count = 0;
	doc->page = fz_malloc_array(ctx, count, const char *);

	for (i = 0; i < count; i++)
	{
		const char *name = fz_list_archive_entry(ctx, arch, i);
		const char *ext = name ? strrchr(name, '.') : NULL;
		for (k = 0; djvu_ext_list[k]; k++)
		{
			if (ext && !fz_strcasecmp(ext, djvu_ext_list[k]))
			{
				doc->page[doc->page_count++] = name;
				break;
			}
		}
	}
}

static void
djvu_drop_document(fz_context *ctx, fz_document *doc_)
{
	djvu_document *doc = (djvu_document*)doc_;
	fz_drop_archive(ctx, doc->arch);
	fz_free(ctx, (char **)doc->page);
}

static int
djvu_count_pages(fz_context *ctx, fz_document *doc_, int chapter)
{
	djvu_document *doc = (djvu_document*)doc_;
	return doc->page_count;
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
			immat = fz_image_orientation_matrix(ctx, image);
			immat = fz_post_scale(immat, w, h);
			ctm = fz_concat(immat, ctm);
			fz_fill_image(ctx, dev, image, ctm, 1, fz_default_color_params);
		}
		fz_catch(ctx)
		{
			fz_report_error(ctx);
			fz_warn(ctx, "cannot render image on page");
		}
	}
}

static void
djvu_drop_page(fz_context *ctx, fz_page *page_)
{
	djvu_page *page = (djvu_page*)page_;
	fz_drop_image(ctx, page->image);
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
	page->super.drop_page = djvu_drop_page;

	fz_try(ctx)
	{
		buf = fz_read_archive_entry(ctx, doc->arch, doc->page[number]);
		fz_keep_buffer(ctx, buf);
		page->image = fz_new_image_from_buffer(ctx, buf);
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
