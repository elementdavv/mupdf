#include "mupdf/fitz.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <android/log.h>

#include "mupdf/fitz/buffer.h"
#include "mupdf/fitz/context.h"
#include "mupdf/fitz/stream.h"

#include "ddjvuapi.h"
#include "tiffiop.h"

extern int try_open_archive;

typedef struct
{
	int idx;
	int size;
	char *name;
	char *buf;
} djvu_entry;

typedef struct
{
	fz_archive super;

	int count;
	const char *filename;
	djvu_entry *entries;
	ddjvu_context_t *dctx;
	ddjvu_document_t *doc;
} fz_djvu_archive;

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
  int flag_quality = 900;

  /* Process size specification */
  prect.x = 0;
  prect.y = 0;
      prect.w = iw;
      prect.h = ih;

  /* Process aspect ratio */
  if (iw > 0 && ih > 0)
    {
      double dw = (double)iw / prect.w;
      double dh = (double)ih / prect.h;
      if (dw > dh)
        prect.h = (int)(ih / dw);
      else
        prect.w = (int)(iw / dh);
    }

  /* Process segment specification */
  rrect = prect;

  /* Process mode specification */
  mode = DDJVU_RENDER_COLOR;

  /* Determine output pixel format and compression */
  style = DDJVU_FORMAT_RGB24;

      compression = COMPRESSION_NONE;
# ifdef JPEG_SUPPORT
      if (TIFFFindCODEC(COMPRESSION_JPEG))
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

  if (! (fmt = ddjvu_format_create(style, 0, 0))) {
	__android_log_print(ANDROID_LOG_INFO, "libmupdf", "Cannot determine pixel style for page %d", pageno);
	return;
	}
  ddjvu_format_set_row_order(fmt, 1);
  /* Allocate buffer */
    rowsize = rrect.w * 3;
  size_t bufsize = (size_t)rowsize * rrect.h;

  if (bufsize / rowsize != rrect.h) {
	__android_log_print(ANDROID_LOG_INFO, "libmupdf", "Integer overflow when allocating image buffer for page %d", pageno);
	return;
	}
  image = (char*)malloc(bufsize);

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
# ifdef JPEG_SUPPORT
          if (compression != COMPRESSION_JPEG)
# endif
# ifdef ZIP_SUPPORT
            if (compression != COMPRESSION_DEFLATE)
# endif
              TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, (uint32)64);

          TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, (uint16)8);
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
	char filename[256] = {0};
	sprintf(filename, "%s/page.tiff", ctx->private_path);
	djvu->filename = fz_strdup(ctx, filename);
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

	for (int i = 0; i < djvu->count; i++) {
		char name[16] = {0};
		sprintf(name, "%d.tiff", i);
		djvu->entries[i].name = fz_strdup(ctx, name);
		djvu->entries[i].idx = i;
		djvu->entries[i].size = 0;
		djvu->entries[i].buf = 0;
	}
}

static void retrive(fz_context *ctx, TIFF *tiff, fz_djvu_archive *djvu, int i)
{
	int size = TIFFGetFileSize(tiff);
	djvu->entries[i].buf = fz_malloc(ctx, size);
	TIFFSeekFile(tiff, 0, SEEK_SET);

	if (TIFFReadFile(tiff, djvu->entries[i].buf, size) == size) {
		djvu->entries[i].size = size;
	}
	else {
		fz_free(ctx, djvu->entries[i].buf);
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
		TIFF *tiff = TIFFOpen(djvu->filename, "w");
		render(tiff, page, i);
		ddjvu_page_release(page);
		TIFFFlush(tiff);
		retrive(ctx, tiff, djvu, i);
		TIFFClose(tiff);
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

	if (ent->size == 0)
		decodeEntry(ctx, djvu, ent->idx);

	if (ent->size == 0)
		return NULL;

	fz_buffer *ubuf = fz_new_buffer_from_copied_data(ctx, ent->buf, ent->size);
	return ubuf;
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

		if (djvu->entries[i].size > 0)
			fz_free(ctx, djvu->entries[i].buf);
	}
	fz_free(ctx, djvu->entries);
	fz_free(ctx, djvu->filename);
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
