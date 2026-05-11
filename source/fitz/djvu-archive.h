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

#ifndef DJVU_ARCHIVE_H
#define DJVU_ARCHIVE_H

#include "mupdf/fitz.h"

#include "miniexp.h"
#include "ddjvuapi.h"
#include "tiffiop.h"

#include <android/log.h>

typedef struct
{
	int idx;
	char *name;
	fz_buffer *ubuf;
	miniexp_t pagetext;
	miniexp_t *hyperlinks;
} djvu_entry;

typedef struct
{
	fz_archive super;

	int count;
	djvu_entry *entries;
	ddjvu_context_t *dctx;
	ddjvu_document_t *doc;
    miniexp_t outline;
} fz_djvu_archive;

#endif
