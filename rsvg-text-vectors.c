/* vim: set sw=4: -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
   rsvg-text-vectors.c: Vector text handling routines for RSVG

   Copyright (C) 2004 Dom Lachowicz <cinamod@hotmail.com>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this program; if not, write to the
   Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
*/

#include <string.h>

#include "rsvg-private.h"
#include "rsvg-styles.h"
#include "rsvg-text.h"
#include "rsvg-shapes.h"
#include "rsvg-css.h"

#include <libart_lgpl/art_affine.h>
#include <libart_lgpl/art_render_mask.h>

#include <pango/pangoft2.h>

#include <ft2build.h>
#include FT_GLYPH_H
#include FT_OUTLINE_H

typedef struct _RsvgTextLayout RsvgTextLayout;

struct _RsvgTextLayout
{
	PangoLayout * layout;
	RsvgHandle  * ctx;
	TextAnchor anchor;
};

typedef struct _RenderCtx RenderCtx;

struct _RenderCtx
{
	GString      *path;
	gboolean      wrote;
	gdouble       offset_x;
	gdouble       offset_y;
	gdouble       position_x;
	gdouble       position_y;
};

typedef void (* RsvgTextRenderFunc) (PangoFont  *font,
									 PangoGlyph  glyph,
									 FT_Int32    load_flags,
									 FT_Matrix  *trafo,
									 gint        x,
									 gint        y,
									 gpointer    render_data);

#ifndef FT_GLYPH_FORMAT_OUTLINE
#define FT_GLYPH_FORMAT_OUTLINE ft_glyph_format_outline
#endif

#ifndef FT_LOAD_TARGET_MONO
#define FT_LOAD_TARGET_MONO FT_LOAD_MONOCHROME
#endif

/* TODO: stuff these into the RsvgHandle object, expose API for manipulating them */
#define ANTIALIAS_TEXT 0
#define HINT_TEXT 0
#define AUTO_HINT_TEXT 0

static RenderCtx *
rsvg_render_ctx_new (void)
{
	RenderCtx *ctx;
	
	ctx = g_new0 (RenderCtx, 1);
	ctx->path = g_string_new (NULL);
	
	return ctx;
}

static void
rsvg_render_ctx_free(RenderCtx *ctx)
{
	g_string_free(ctx->path, TRUE);
	g_free(ctx);
}

static void
rsvg_text_ft2_subst_func (FcPattern *pattern,
                          gpointer   data)
{
	RsvgHandle *ctx = (RsvgHandle *)data;

	(void)ctx;

	FcPatternAddBool (pattern, FC_HINTING, HINT_TEXT);
	FcPatternAddBool (pattern, FC_ANTIALIAS, ANTIALIAS_TEXT);
	FcPatternAddBool (pattern, FC_AUTOHINT, AUTO_HINT_TEXT);	
}

static PangoContext *
rsvg_text_get_pango_context (RsvgHandle *ctx)
{
	PangoContext    *context;
	PangoFT2FontMap *fontmap;
	
	fontmap = PANGO_FT2_FONT_MAP (pango_ft2_font_map_new ());
	
	pango_ft2_font_map_set_resolution (fontmap, ctx->dpi, ctx->dpi);
	
	pango_ft2_font_map_set_default_substitute (fontmap,
											   rsvg_text_ft2_subst_func,
											   ctx,
											   (GDestroyNotify) NULL);
	
	context = pango_ft2_font_map_create_context (fontmap);
	g_object_unref (fontmap);
	
	return context;
}

static void
rsvg_text_layout_free(RsvgTextLayout * layout)
{
	g_object_unref(G_OBJECT(layout->layout));
	g_free(layout);
}

static RsvgTextLayout *
rsvg_text_layout_new (RsvgHandle *ctx,
					  RsvgState *state,
					  const char *text)
{
	RsvgTextLayout * layout;
	PangoFontDescription *font_desc;
	
	if (ctx->pango_context == NULL)
		ctx->pango_context = rsvg_text_get_pango_context (ctx);
	
	if (state->lang)
		pango_context_set_language (ctx->pango_context,
									pango_language_from_string (state->lang));
	
	pango_context_set_base_dir (ctx->pango_context, state->text_dir);
	
	layout = g_new0 (RsvgTextLayout, 1);
	layout->layout = pango_layout_new (ctx->pango_context);
	layout->ctx = ctx;
	
	font_desc = pango_font_description_copy (pango_context_get_font_description (ctx->pango_context));
	
	if (state->font_family)
		pango_font_description_set_family_static (font_desc, state->font_family);
	
	pango_font_description_set_style (font_desc, state->font_style);
	pango_font_description_set_variant (font_desc, state->font_variant);
	pango_font_description_set_weight (font_desc, state->font_weight);
	pango_font_description_set_stretch (font_desc, state->font_stretch); 
	pango_font_description_set_size (font_desc, state->font_size * PANGO_SCALE / ctx->dpi * 72); 
	pango_layout_set_font_description (layout->layout, font_desc);
	pango_font_description_free (font_desc);
	
	if (text)
		pango_layout_set_text (layout->layout, text, -1);
	else
		pango_layout_set_text (layout->layout, NULL, 0);
	
	pango_layout_set_alignment (layout->layout, (state->text_dir == PANGO_DIRECTION_LTR || 
												 state->text_dir == PANGO_DIRECTION_TTB_LTR) ? 
								PANGO_ALIGN_LEFT : PANGO_ALIGN_RIGHT);
	
	layout->anchor = state->text_anchor;

	return layout;
}

static void
rsvg_text_layout_get_offsets (RsvgTextLayout *layout,
                              gint           *x,
                              gint           *y)
{
	PangoRectangle  ink;
	PangoRectangle  logical;
	
	pango_layout_get_pixel_extents (layout->layout, &ink, &logical);
	
	if (ink.width < 1 || ink.height < 1) {
		*x = *y = 0;
		return;
	}
	
	*x = MIN (ink.x, logical.x);
	*y = MIN (ink.y, logical.y);
}

static FT_Int32
rsvg_text_layout_render_flags (RsvgTextLayout *layout)
{
	gint flags = 0;
	
	if (ANTIALIAS_TEXT)
		flags |= FT_LOAD_NO_BITMAP;
	else
		flags |= FT_LOAD_TARGET_MONO;
	
	if (!HINT_TEXT)
		flags |= FT_LOAD_NO_HINTING;
	
	if (AUTO_HINT_TEXT)
		flags |= FT_LOAD_FORCE_AUTOHINT;	

	return flags;
}

static void
rsvg_text_vector_coords (RenderCtx       *ctx,
						 const FT_Vector *vector,
						 gdouble         *x,
						 gdouble         *y)
{
	*x = ctx->offset_x + (double)vector->x / 64. + ctx->position_x;
	*y = ctx->offset_y - (double)vector->y / 64. + ctx->position_y;
}

static gint
moveto (FT_Vector *to,
		gpointer   data)
{
	RenderCtx * ctx;
	gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
	gdouble x, y;
	
	ctx = (RenderCtx *)data;
	
	if (ctx->wrote)
		g_string_append(ctx->path, "Z ");
	else
		ctx->wrote = TRUE;

	g_string_append_c(ctx->path, 'M');
	
	rsvg_text_vector_coords(ctx, to, &x, &y);
	g_string_append(ctx->path, g_ascii_dtostr(buf, sizeof(buf), x));
	g_string_append_c(ctx->path, ',');
	g_string_append(ctx->path, g_ascii_dtostr(buf, sizeof(buf), y));
	g_string_append_c(ctx->path, ' ');

	return 0;
}

static gint
lineto (FT_Vector *to,
		gpointer   data)
{
	RenderCtx * ctx;
	gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
	gdouble x, y;
	
	ctx = (RenderCtx *)data;
	
	if (!ctx->wrote)
		return 0;

	g_string_append_c(ctx->path, 'L');
	
	rsvg_text_vector_coords(ctx, to, &x, &y);
	g_string_append(ctx->path, g_ascii_dtostr(buf, sizeof(buf), x));
	g_string_append_c(ctx->path, ',');
	g_string_append(ctx->path, g_ascii_dtostr(buf, sizeof(buf), y));
	g_string_append_c(ctx->path, ' ');

	return 0;
}

static gint
conicto (FT_Vector *ftcontrol,
		 FT_Vector *to,
		 gpointer   data)
{
	RenderCtx * ctx;
	gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
	gdouble x, y;
	
	ctx = (RenderCtx *)data;

	if (!ctx->wrote)
		return 0;

	g_string_append_c(ctx->path, 'Q');
	
	rsvg_text_vector_coords(ctx, ftcontrol, &x, &y);
	g_string_append(ctx->path, g_ascii_dtostr(buf, sizeof(buf), x));
	g_string_append_c(ctx->path, ',');
	g_string_append(ctx->path, g_ascii_dtostr(buf, sizeof(buf), y));
	
	rsvg_text_vector_coords(ctx, to, &x, &y);
	g_string_append_c(ctx->path, ' ');
	g_string_append(ctx->path, g_ascii_dtostr(buf, sizeof(buf), x));
	g_string_append_c(ctx->path, ',');
	g_string_append(ctx->path, g_ascii_dtostr(buf, sizeof(buf), y));
	g_string_append_c(ctx->path, ' ');

	return 0;
}

static gint
cubicto (FT_Vector *ftcontrol1,
		 FT_Vector *ftcontrol2,
		 FT_Vector *to,
		 gpointer   data)
{
	RenderCtx * ctx;
	gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
	gdouble x, y;
	
	ctx = (RenderCtx *)data;
	
	if (!ctx->wrote)
		return 0;

	g_string_append_c(ctx->path, 'C');
	
	rsvg_text_vector_coords(ctx, ftcontrol1, &x, &y);
	g_string_append(ctx->path, g_ascii_dtostr(buf, sizeof(buf), x));
	g_string_append_c(ctx->path, ',');
	g_string_append(ctx->path, g_ascii_dtostr(buf, sizeof(buf), y));
	
	rsvg_text_vector_coords(ctx, ftcontrol2, &x, &y);
	g_string_append_c(ctx->path, ' ');
	g_string_append(ctx->path, g_ascii_dtostr(buf, sizeof(buf), x));
	g_string_append_c(ctx->path, ',');
	g_string_append(ctx->path, g_ascii_dtostr(buf, sizeof(buf), y));
	
	rsvg_text_vector_coords(ctx, to, &x, &y);
	g_string_append_c(ctx->path, ' ');
	g_string_append(ctx->path, g_ascii_dtostr(buf, sizeof(buf), x));
	g_string_append_c(ctx->path, ',');
	g_string_append(ctx->path, g_ascii_dtostr(buf, sizeof(buf), y));
	g_string_append_c(ctx->path, ' ');	

	return 0;
}

static void
rsvg_text_layout_render_trafo (RsvgTextLayout *layout,
                               FT_Matrix      *trafo)
{
	RsvgState * state;

	state = rsvg_state_current(layout->ctx);
	if(0 /* state */) 
		{
			trafo->xx = state->affine[0] * 65536.0;
			trafo->xy = state->affine[1] * 65536.0;
			trafo->yx = state->affine[2] * 65536.0;
			trafo->yy = state->affine[3] * 65536.0;
		}
	else 
		{
			trafo->xx = 1 * 65536.0;
			trafo->xy = 0 * 65536.0;
			trafo->yx = 0 * 65536.0;
			trafo->yy = 1 * 65536.0;
		}
}

static void
rsvg_text_layout_render_glyphs (RsvgTextLayout     *layout,
								PangoFont          *font,
								PangoGlyphString   *glyphs,
								RsvgTextRenderFunc  render_func,
								gint                x,
								gint                y,
								gpointer            render_data)
{
	PangoGlyphInfo *gi;
	FT_Int32        flags;
	FT_Matrix       trafo;
	FT_Vector       pos;
	gint            i;
	gint            x_position = 0;
	
	flags = rsvg_text_layout_render_flags (layout);
	rsvg_text_layout_render_trafo (layout, &trafo);

	for (i = 0, gi = glyphs->glyphs; i < glyphs->num_glyphs; i++, gi++)
		{
			if (gi->glyph)
				{
					pos.x = x + x_position + gi->geometry.x_offset;
					pos.y = y + gi->geometry.y_offset;
					
					FT_Vector_Transform (&pos, &trafo);

					render_func (font, gi->glyph, flags, &trafo,
								 pos.x, pos.y,
								 render_data);
				}
			
			x_position += glyphs->glyphs[i].geometry.width;
		}
}

static void
rsvg_text_render_vectors (PangoFont     *font,
						  PangoGlyph     pango_glyph,
						  FT_Int32       flags,
						  FT_Matrix     *trafo,
						  gint           x,
						  gint           y,
						  gpointer       ud)
{
	static const FT_Outline_Funcs outline_funcs =
		{
			moveto,
			lineto,
			conicto,
			cubicto,
			0,
			0
		};
	
	FT_Face   face;
	FT_Glyph  glyph;
	
	RenderCtx *context = (RenderCtx *)ud;
	
	face = pango_ft2_font_get_face (font);

	FT_Load_Glyph (face, (FT_UInt) pango_glyph, flags);

	FT_Get_Glyph (face->glyph, &glyph);
	
	if (face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
		{
			FT_OutlineGlyph outline_glyph = (FT_OutlineGlyph) glyph;
			
			context->offset_x = (gdouble) x / PANGO_SCALE;
			context->offset_y = (gdouble) y / PANGO_SCALE - (int)face->size->metrics.ascender / 64;

			FT_Outline_Decompose (&outline_glyph->outline, &outline_funcs, context);			
		}
	
	FT_Done_Glyph (glyph);
}

static void
rsvg_text_layout_render_line (RsvgTextLayout     *layout,
							  PangoLayoutLine    *line,
							  RsvgTextRenderFunc  render_func,
							  gint                x,
							  gint                y,
							  gpointer            render_data)
{
	PangoRectangle  rect;
	GSList         *list;
	gint            x_off = 0;
	
	for (list = line->runs; list; list = list->next)
		{
			PangoLayoutRun *run = list->data;
			
			pango_glyph_string_extents (run->glyphs, run->item->analysis.font,
										NULL, &rect);
			rsvg_text_layout_render_glyphs (layout,
											run->item->analysis.font, run->glyphs,
											render_func,
											x + x_off, y,
											render_data);
			
			x_off += rect.width;
		}
}

static void
rsvg_text_layout_render (RsvgTextLayout     *layout,
						 RsvgTextRenderFunc  render_func,
						 gpointer            render_data)
{
	PangoLayoutIter *iter;
	gint             x, y;
	gint anchoroffset;

	rsvg_text_layout_get_offsets (layout, &x, &y);

	x *= PANGO_SCALE;
	y *= PANGO_SCALE;
	
	iter = pango_layout_get_iter (layout->layout);
	
	do
		{
			PangoRectangle   rect;
			PangoLayoutLine *line;
			gint             baseline;
			
			line = pango_layout_iter_get_line (iter);
			
			pango_layout_iter_get_line_extents (iter, NULL, &rect);
			baseline = pango_layout_iter_get_baseline (iter);
			
			if (layout->anchor == TEXT_ANCHOR_START)
				anchoroffset = 0;
			else if (layout->anchor == TEXT_ANCHOR_MIDDLE)
				anchoroffset = rect.width / 2;
			else
				anchoroffset = rect.width;
			
			rsvg_text_layout_render_line (layout, line,
										  render_func,
										  x + rect.x - anchoroffset,
										  y + baseline,
										  render_data);
		}
	while (pango_layout_iter_next_line (iter));
	
	pango_layout_iter_free (iter);
}

void 
rsvg_text_render_text (RsvgHandle *ctx,
					   RsvgState  *state,
					   const char *text,
					   const char *id,
					   gdouble x, 
					   gdouble y)
{
	RsvgTextLayout *layout;
	RenderCtx      *render;
	
	state->fill_rule = FILL_RULE_EVENODD;	
	state->has_fill_rule = TRUE;

	layout = rsvg_text_layout_new (ctx, state, text);
	render = rsvg_render_ctx_new ();

	render->position_x = x;
	render->position_y = y;

	rsvg_text_layout_render (layout, rsvg_text_render_vectors, 
							 (gpointer)render);

	if (render->wrote)
		g_string_append_c(render->path, 'Z');

#ifdef RSVG_TEXT_DEBUG
	fprintf(stderr, "%s\n", render->path->str);
#endif

	rsvg_handle_path (ctx, render->path->str, id);

	rsvg_render_ctx_free (render);
	rsvg_text_layout_free (layout);
}
