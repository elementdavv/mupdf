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

#include <unistd.h>
#include "djvu-archive.h"

extern int try_open_archive;
static char djvu_cache_path[1024] = {0};

void fz_set_djvu_cache_path(const char *path)
{
	strncpy(djvu_cache_path, path, sizeof(djvu_cache_path) - 1);
}

void handle_ddjvu_messages(ddjvu_context_t *dctx, int wait)
{
    const ddjvu_message_t *msg;

    if (wait)
    	ddjvu_message_wait(dctx);

    while ((msg = ddjvu_message_peek(dctx)))
    {
       	switch(msg->m_any.tag)
       	{
       	case DDJVU_ERROR:
			__android_log_print(ANDROID_LOG_INFO, "error", "%s", msg->m_error.message);
			break;
       	case DDJVU_INFO:
			break;
       	case DDJVU_NEWSTREAM:
			break;
       	case DDJVU_DOCINFO:
			break;
       	default:
			break;
       	}
       	ddjvu_message_pop(dctx);
    }
}

static void render(TIFF *tiff, ddjvu_page_t *page, int pageno)
{
  ddjvu_rect_t prect;
  ddjvu_rect_t rrect;
  ddjvu_format_style_t style;
  ddjvu_render_mode_t mode;
  ddjvu_format_t *fmt;
  int iw = ddjvu_page_get_width(page);
  int ih = ddjvu_page_get_height(page);
  int dpi = ddjvu_page_get_resolution(page);
  ddjvu_page_type_t type = ddjvu_page_get_type(page);
  char *image = 0;
  char white = (char)0xFF;
  int rowsize;
  int compression = COMPRESSION_NONE;
  // 1-100 jpeg, 900 zip, 901 lzw, 1000 raw
  int flag_quality = 900;

  /* Process size specification */
  prect.x = 0;
  prect.y = 0;
      prect.w = iw;
      prect.h = ih;

  /* Process segment specification */
  rrect = prect;

  /* Process mode specification */
  mode = DDJVU_RENDER_COLOR;

  /* Determine output pixel format and compression */
  style = DDJVU_FORMAT_RGB24;
  if (type==DDJVU_PAGETYPE_BITONAL)
    {
      style = DDJVU_FORMAT_GREY8;
      if ((int)prect.w == iw && (int)prect.h == ih)
        style = DDJVU_FORMAT_MSBTOLSB;
    }

      if (flag_quality < 1000) {
# ifdef CCITT_SUPPORT
      if (style==DDJVU_FORMAT_MSBTOLSB
          && TIFFFindCODEC(COMPRESSION_CCITT_T6))
        compression = COMPRESSION_CCITT_T6;
# endif
# ifdef JPEG_SUPPORT
      if (compression == COMPRESSION_NONE
          && style!=DDJVU_FORMAT_MSBTOLSB
          && flag_quality>0 && flag_quality<=100
          && TIFFFindCODEC(COMPRESSION_JPEG))
        compression = COMPRESSION_JPEG;
# endif
# ifdef ZIP_SUPPORT
      if (compression == COMPRESSION_NONE
          && flag_quality == 900
          && TIFFFindCODEC(COMPRESSION_DEFLATE))
        /* All pdf engines understand deflate. */
        compression = COMPRESSION_DEFLATE;
# endif
# ifdef LZW_SUPPORT
      if (compression == COMPRESSION_NONE
          && flag_quality == 901
          && TIFFFindCODEC(COMPRESSION_LZW))
        /* Because of patents that are now expired, some versions
           of libtiff only support lzw decoding and trigger an error
           condition when trying to encode. Unfortunately we cannot
           know this in advance and select another compression scheme. */
        compression = COMPRESSION_LZW;
# endif
# ifdef PACKBITS_SUPPORT
      if (compression == COMPRESSION_NONE
          && TIFFFindCODEC(COMPRESSION_PACKBITS))
        /* This mediocre default produces the most portable tiff files. */
        compression = COMPRESSION_PACKBITS;
# endif
      }

  fmt = ddjvu_format_create(style, 0, 0);
  ddjvu_format_set_row_order(fmt, 1);
  ddjvu_format_set_gamma(fmt, 2.2);
  /* Allocate buffer */
  if (style == DDJVU_FORMAT_MSBTOLSB) {
    white = 0x00;
    rowsize = (rrect.w + 7) / 8;
  } else if (style == DDJVU_FORMAT_GREY8)
    rowsize = rrect.w;
  else
    rowsize = rrect.w * 3;
  size_t bufsize = (size_t)rowsize * rrect.h;

  if (! (image = (char*)malloc(bufsize))) {
	__android_log_print(ANDROID_LOG_INFO, "libmupdf", "Cannot allocate image buffer for page %d", pageno);
	return;
  }

  /* Render */
  if (! ddjvu_page_render(page, mode, &prect, &rrect, fmt, rowsize, image))
    memset(image, white, rowsize * rrect.h);

  /* Output */
        int i;
        char *s = image;
        TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, (uint32)rrect.w);
        TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, (uint32)rrect.h);
        TIFFSetField(tiff, TIFFTAG_XRESOLUTION, (float)((dpi*prect.w+iw/2)/iw));
        TIFFSetField(tiff, TIFFTAG_YRESOLUTION, (float)((dpi*prect.h+ih/2)/ih));
        TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
# ifdef CCITT_SUPPORT
        if (compression != COMPRESSION_CCITT_T6)
# endif
# ifdef JPEG_SUPPORT
          if (compression != COMPRESSION_JPEG)
# endif
# ifdef ZIP_SUPPORT
            if (compression != COMPRESSION_DEFLATE)
# endif
              TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, (uint32)64);
        if (style == DDJVU_FORMAT_MSBTOLSB) {
          TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, (uint16)1);
          TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, (uint16)1);
          TIFFSetField(tiff, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB);
          TIFFSetField(tiff, TIFFTAG_COMPRESSION, compression);
          TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE);
        } else {
          TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, (uint16)8);
          if (style == DDJVU_FORMAT_GREY8) {
            TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, (uint16)1);
            TIFFSetField(tiff, TIFFTAG_COMPRESSION, compression);
            TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
          } else {
            TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, (uint16)3);
            TIFFSetField(tiff, TIFFTAG_COMPRESSION, compression);
# ifdef JPEG_SUPPORT
            if (compression == COMPRESSION_JPEG) {
              TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR);
              TIFFSetField(tiff, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
              TIFFSetField(tiff, TIFFTAG_JPEGQUALITY, flag_quality);
            } else
# endif
              TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
          }
        }
        if (rowsize != TIFFScanlineSize(tiff)) {
	       __android_log_print(ANDROID_LOG_INFO, "libmupdf", "internal error (%d!=%d)", rowsize, (int)TIFFScanlineSize(tiff));
	       return;
        }
        for (i=0; i<(int)rrect.h; i++,s+=rowsize)
          TIFFWriteScanline(tiff, s, i, 0);

  /* Free */
  ddjvu_format_release(fmt);
  free(image);
}

static void ensure_djvu_context(fz_context *ctx, fz_djvu_archive *djvu)
{
	djvu->doc = 0;

	const char *programname = "net.timelegend.mupdf";
	const char *url = "https://timelegend.net/mupdf";
	fz_stream *file = djvu->super.file;
	fz_buffer *buf;

	fz_try(ctx) {
		fz_seek(ctx, file, 0, 0);
		buf = fz_read_all(ctx, file, 1024);
	}
	fz_catch(ctx) {
		fz_rethrow(ctx);
	}
	ddjvu_context_t* dctx = ddjvu_context_create(programname);

	if (! dctx) {
		fz_drop_buffer(ctx, buf);
		fz_throw(ctx, FZ_ERROR_GENERIC, "ddjvu_context_create");
	}
	ddjvu_document_t * doc  = ddjvu_document_create(dctx, url, TRUE);

	if (! doc) {
		ddjvu_context_release(dctx);
		fz_drop_buffer(ctx, buf);
		fz_throw(ctx, FZ_ERROR_GENERIC, "ddjvu_document_create");
	}
	ddjvu_stream_write(doc, 0, (const char *)buf->data, buf->len);
	ddjvu_stream_close(doc, 0, FALSE);

  	while (! ddjvu_document_decoding_done(doc)) {
    	handle_ddjvu_messages(dctx, TRUE);
	}
	if (ddjvu_document_decoding_error(doc)) {
		ddjvu_document_release(doc);
		ddjvu_context_release(dctx);
		fz_drop_buffer(ctx, buf);
		fz_throw(ctx, FZ_ERROR_GENERIC, "ddjvu_document_decoding_error");
	}
	miniexp_t outline;

	while ((outline = ddjvu_document_get_outline(doc)) == miniexp_dummy)
		handle_ddjvu_messages(dctx, TRUE);

	djvu->outline = outline;
	djvu->doc = doc;
	djvu->dctx = dctx;
	fz_drop_buffer(ctx, buf);
}

static void ensure_djvu_entries(fz_context *ctx, fz_djvu_archive *djvu)
{
	ensure_djvu_context(ctx, djvu);

	if (djvu->doc == 0)
		return;

	djvu->count = ddjvu_document_get_pagenum(djvu->doc);
	djvu->entries = fz_realloc_array(ctx, djvu->entries, djvu->count, djvu_entry);
	int filenum = ddjvu_document_get_filenum(djvu->doc);

	for (int i = 0; i < filenum; i++) {
		ddjvu_fileinfo_t finfo;
		ddjvu_document_get_fileinfo(djvu->doc, i, &finfo);

		if (finfo.type == 80) {		// [P]age
			djvu->entries[finfo.pageno].idx = finfo.pageno;
			const char *name = finfo.name ? finfo.name : (finfo.id ? finfo.id : finfo.title);
			djvu->entries[finfo.pageno].name = fz_strdup(ctx, name);
			djvu->entries[finfo.pageno].ubuf = 0;
			djvu->entries[finfo.pageno].hyperlinks = 0;
		}
	}
}

static void retrive(fz_context *ctx, TIFF *tiff, fz_djvu_archive *djvu, int i)
{
	int size = TIFFGetFileSize(tiff);
	unsigned char *data = fz_malloc(ctx, size);
	TIFFSeekFile(tiff, 0, SEEK_SET);

	if (TIFFReadFile(tiff, data, size) == size) {
		djvu->entries[i].ubuf = fz_new_buffer_from_data(ctx, data, size);
	}
	else {
		fz_free(ctx, data);
	}
}

static void decodeEntry(fz_context *ctx, fz_djvu_archive *djvu, int i)
{
		__android_log_print(ANDROID_LOG_INFO, "libmupdf", "decode:%d",i);

	if (djvu->doc == 0)
		ensure_djvu_context(ctx, djvu);

	if (djvu->doc == 0)
		return;

	ddjvu_page_t *page = ddjvu_page_create_by_pageno(djvu->doc, i);

	if (page) {
		while (! ddjvu_page_decoding_done(page)) {
    		handle_ddjvu_messages(djvu->dctx, TRUE);
		}
		if (ddjvu_page_decoding_error(page)) {
			fz_throw(ctx, FZ_ERROR_GENERIC, "ddjvu_page_decoding_error: %d", i);
		}
		char filename[1024] = {0};
		strcat(filename, djvu_cache_path);
		strcat(filename, "/");
		strcat(filename, djvu->entries[i].name);
		TIFF *tiff = TIFFOpen(filename, "w");
		render(tiff, page, i);
		ddjvu_page_release(page);
		TIFFFlush(tiff);
		retrive(ctx, tiff, djvu, i);
		TIFFClose(tiff);
		unlink(filename);
		miniexp_t pagetext;

		while ((pagetext = ddjvu_document_get_pagetext(djvu->doc, i, "word")) == miniexp_dummy)
			handle_ddjvu_messages(djvu->dctx, TRUE);

		djvu->entries[i].pagetext = pagetext;
		miniexp_t pageanno;

		while ((pageanno = ddjvu_document_get_pageanno(djvu->doc, i)) == miniexp_dummy)
			handle_ddjvu_messages(djvu->dctx, TRUE);

		djvu->entries[i].hyperlinks = ddjvu_anno_get_hyperlinks(pageanno);
	}
}

static djvu_entry *lookup_djvu_entry(fz_context *ctx, fz_djvu_archive *djvu, const char *name)
{
	for (int i = 0; i < djvu->count; i++)
		if (!fz_strcasecmp(name, djvu->entries[i].name))
			return &djvu->entries[i];

	return NULL;
}

static fz_buffer *read_djvu_entry(fz_context *ctx, fz_archive *arch, const char *name)
{
	fz_djvu_archive *djvu = (fz_djvu_archive *) arch;
	djvu_entry *ent = lookup_djvu_entry(ctx, djvu, name);

	if (! ent)
		return NULL;

	if (ent->ubuf == 0)
		decodeEntry(ctx, djvu, ent->idx);

	if (ent->ubuf == 0)
		return NULL;

	return ent->ubuf;
}

static fz_stream *open_djvu_entry(fz_context *ctx, fz_archive *arch, const char *name)
{
	fz_buffer *ubuf = read_djvu_entry(ctx, arch, name);
	return fz_open_buffer(ctx, ubuf);
}

static int count_djvu_entries(fz_context *ctx, fz_archive *arch)
{
	fz_djvu_archive *djvu = (fz_djvu_archive *) arch;
	return djvu->count;
}

static const char *list_djvu_entry(fz_context *ctx, fz_archive *arch, int idx)
{
	fz_djvu_archive *djvu = (fz_djvu_archive *) arch;

	if (idx < 0 || idx >= djvu->count)
		return NULL;

	return djvu->entries[idx].name;
}

static int has_djvu_entry(fz_context *ctx, fz_archive *arch, const char *name)
{
	fz_djvu_archive *djvu = (fz_djvu_archive *) arch;
	djvu_entry *ent = lookup_djvu_entry(ctx, djvu, name);
	return ent != NULL;
}

static void drop_djvu_archive(fz_context *ctx, fz_archive *arch)
{
	fz_djvu_archive *djvu = (fz_djvu_archive *) arch;

	for (int i = 0; i < djvu->count; ++i) {
		fz_free(ctx, djvu->entries[i].name);
		fz_drop_buffer(ctx, djvu->entries[i].ubuf);

		if (djvu->entries[i].hyperlinks)
			fz_free(ctx, djvu->entries[i].hyperlinks);
	}
	fz_free(ctx, djvu->entries);
	ddjvu_document_release(djvu->doc);
	ddjvu_context_release(djvu->dctx);
	djvu->doc = 0;
}

int
fz_is_djvu_archive(fz_context *ctx, fz_stream *file)
{
	const unsigned char signature[4] = { 'A', 'T', '&', 'T' };
	unsigned char data[4];
	size_t n;

	if (file == NULL)
		return 0;

	fz_seek(ctx, file, 0, 0);
	n = fz_read(ctx, file, data, nelem(data));

	if (n != nelem(signature))
		return 0;

	if (memcmp(data, signature, nelem(signature)))
		return 0;

	return 1;
}

fz_archive *
fz_open_djvu_archive_with_stream(fz_context *ctx, fz_stream *file)
{
	if (try_open_archive)
		return NULL;

	fz_djvu_archive *djvu;

	if (!fz_is_djvu_archive(ctx, file))
		fz_throw(ctx, FZ_ERROR_FORMAT, "cannot recognize djvu archive");

	djvu = fz_new_derived_archive(ctx, file, fz_djvu_archive);
	djvu->super.format = "djvu";
	djvu->super.count_entries = count_djvu_entries;
	djvu->super.list_entry = list_djvu_entry;
	djvu->super.has_entry = has_djvu_entry;
	djvu->super.read_entry = read_djvu_entry;
	djvu->super.open_entry = open_djvu_entry;
	djvu->super.drop_archive = drop_djvu_archive;

	fz_try(ctx)
	{
		ensure_djvu_entries(ctx, djvu);
	}
	fz_catch(ctx)
	{
		fz_drop_archive(ctx, &djvu->super);
		fz_rethrow(ctx);
	}

	return &djvu->super;
}

fz_archive *
fz_open_djvu_archive(fz_context *ctx, const char *filename)
{
	fz_archive *djvu = NULL;
	fz_stream *file;

	file = fz_open_file(ctx, filename);

	fz_var(djvu);

	fz_try(ctx)
		djvu = fz_open_djvu_archive_with_stream(ctx, file);
	fz_always(ctx)
		fz_drop_stream(ctx, file);
	fz_catch(ctx)
		fz_rethrow(ctx);

	return djvu;
}
