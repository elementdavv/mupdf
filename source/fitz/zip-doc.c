// Copyright (C) 2023 Artifex Software, Inc.
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

static fz_document *
zip_open_document(fz_context *ctx, const fz_document_handler *handler, fz_stream *ostm, fz_stream *accel, fz_archive *dir, void *state)
{
	fz_archive *arch = fz_open_zip_archive_with_stream(ctx, ostm);
	if (fz_count_archive_entries(ctx, arch) == 0)
		fz_throw(ctx, FZ_ERROR_FORMAT, "no entries found");

	const char *name = fz_list_archive_entry(ctx, arch, 0);
	fz_stream *stm = fz_open_archive_entry(ctx, arch, name);
	fz_buffer *buf = NULL;
	fz_document *doc = NULL;

	fz_var(buf);
	fz_var(doc);

	fz_try(ctx)
	{
		buf = fz_read_all(ctx, stm, 1024);
		/* No way to pass the magic onwards :( */
		doc = fz_open_document_with_buffer(ctx, "application/octet-stream", buf);
	}
	fz_always(ctx)
	{
		fz_drop_stream(ctx, stm);
		fz_drop_buffer(ctx, buf);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);

	return doc;
}

static const char *zip_extensions[] =
{
	"zip",
	NULL
};

static const char *zip_mimetypes[] =
{
	"application/zip",
	NULL
};

static int
zip_recognize_doc_content(fz_context *ctx, const fz_document_handler *handler, fz_stream *stream, fz_archive *dir, void **state, fz_document_recognize_state_free_fn **free_state)
{
	if (fz_is_zip_archive(ctx, stream))
		return 100;
	return 0;
}

fz_document_handler zip_document_handler =
{
	NULL,
	zip_open_document,
	zip_extensions,
	zip_mimetypes,
	zip_recognize_doc_content
};
