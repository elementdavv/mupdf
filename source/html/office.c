#include "mupdf/fitz.h"
#include "html-imp.h"

#undef DEBUG_OFFICE_TO_HTML

/* Defaults are all 0's. FIXME: Very subject to change. Possibly might be removed entirely. */
typedef struct
{
	int output_page_numbers;
	int output_sheet_names;
	int output_cell_markers;
	int output_cell_row_markers;
	int output_cell_names;
	int output_formatting;
	int output_filenames;
	int output_errors;
}
fz_office_to_html_opts;

typedef struct
{
	int col1, col2, row1, row2;
}
merge_cell;

typedef struct
{
	fz_office_to_html_opts opts;

	fz_output *out;

	int page;

	/* State for if we are parsing a sheet. */
	/* The last column label we have to send. */
	char *label;
	/* Columns are numbered from 1. */
	/* The column we are at. */
	int col_at;
	/* The column we last signalled. If this is 0, then we haven't
	 * even started a row yet. */
	int col_signalled;
	/* Rows are numbered from 1. */
	/* the row we are at*/
	int row_at;

	merge_cell **merge_cells;
	int merge_len;

	/* If we are currently processing a spreadsheet, store the current
	 * sheets name here. */
	const char *sheet_name;

	int shared_string_max;
	int shared_string_len;
	char **shared_strings;

	int footnotes_max;
	char **footnotes;

	char *title;
} doc_info;

static char *lookup_rel(fz_context *ctx, fz_xml *rels, const char *id);
static char *make_absolute_path(fz_context *ctx, const char *abs, const char *rel);
static char *make_rel_name(fz_context *ctx, const char *file);

static void
doc_escape(fz_context *ctx, fz_output *output, const char *str_)
{
	const unsigned char *str = (const unsigned char *)str_;
	int c;

	if (!str)
		return;

	while ((c = *str++) != 0)
	{
		if (c == '&')
		{
			fz_write_string(ctx, output, "&amp;");
		}
		else if (c == '<')
		{
			fz_write_string(ctx, output, "&lt;");
		}
		else if (c == '>')
		{
			fz_write_string(ctx, output, "&gt;");
		}
		else
		{
			/* We get utf-8 in, just parrot it out again. */
			fz_write_byte(ctx, output, c);
		}
	}
}

static void
show_text(fz_context *ctx, fz_xml *top, doc_info *info)
{
	fz_xml *pos = top;
	fz_xml *next;

	while (pos)
	{
		doc_escape(ctx, info->out, fz_xml_text(pos));

		if (fz_xml_is_tag(pos, "lineBreak"))
		{
			fz_write_string(ctx, info->out, "\n");
		}
		else if (fz_xml_is_tag(pos, "tab"))
		{
			fz_write_string(ctx, info->out, "\t");
		}
		else if (fz_xml_is_tag(pos, "lastRenderedPageBreak"))
		{
			info->page++;
		}

		/* Always try to move down. */
		next = fz_xml_down(pos);
		if (next)
		{
			/* We can move down, easy! */
			pos = next;
			continue;
		}

		if (pos == top)
			break;

		/* We can't move down, try moving to next. */
		next = fz_xml_next(pos);
		if (next)
		{
			/* We can move to next, easy! */
			pos = next;
			continue;
		}

		/* If we can't go down, or next, pop up until we
		 * find somewhere we can go next from. */
		while (1)
		{
			/* OK. So move up. */
			pos = fz_xml_up(pos);
			/* Check for hitting the top. */
			if (pos == top)
				pos = NULL;
			if (pos == NULL)
				break;
			/* We've returned to a node. See if it's a 'p'. */
			if (fz_xml_is_tag(pos, "p"))
			{
				fz_write_string(ctx, info->out, "\n");
			}
			next = fz_xml_next(pos);
			if (next)
			{
				pos = next;
				break;
			}
		}
	}
}

static void
show_footnote(fz_context *ctx, fz_xml *v, doc_info *info)
{
	int n = fz_atoi(fz_xml_att(v, "w:id"));

	if (n < 0 || n >= info->footnotes_max)
		return;

	if (info->footnotes[n] == NULL ||
		info->footnotes[n][0] == 0)
		return;

	/* Then send the strings. */
	doc_escape(ctx, info->out, info->footnotes[n]);
}

static void
process_image(fz_context *ctx, doc_info *info, char *rid, fz_xml *rels, fz_archive *arch, const char *file)
{
	if (!rels) return;

	char *iafile = NULL;
	fz_buffer *buf = NULL;
	fz_image *img = NULL;

	fz_try(ctx)
	{
		char *ifile = lookup_rel(ctx, rels, rid);
		if (!ifile) return;

		iafile = make_absolute_path(ctx, file, ifile);
		buf = fz_read_archive_entry(ctx, arch, iafile);
		img = fz_new_image_from_buffer(ctx, buf);
		fz_write_string(ctx, info->out, "<img loading=\"lazy\" src=\"");
		fz_write_image_as_data_uri(ctx, info->out, img);
		fz_write_string(ctx, info->out, "\"");
	}
	fz_always(ctx)
	{
		fz_free(ctx, iafile);
		fz_drop_buffer(ctx, buf);
		fz_drop_image(ctx, img);
	}
	fz_catch(ctx)
	{
		// fz_rethrow(ctx);
	}
}

static void
process_doc_stream(fz_context *ctx, fz_xml *xml, doc_info *info, int do_pages, fz_xml *rels, fz_archive *arch, const char *file)
{
	fz_xml *pos;
	fz_xml *next;
	const char *paragraph_style = NULL;
	const char *inline_style = NULL;
	int is_image = 0;
	int grid_span = 0;

#ifdef DEBUG_OFFICE_TO_HTML
	fz_write_printf(ctx, fz_stddbg(ctx), "process_doc_stream:\n");
	fz_output_xml(ctx, fz_stddbg(ctx), xml, 0);
#endif

	/* First off, see if we can do page numbers. */
	if (do_pages)
	{
		pos = fz_xml_find_dfs(xml, "lastRenderedPageBreak", NULL, NULL);
		if (pos)
		{
			/* We *can* do page numbers, so start here. */
			fz_write_string(ctx, info->out, "<div id=\"page1\">\n");
			info->page = 1;
		}
	}

	/* Now walk the tree for real. */
	pos = xml;
	while (pos)
	{
		/* When we arrive on a node, check if it's a 't'. */
		if (fz_xml_is_tag(pos, "t"))
		{
			show_text(ctx, pos, info);
			/* Do NOT go down, we've already dealt with that. */
		}
		else if (fz_xml_is_tag(pos, "br"))
		{
			if (paragraph_style && strcmp(paragraph_style, "pre"))
			{
				fz_write_printf(ctx, info->out, "<br/>\n");
			}
			else
			{
				fz_write_printf(ctx, info->out, "\n");
			}
		}
		else if (fz_xml_is_tag(pos, "footnoteReference"))
		{
			show_footnote(ctx, pos, info);
			/* Do NOT go down, we've already dealt with that. */
		}
		else if (fz_xml_is_tag(pos, "tabs"))
		{
			/* Don't walk through tabs, or we will hit lots of 'tab' entries and
			 * output incorrect information. */
		}
		else if (fz_xml_is_tag(pos, "pStyle"))
		{
			/* Should prob fix fz_xml_*() to strip namespace prefix
			from attributes, to match what it does for tag names.
			*/
			paragraph_style = fz_xml_att(pos, "w:val");
			if (paragraph_style)
			{
				if (!strcmp(paragraph_style, "BodyText"))
					paragraph_style = NULL;
				else if (!strcmp(paragraph_style, "Heading1"))
					paragraph_style = "h1";
				else if (!strcmp(paragraph_style, "Heading2"))
					paragraph_style = "h2";
				else if (!strcmp(paragraph_style, "Heading3"))
					paragraph_style = "h3";
				else if (!strcmp(paragraph_style, "Heading4"))
					paragraph_style = "h4";
				else if (!strcmp(paragraph_style, "Heading5"))
					paragraph_style = "h5";
				else if (!strcmp(paragraph_style, "Heading6"))
					paragraph_style = "h6";
				else if (!strcmp(paragraph_style, "SourceCode"))
					paragraph_style = "pre";
				else
					paragraph_style = NULL;

				if (paragraph_style)
					fz_write_printf(ctx, info->out, "<%s>", paragraph_style);
			}
		}
		else if (fz_xml_is_tag(pos, "rStyle"))
		{
			inline_style = fz_xml_att(pos, "w:val");
			if (inline_style)
			{
				if (!strcmp(inline_style, "VerbatimChar"))
					inline_style = "tt";
				else
				{
					if (0)
						fz_write_printf(ctx, info->out, "<!-- %s -->", inline_style);
					inline_style = NULL;
				}
				if (inline_style)
					fz_write_printf(ctx, info->out, "<%s>", inline_style);
			}
		}
		else
		{
			fz_xml *down;
			if (fz_xml_is_tag(pos, "lineBreak"))
			{
				fz_write_string(ctx, info->out, "\n");
			}
			else if (fz_xml_is_tag(pos, "p"))
			{
				fz_write_string(ctx, info->out, "<p>");
			}
			else if (fz_xml_is_tag(pos, "tab"))
			{
				fz_write_string(ctx, info->out, "\t");
			}
			else if (fz_xml_is_tag(pos, "tbl"))				// w:tbl
			{
				fz_write_string(ctx, info->out, "<table>\n");
			}
			else if (fz_xml_is_tag(pos, "tr"))				// w:tr
			{
				fz_write_string(ctx, info->out, "<tr>\n");
			}
			else if (fz_xml_is_tag(pos, "tc"))				// w:tc
			{
				fz_write_string(ctx, info->out, "<td");
			}
			else if (fz_xml_is_tag(pos, "gridSpan"))		// w:gridSpan
			{
				char *gspan = fz_xml_att(pos, "w:val");
				if (gspan) {
					grid_span = fz_atoi(gspan);
					if (grid_span > 1) {
						fz_write_string(ctx, info->out, " span=\"restart\"");
						grid_span--;
					}
				}
			}
			else if (fz_xml_is_tag(pos, "vMerge"))			// w:vMerge
			{
				char *vMerge = fz_xml_att(pos, "w:val");
				if (vMerge && !strcmp(vMerge, "restart"))
					fz_write_string(ctx, info->out, " merge=\"restart\"");
				else
					fz_write_string(ctx, info->out, " merge=\"continue\"");
			}
			else if (fz_xml_is_tag(pos, "hMerge"))			// w:hMerge
			{
				char *hMerge = fz_xml_att(pos, "w:val");
				if (hMerge && !strcmp(hMerge, "restart"))
					fz_write_string(ctx, info->out, " span=\"restart\"");
				else
					fz_write_string(ctx, info->out, " span=\"continue\"");
			}
			else if (fz_xml_is_tag(pos, "blip"))			// a:blip
			{
				char *rid = fz_xml_att(pos, "r:embed");
				is_image = 1;
				process_image(ctx, info, rid, rels, arch, file);
			}
			else if (fz_xml_is_tag(pos, "imagedata"))		// v:imagedata
			{
				char *rid = fz_xml_att(pos, "r:id");
				is_image = 1;
				process_image(ctx, info, rid, rels, arch, file);
			}
			else if (fz_xml_is_tag(pos, "xfrm"))			// a:xfrm, blip prop
			{
				if (is_image) {
					is_image = 0;
					char *rot = fz_xml_att(pos, "rot");
					if (rot) {
						fz_write_string(ctx, info->out, " rotate-angle=\"");
						fz_write_string(ctx, info->out, rot);
						fz_write_string(ctx, info->out, "\"");
					}
					fz_write_string(ctx, info->out, ">\n");
				}
			}
			else if (do_pages && fz_xml_is_tag(pos, "lastRenderedPageBreak"))
			{
				if (info->page)
					fz_write_string(ctx, info->out, "\n</div>\n");
				info->page++;
				fz_write_printf(ctx, info->out, "<div id=\"page%d\">\n", info->page);
			}
			/* Try to move down. */
			down = fz_xml_down(pos);
			if (down)
			{
				/* We can move down, easy! */
				pos = down;
				continue;
			}
		}
		/* Try moving to next. */
		next = fz_xml_next(pos);
		if (next)
		{
			/* We can move to next, easy! */
			pos = next;
			continue;
		}

		/* If we can't go down, or next, pop up until we
		 * find somewhere we can go next from. */
		while (1)
		{
			/* OK. So move up. */
			pos = fz_xml_up(pos);
			/* Check for hitting the top. */
			if (pos == NULL)
				break;
			/* We've returned to a node. See if it's a 'p'. */
			if (fz_xml_is_tag(pos, "p"))
			{
				if (paragraph_style)
				{
					fz_write_printf(ctx, info->out, "</%s>", paragraph_style);
					paragraph_style = NULL;
				}
				fz_write_string(ctx, info->out, "</p>\n");
			}
			else if (fz_xml_is_tag(pos, "r"))
			{
				/* Seems to be pseudo-close for rStyle. */
				if (inline_style)
				{
					fz_write_printf(ctx, info->out, "</%s>", inline_style);
					inline_style = NULL;
				}
			}
			else if (fz_xml_is_tag(pos, "shape"))		// v:shape, closing imagedata
			{
				if (is_image) {
					is_image = 0;
					fz_write_string(ctx, info->out, ">\n");
				}
			}
			else if (fz_xml_is_tag(pos, "tcPr"))		// w:tcPr
			{
				fz_write_string(ctx, info->out, ">\n");
			}
			else if (fz_xml_is_tag(pos, "tc"))			// w:tc
			{
				fz_write_string(ctx, info->out, "</td>\n");
				if (grid_span > 0) {
					fz_write_string(ctx, info->out, "<td span=\"continue\"></td>\n");
					grid_span--;
				}
			}
			else if (fz_xml_is_tag(pos, "tr"))			// w:tr
			{
				fz_write_string(ctx, info->out, "</tr>\n");
			}
			else if (fz_xml_is_tag(pos, "tbl"))			// w:tbl
			{
				fz_write_string(ctx, info->out, "</table>\n");
			}
			next = fz_xml_next(pos);
			if (next)
			{
				pos = next;
				break;
			}
		}
	}

	if (do_pages && info->page)
		fz_write_string(ctx, info->out, "\n</div>\n");
}

static void
process_item(fz_context *ctx, fz_archive *arch, const char *file, doc_info *info, int do_pages)
{
	char *file_rels;
	fz_xml *rels = NULL;

	if (file == NULL)
		return;

	file_rels = make_rel_name(ctx, file);

	fz_var(rels);

	rels = fz_parse_xml_archive_entry(ctx, arch, file_rels, 0);
	fz_xml *xml = fz_parse_xml_archive_entry(ctx, arch, file, 1);

	fz_try(ctx)
		process_doc_stream(ctx, xml, info, do_pages, rels, arch, file);
	fz_always(ctx)
	{
		fz_drop_xml(ctx, xml);
		fz_drop_xml(ctx, rels);
		fz_free(ctx, file_rels);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static void
process_rootfile(fz_context *ctx, fz_archive *arch, const char *file, doc_info *info)
{
	fz_xml *xml = fz_parse_xml_archive_entry(ctx, arch, file, 0);

	fz_try(ctx)
	{
		/* FIXME: Should really search for these just inside 'spine'. */
		fz_xml *pos = fz_xml_find_dfs(xml, "itemref", NULL, NULL);
		while (pos)
		{
			char *idref = fz_xml_att(pos, "idref");
			fz_xml *item = fz_xml_find_dfs(xml, "item", "id", idref);
			while (item)
			{
				char *type = fz_xml_att(item, "media-type");
				char *href = fz_xml_att(item, "href");
				if (type && href && !strcmp(type, "application/xml"))
				{
					process_item(ctx, arch, href, info, 1);
				}
				item = fz_xml_find_next_dfs(pos, "item", "id", idref);
			}
			pos = fz_xml_find_next_dfs(pos, "itemref", NULL, NULL);
		}
	}
	fz_always(ctx)
		fz_drop_xml(ctx, xml);
	fz_catch(ctx)
		fz_rethrow(ctx);
}

/* XLSX support */
static char *
make_rel_name(fz_context *ctx, const char *file)
{
	size_t z = strlen(file);
	char *s = fz_malloc(ctx, z + 12);
	char *t;
	const char *p;
	const char *slash = file;

	for (p = file; *p != 0; p++)
		if (*p == '/')
			slash = p+1;

	t = s;
	if (slash != file)
	{
		memcpy(t, file, slash - file);
		t += slash - file;
	}
	memcpy(t, "_rels/", 6);
	t += 6;
	memcpy(t, file + (slash - file), z - (slash - file));
	t += z - (slash - file);
	memcpy(t, ".rels", 6);

	return s;
}

static char *lookup_rel(fz_context *ctx, fz_xml *rels, const char *id)
{
	fz_xml *pos;

	if (id == NULL)
		return NULL;

	pos = fz_xml_find_dfs(rels, "Relationship", NULL, NULL);
	while (pos)
	{
		char *id2 = fz_xml_att(pos, "Id");

		if (id2 && !strcmp(id, id2))
			return fz_xml_att(pos, "Target");

		pos = fz_xml_find_next_dfs(pos, "Relationship", NULL, NULL);
	}

	return NULL;
}

static char *get_merge_str(doc_info *info) {
	int col = info->col_signalled;
	int row = info->row_at;

	for (int i = 0; i < info->merge_len; i++) {
		merge_cell* mc = info->merge_cells[i];
		if (col >= mc->col1 && col <= mc->col2 && row >= mc->row1 && row <= mc->row2) {
			if (col > mc->col1)
				return " span=\"continue\"";
			if (row > mc->row1) {
				if (mc->col1 < mc->col2)
					return " span=\"restart\" merge=\"continue\"";
				else
					return " merge=\"continue\"";
			}
			if (mc->col1 < mc->col2 && mc->row1 < mc->row2)
				return " span=\"restart\" merge=\"restart\"";
			if (mc->col1 < mc->col2)
				return " span=\"restart\"";
			if (mc->row1 < mc->row2)
				return " merge=\"restart\"";
		}
	}
	return "";
}

static void
send_cell_formatting(fz_context *ctx, doc_info *info)
{
	if (info->col_signalled == 0)
	{
		fz_write_string(ctx, info->out, "<tr>\n");
	}

	/* Send the label */
	while (info->col_signalled < info->col_at)
	{
		if (info->col_signalled > 0)
			fz_write_string(ctx, info->out, "</td>");
		info->col_signalled++;
		if (info->col_signalled < info->col_at)
			fz_write_printf(ctx, info->out, "<td%s>", get_merge_str(info));
	}
	if (info->sheet_name && info->sheet_name[0])
		fz_write_printf(ctx, info->out, "<td id=\"%s!%s\"%s>", info->sheet_name, info->label, get_merge_str(info));
	else
		fz_write_printf(ctx, info->out, "<td id=\"%s\"%s>", info->label, get_merge_str(info));
}

static void
show_shared_string(fz_context *ctx, fz_xml *v, doc_info *info)
{
	const char *t = fz_xml_text(fz_xml_down(v));
	int n = fz_atoi(t);

	if (n < 0 || n >= info->shared_string_len)
		return;

	if (info->shared_strings[n] == NULL ||
		info->shared_strings[n][0] == 0)
		return;

	send_cell_formatting(ctx, info);
	/* Then send the strings. */
	doc_escape(ctx, info->out, info->shared_strings[n]);
}

static int
col_from_label(const char *label, int *row)
{
	int col = 0;
	int len = 26;
	int base = 0;

	/* If we can't read the column, return 0. */
	if (label == NULL || *label < 'A' || *label > 'Z')
		return 0;

	/*	Each section (A-Z, AA-ZZ, AAA-ZZZ etc) is of len 'len', and starts
	 *	at base index 'base'. Each section is 26 times as long, and starts
	 *	at base + len from the previous section.
	 *
	 *	A:	col = 26 * 0 + 0 + 0
	 *	AA:	col = (26 * 0 + 0 + 0) * 26 + 0 + 26 = 26
	 *	AAA:	col = (((26 * 0 + 0 + 0) * 26 + 0 + 26)*26 + 0 + 26*26 = 26 + 26 * 26
	 */
	do
	{
		col = 26 * col + (*label++) - 'A' + base;
		base += len;
		len *= 26;
	}
	while (*label >= 'A' && *label <= 'Z');

	if (row)
		*row = fz_atoi(label);

	return col+1;
}

static void
show_cell_text(fz_context *ctx, fz_xml *top, doc_info *info)
{
	fz_xml *pos = top;
	fz_xml *next;

	if (!pos)
	{
		send_cell_formatting(ctx, info);
		doc_escape(ctx, info->out, " ");
		return;
	}

	while (pos)
	{
		char *text = fz_xml_text(pos);

		if (text)
		{
			send_cell_formatting(ctx, info);
			doc_escape(ctx, info->out, text);
		}

		/* Always try to move down. */
		next = fz_xml_down(pos);
		if (next)
		{
			/* We can move down, easy! */
			pos = next;
			continue;
		}

		if (pos == top)
			break;

		/* We can't move down, try moving to next. */
		next = fz_xml_next(pos);
		if (next)
		{
			/* We can move to next, easy! */
			pos = next;
			continue;
		}

		/* If we can't go down, or next, pop up until we
		 * find somewhere we can go next from. */
		while (1)
		{
			/* OK. So move up. */
			pos = fz_xml_up(pos);
			/* Check for hitting the top. */
			if (pos == top)
				pos = NULL;
			if (pos == NULL)
				break;
			next = fz_xml_next(pos);
			if (next)
			{
				pos = next;
				break;
			}
		}
	}
}

static void
arrived_at_cell(fz_context *ctx, doc_info *info, const char *label)
{
	int col;

	/* If we have a label queued, and no label is given here, then we're
	 * processing a 'cell' callback after having had a 'cellname'
	 * callback. So don't signal it twice! */
	if (label == NULL && info->label)
		return;

	col = label ? col_from_label(label, NULL) : 0;

	fz_free(ctx, info->label);
	info->label = NULL;
	info->label = label ? fz_strdup(ctx, label) : NULL;
	info->col_at = col;
}

static void
show_cell(fz_context *ctx, fz_xml *cell, doc_info *info)
{
	char *t = fz_xml_att(cell, "t");
	fz_xml *v = fz_xml_find_down(cell, "v");
	const char *r = fz_xml_att(cell, "r");

	arrived_at_cell(ctx, info, r);
	if (t && t[0] == 's' && t[1] == 0)
		show_shared_string(ctx, v, info);
	else
		show_cell_text(ctx, v, info);
}

static void add_blank_cell(fz_context *ctx, doc_info *info, int colnum) {
	if (info->col_signalled < colnum) {
		info->col_at = colnum;
		show_cell_text(ctx, NULL, info);
	}
}

static void
new_row(fz_context *ctx, doc_info *info, int colnum)
{
	if (info->col_signalled)
	{
		add_blank_cell(ctx, info, colnum);
		/* We've sent at least one cell. So need to close the
		 * td and tr */
		fz_write_string(ctx, info->out, "</td>\n</tr>\n");
	}
	else
	{
		/* We've not sent anything for this row. Keep the counts
		 * correct. */
		fz_write_string(ctx, info->out, "<tr></tr>\n");
	}
	info->col_at = 1;
	info->row_at++;
	info->col_signalled = 0;
	fz_free(ctx, info->label);
	info->label = NULL;
}

static void getMergeInfo(fz_context *ctx, fz_xml *xml, doc_info *info) {
	fz_xml *pos = xml;
	fz_xml *next;
	info->merge_cells = NULL;
	info->merge_len = 0;

	fz_try(ctx)
	{
		int i = 0;
		while (pos)
		{
			if (fz_xml_is_tag(pos, "mergeCells"))
			{
				char *count = fz_xml_att(pos, "count");
				if (count)
				{
					info->merge_len = fz_atoi(count);
					if (info->merge_len)
						info->merge_cells = fz_malloc_array(ctx, info->merge_len, merge_cell*);
				}
			}
			if (fz_xml_is_tag(pos, "mergeCell"))
			{
				char *ref = fz_xml_att(pos, "ref");
				if (ref && i < info->merge_len) {
					merge_cell *mc = fz_malloc_struct(ctx, merge_cell);
					char *token = strtok(ref, ":");
					mc->col1 = col_from_label(token, &(mc->row1));
					token = strtok(NULL, ":");
					mc->col2 = col_from_label(token, &(mc->row2));
					info->merge_cells[i++] = mc;
				}
			}
			else
			{
				next = fz_xml_down(pos);
				if (next)
				{
					pos = next;
					continue;
				}
			}
			next = fz_xml_next(pos);
			if (next)
			{
				pos = next;
				continue;
			}
			while (1)
			{
				pos = fz_xml_up(pos);
				if (pos == NULL)
					break;
				next = fz_xml_next(pos);
				if (next)
				{
					pos = next;
					break;
				}
			}
		}
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static void
process_sheet(fz_context *ctx, fz_archive *arch, const char *name, const char *file, doc_info *info)
{
	fz_xml *xml = fz_parse_xml_archive_entry(ctx, arch, file, 1);

#ifdef DEBUG_OFFICE_TO_HTML
	fz_write_printf(ctx, fz_stddbg(ctx), "process_sheet:\n");
	fz_output_xml(ctx, fz_stddbg(ctx), xml, 0);
#endif

	fz_write_printf(ctx, info->out, "<table id=\"%s\">\n", name);

	info->sheet_name = name;
	info->col_at = 0;
	info->row_at = 1;
	info->col_signalled = 0;

	int colnum = 0;

	fz_try(ctx)
	{
		getMergeInfo(ctx, xml, info);

		fz_xml *pos = xml;
		fz_xml *next;

		while (pos)
		{
			/* When we arrive on a node, check if it's a cell. */
			if (fz_xml_is_tag(pos, "c"))
			{
				show_cell(ctx, pos, info);
				/* Do NOT go down, we've already dealt with that. */
			}
			else if (fz_xml_is_tag(pos, "col"))
			{
				colnum++;
			}
			else
			{
				/* Try to move down. */
				next = fz_xml_down(pos);
				if (next)
				{
					/* We can move down, easy! */
					pos = next;
					continue;
				}
			}
			/* Try moving to next. */
			next = fz_xml_next(pos);
			if (next)
			{
				/* We can move to next, easy! */
				pos = next;
				continue;
			}

			/* If we can't go down, or next, pop up until we
			 * find somewhere we can go next from. */
			while (1)
			{
				/* OK. So move up. */
				pos = fz_xml_up(pos);
				/* Check for hitting the top. */
				if (pos == NULL)
					break;

				/* We've returned to a node. See if it's a 'row'. */
				if (fz_xml_is_tag(pos, "row"))
					new_row(ctx, info, colnum);

				next = fz_xml_next(pos);
				if (next)
				{
					pos = next;
					break;
				}
			}
		}
		if (info->col_signalled) {
			add_blank_cell(ctx, info, colnum);
			fz_write_printf(ctx, info->out, "</td>\n</tr>\n");
		}
		fz_write_printf(ctx, info->out, "</table>\n");
	}
	fz_always(ctx) {
		fz_drop_xml(ctx, xml);
		for (int i = 0; i< info->merge_len; ++i)
			fz_free(ctx, info->merge_cells[i]);
		fz_free(ctx, info->merge_cells);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static void
process_slide(fz_context *ctx, fz_archive *arch, const char *file, doc_info *info)
{
	fz_write_printf(ctx, info->out, "<div id=\"slide%d\">\n", info->page++);
	process_item(ctx, arch, file, info, 0);
	fz_write_printf(ctx, info->out, "</div>\n");
}

static char *
make_absolute_path(fz_context *ctx, const char *abs, const char *rel)
{
	const char *a = abs;
	const char *aslash = a;
	size_t z1, z2;
	char *s;

	if (rel == NULL)
		return NULL;
	if (abs == NULL || *rel == '/')
		return fz_strdup(ctx, rel);

	for (a = abs; *a != 0; a++)
		if (*a == '/')
			aslash = a+1;

	if (rel[0] == 0)
		fz_throw(ctx, FZ_ERROR_FORMAT, "Unresolvable path");

	z1 = aslash - abs;
	z2 = strlen(rel);
	s = fz_malloc(ctx, z1 + z2 + 1);
	if (z1)
		memcpy(s, abs, z1);
	memcpy(s+z1, rel, z2+1);

	return s;
}

static char *
collate_t_content(fz_context *ctx, fz_xml *top)
{
	char *val = NULL;
	fz_xml *next;
	fz_xml *pos = fz_xml_down(top);

	while (pos != top)
	{
		/* Capture all the 't' content. */
		if (fz_xml_is_tag(pos, "t"))
		{
			/* Remember the content. */
			char *s = fz_xml_text(fz_xml_down(pos));

			if (s == NULL)
			{
				/* Do nothing */
			}
			else if (val == NULL)
				val = fz_strdup(ctx, s);
			else
			{
				char *val2;
				size_t z1 = strlen(val);
				size_t z2 = strlen(s) + 1;
				fz_try(ctx)
				{
					val2 = fz_malloc(ctx, z1 + z2);
				}
				fz_catch(ctx)
				{
					fz_free(ctx, val);
					fz_rethrow(ctx);
				}
				memcpy(val2, val, z1);
				memcpy(val2 + z1, s, z2);
				fz_free(ctx, val);
				val = val2;
			}
			/* Do NOT go down, we've already dealt with that. */
		}
		else if (fz_xml_is_tag(pos, "rPr") || fz_xml_is_tag(pos, "rPh"))
		{
			/* We do not want the 't' content from within these. */
		}
		else
		{
			/* Try to move down. */
			next = fz_xml_down(pos);
			if (next)
			{
				/* We can move down, easy! */
				pos = next;
				continue;
			}
		}
		/* Try moving to next. */
		next = fz_xml_next(pos);
		if (next)
		{
			/* We can move to next, easy! */
			pos = next;
			continue;
		}

		/* If we can't go down, or next, pop up until we
		 * find somewhere we can go next from. */
		while (1)
		{
			/* OK. So move up. */
			pos = fz_xml_up(pos);
			/* Check for hitting the top. */
			if (pos == top)
				break;
			next = fz_xml_next(pos);
			if (next)
			{
				pos = next;
				break;
			}
		}
	}

	return val;
}

static fz_xml *
try_parse_xml_archive_entry(fz_context *ctx, fz_archive *arch, const char *filename, int preserve_white)
{
	if (!fz_has_archive_entry(ctx, arch, filename))
		return NULL;

	return fz_parse_xml_archive_entry(ctx, arch, filename, preserve_white);
}

static void
load_shared_strings(fz_context *ctx, fz_archive *arch, fz_xml *rels, doc_info *info, const char *file)
{
	fz_xml *pos = fz_xml_find_dfs(rels, "Relationship", "Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings");
	const char *ss_file = fz_xml_att(pos, "Target");
	char *resolved = NULL;
	fz_xml *xml = NULL;
	char *str = NULL;

	if (ss_file == NULL)
		return;

	fz_var(xml);
	fz_var(str);
	fz_var(resolved);

	fz_try(ctx)
	{
		fz_xml *pos;

		resolved = make_absolute_path(ctx, file, ss_file);
		xml = fz_parse_xml_archive_entry(ctx, arch, resolved, 1);

		pos = fz_xml_find_dfs(xml, "si", NULL, NULL);
		while (pos)
		{
			int n = info->shared_string_len;
			str = collate_t_content(ctx, pos);

			if (n == info->shared_string_max)
			{
				int max = info->shared_string_max;
				int newmax = max ? max * 2 : 1024;
				char **arr = fz_realloc(ctx, info->shared_strings, sizeof(*arr) * newmax);
				memset(&arr[max], 0, sizeof(*arr) * (newmax - max));
				info->shared_strings = arr;
				info->shared_string_max = newmax;
			}

			info->shared_strings[n] = str;
			str = NULL;
			info->shared_string_len++;
			pos = fz_xml_find_next_dfs(pos, "si", NULL, NULL);
		}
	}
	fz_always(ctx)
	{
		fz_drop_xml(ctx, xml);
		fz_free(ctx, resolved);
		fz_free(ctx, str);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static void
load_footnotes(fz_context *ctx, fz_archive *arch, fz_xml *rels, doc_info *info, const char *file)
{
	char *resolved = NULL;
	fz_xml *xml = NULL;
	char *str = NULL;

	fz_var(xml);
	fz_var(str);
	fz_var(resolved);

	fz_try(ctx)
	{
		fz_xml *pos;

		resolved = make_absolute_path(ctx, file, "footnotes.xml");
		xml = try_parse_xml_archive_entry(ctx, arch, resolved, 1);
		if (xml == NULL)
			break;

		pos = fz_xml_find_dfs(xml, "footnote", NULL, NULL);
		while (pos)
		{
			int n = fz_atoi(fz_xml_att(pos, "w:id"));

			str = collate_t_content(ctx, pos);

			if (str && n >= 0)
			{
				if (n >= info->footnotes_max)
				{
					int max = info->footnotes_max;
					int newmax = max ? max * 2 : 1024;
					char **arr;
					if (newmax < n)
						newmax = n+1;
					arr = fz_realloc(ctx, info->footnotes, sizeof(*arr) * newmax);
					memset(&arr[max], 0, sizeof(*arr) * (newmax - max));
					info->footnotes = arr;
					info->footnotes_max = newmax;
				}

				info->footnotes[n] = str;
				str = NULL;
			}
			pos = fz_xml_find_next_dfs(pos, "footnote", NULL, NULL);
		}
	}
	fz_always(ctx)
	{
		fz_drop_xml(ctx, xml);
		fz_free(ctx, resolved);
		fz_free(ctx, str);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static void
process_office_document(fz_context *ctx, fz_archive *arch, const char *file, doc_info *info)
{
	char *file_rels;
	fz_xml *xml = NULL;
	fz_xml *rels = NULL;
	char *resolved_rel = NULL;

	if (file == NULL)
		return;

	file_rels = make_rel_name(ctx, file);

	fz_var(resolved_rel);

	fz_var(rels);
	fz_var(xml);

	fz_try(ctx)
	{
		fz_xml *pos;

		rels = fz_parse_xml_archive_entry(ctx, arch, file_rels, 0);
		xml = fz_parse_xml_archive_entry(ctx, arch, file, 1);

		/* XLSX */
		pos = fz_xml_find_dfs(xml, "sheet", NULL, NULL);
		if (pos)
		{
			int first = 1;
			load_shared_strings(ctx, arch, rels, info, file);
			while (pos)
			{
				char *name = fz_xml_att(pos, "name");
				char *id = fz_xml_att(pos, "r:id");
				char *sheet = lookup_rel(ctx, rels, id);

				if (sheet)
				{
					if (first)
						fz_write_printf(ctx, info->out, "<pre>%s</pre>", name);
					else
						fz_write_printf(ctx, info->out, "<pre style=\"page-break-before:always\">%s</pre>", name);
					resolved_rel = make_absolute_path(ctx, file, sheet);
					process_sheet(ctx, arch, name, resolved_rel, info);
					fz_free(ctx, resolved_rel);
					resolved_rel = NULL;
				}
				pos = fz_xml_find_next_dfs(pos, "sheet", NULL, NULL);
				first = 0;
			}
			break;
		}

		/* Let's try it as a powerpoint */
		pos = fz_xml_find_dfs(xml, "sldId", NULL, NULL);
		if (pos)
		{
			int first = 1;
			while (pos)
			{
				char *id = fz_xml_att(pos, "r:id");
				char *sheet = lookup_rel(ctx, rels, id);

				if (sheet)
				{
					if (first)
						fz_write_printf(ctx, info->out, "<pre>Slide %d</pre>", info->page + 1);
					else
						fz_write_printf(ctx, info->out, "<pre style=\"page-break-before:always\">Slide %d</pre>", info->page + 1);
					resolved_rel = make_absolute_path(ctx, file, sheet);
					process_slide(ctx, arch, resolved_rel, info);
					fz_free(ctx, resolved_rel);
					resolved_rel = NULL;
				}
				pos = fz_xml_find_next_dfs(pos, "sldId", NULL, NULL);
				first = 0;
			}
			break;
		}

		/* Let's try it as word. */
		{
			load_footnotes(ctx, arch, rels, info, file);
			process_doc_stream(ctx, xml, info, 1, rels, arch, file);
		}
	}
	fz_always(ctx)
	{
		fz_drop_xml(ctx, xml);
		fz_drop_xml(ctx, rels);
		fz_free(ctx, resolved_rel);
		fz_free(ctx, file_rels);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static void
process_office_document_properties(fz_context *ctx, fz_archive *arch, const char *file, doc_info *info)
{
	fz_xml *xml = NULL;
	char *title;

	fz_var(xml);

	fz_try(ctx)
	{
		fz_xml *pos;

		xml = fz_parse_xml_archive_entry(ctx, arch, file, 1);

		pos = fz_xml_find_dfs(xml, "title", NULL, NULL);
		title = fz_xml_text(fz_xml_down(pos));
		if (title)
		{
			fz_write_string(ctx, info->out, "<title>");
			doc_escape(ctx, info->out, title);
			fz_write_string(ctx, info->out, "</title>");
		}
	}
	fz_always(ctx)
	{
		fz_drop_xml(ctx, xml);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static fz_buffer *
fz_office_to_html(fz_context *ctx, fz_html_font_set *set, fz_buffer *buffer_in, fz_archive *dir, const char *user_css, fz_office_to_html_opts *opts)
{
	fz_stream *stream = NULL;
	fz_archive *archive = NULL;
	fz_buffer *buffer_out = NULL;
	fz_xml *xml = NULL;
	fz_xml *pos = NULL;
	fz_xml *rels = NULL;
	const char *schema = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";
	const char *schema_props = "http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties";
	doc_info info = { 0 };
	int i;

	fz_var(archive);
	fz_var(stream);
	fz_var(buffer_out);
	fz_var(xml);
	fz_var(rels);

	if (opts)
		info.opts = *opts;

	fz_try(ctx)
	{
		if (buffer_in)
		{
			stream = fz_open_buffer(ctx, buffer_in);
			archive = fz_open_archive_with_stream(ctx, stream);
		}
		else
			archive = fz_keep_archive(ctx, dir);
		buffer_out = fz_new_buffer(ctx, 1024);
		info.out = fz_new_output_with_buffer(ctx, buffer_out);

		/* Is it an HWPX ?*/
		xml = try_parse_xml_archive_entry(ctx, archive, "META-INF/container.xml", 0);
		if (xml)
		{
			pos = fz_xml_find_dfs(xml, "rootfile", "media-type", "application/hwpml-package+xml");
			if (!pos)
				fz_throw(ctx, FZ_ERROR_FORMAT, "Archive not hwpx.");

			while (pos)
			{
				const char *file = fz_xml_att(pos, "full-path");
				process_rootfile(ctx, archive, file, &info);
				pos = fz_xml_find_next_dfs(pos, "rootfile", "media-type", "application/hwpml-package+xml");
			}
			break;
		}

		/* Try other types */
		{
			xml = try_parse_xml_archive_entry(ctx, archive, "_rels/.rels", 0);

			fz_write_string(ctx, info.out, "<html>\n");

			pos = fz_xml_find_dfs(xml, "Relationship", "Type", schema_props);
			if (pos)
			{
				const char *file = fz_xml_att(pos, "Target");
				fz_write_string(ctx, info.out, "<head>\n");
				process_office_document_properties(ctx, archive, file, &info);
				fz_write_string(ctx, info.out, "<style>\n");
				fz_write_string(ctx, info.out, "table{border:1px solid;}\n");
				fz_write_string(ctx, info.out, "td{border:1px solid;}\n");
				fz_write_string(ctx, info.out, "</style>\n");
				fz_write_string(ctx, info.out, "</head>\n");
			}

			fz_write_string(ctx, info.out, "<body>\n");
			pos = fz_xml_find_dfs(xml, "Relationship", "Type", schema);
			if (!pos)
				fz_throw(ctx, FZ_ERROR_FORMAT, "Archive not docx.");

			while (pos)
			{
				const char *file = fz_xml_att(pos, "Target");
				if (file)
					process_office_document(ctx, archive, file, &info);
				pos = fz_xml_find_next_dfs(pos, "Relationship", "Type", schema);
			}

			fz_write_string(ctx, info.out, "</body></html>\n");
		}
	}
	fz_always(ctx)
	{
		fz_drop_xml(ctx, rels);
		fz_drop_xml(ctx, xml);
		for (i = 0; i < info.shared_string_len; ++i)
			fz_free(ctx, info.shared_strings[i]);
		fz_free(ctx, info.shared_strings);
		for (i = 0; i < info.footnotes_max; ++i)
			fz_free(ctx, info.footnotes[i]);
		fz_free(ctx, info.footnotes);
		fz_drop_output(ctx, info.out);
		fz_drop_archive(ctx, archive);
		fz_drop_stream(ctx, stream);
	}
	fz_catch(ctx)
	{
		fz_drop_buffer(ctx, buffer_out);
		fz_rethrow(ctx);
	}

#ifdef DEBUG_OFFICE_TO_HTML
	{
		unsigned char *storage;
		size_t len = fz_buffer_storage(ctx, buffer_out, &storage);
		fz_write_printf(ctx, fz_stddbg(ctx), "fz_office_to_html: Output buffer, len=%zd:\n", len);
		fz_write_buffer(ctx, fz_stddbg(ctx), buffer_out);
	}
#endif

	return buffer_out;
}

/* Office document handler */

static fz_buffer *
office_to_html(fz_context *ctx, fz_html_font_set *set, fz_buffer *buf, fz_archive *zip, const char *user_css)
{
	fz_office_to_html_opts opts = { 0 };

	return fz_office_to_html(ctx, set, buf, zip, user_css, &opts);
}

static const fz_htdoc_format_t fz_htdoc_office =
{
	"Office document",
	office_to_html,
	0, 1, 0
};

static fz_document *
office_open_document(fz_context *ctx, const fz_document_handler *handler, fz_stream *file, fz_stream *accel, fz_archive *zip, void *state)
{
	return fz_htdoc_open_document_with_stream_and_dir(ctx, file, zip, &fz_htdoc_office);
}

static const char *office_extensions[] =
{
	"docx",
	"xlsx",
	"pptx",
	"hwpx",
	NULL
};

static const char *office_mimetypes[] =
{
	// DOCX
	"application/vnd.openxmlformats-officedocument.wordprocessingml.document",
	// XLSX
	"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
	// PPTX
	"application/vnd.openxmlformats-officedocument.presentationml.presentation",
	// HWPX
	"application/haansofthwpx",
	"application/vnd.hancom.hwpx",
	NULL
};

/* We are only ever 75% sure here, to allow a 'better' handler, such as sodochandler
 * to override us by returning 100. */
static int
office_recognize_doc_content(fz_context *ctx, const fz_document_handler *handler, fz_stream *stream, fz_archive *zip, void **state, fz_document_recognize_state_free_fn **free_state)
{
	fz_archive *arch = NULL;
	int ret = 0;
	fz_xml *xml = NULL;

	if (state)
		*state = NULL;
	if (free_state)
		*free_state = NULL;

	fz_var(arch);
	fz_var(ret);
	fz_var(xml);

	fz_try(ctx)
	{
		if (stream)
		{
			arch = fz_try_open_archive_with_stream(ctx, stream);
			if (arch == NULL)
				break;
		}
		else
			arch = fz_keep_archive(ctx, zip);

		xml = fz_try_parse_xml_archive_entry(ctx, arch, "META-INF/container.xml", 0);
		if (xml)
		{
			if (fz_xml_find_dfs(xml, "rootfile", "media-type", "application/hwpml-package+xml"))
				ret = 100; /* HWPX */
			break;
		}
		xml = fz_try_parse_xml_archive_entry(ctx, arch, "_rels/.rels", 0);
		if (xml)
		{
			if (fz_xml_find_dfs(xml, "Relationship", "Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument"))
			{
				ret = 100; /* DOCX | PPTX | XLSX */
			}
			break;
		}
	}
	fz_always(ctx)
	{
		fz_drop_xml(ctx, xml);
		fz_drop_archive(ctx, arch);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);

	return ret;
}

fz_document_handler office_document_handler =
{
	NULL,
	office_open_document,
	office_extensions,
	office_mimetypes,
	office_recognize_doc_content
};
