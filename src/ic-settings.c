/*
 *  ic-settings.c — conversion settings, profiles and ImageMagick command building
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 */

#include <config.h>

#include "ic-settings.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>

/* ------------------------------------------------------------------ */
/* Settings                                                             */
/* ------------------------------------------------------------------ */

void
ic_settings_copy_into (IcSettings *dst, const IcSettings *src)
{
	char *suffix = g_strdup (src->suffix ? src->suffix : "");
	g_free (dst->suffix);
	*dst = *src;
	dst->suffix = suffix;
}

void
ic_settings_clear (IcSettings *s)
{
	g_clear_pointer (&s->suffix, g_free);
}

gboolean
ic_settings_equal (const IcSettings *a, const IcSettings *b)
{
	return a->format == b->format &&
	       a->resize == b->resize &&
	       a->scale == b->scale &&
	       a->width == b->width &&
	       a->height == b->height &&
	       a->no_upscale == b->no_upscale &&
	       a->quality == b->quality &&
	       a->target_kb == b->target_kb &&
	       a->strip == b->strip &&
	       a->output == b->output &&
	       g_strcmp0 (a->suffix ? a->suffix : "", b->suffix ? b->suffix : "") == 0;
}

/* ------------------------------------------------------------------ */
/* Visual edits                                                         */
/* ------------------------------------------------------------------ */

void
ic_edit_reset (IcEdit *e)
{
	e->flip_h = e->flip_v = FALSE;
	e->rot90 = 0;
	e->fine = 0.0;
	e->cx = e->cy = 0.0;
	e->cw = e->ch = 1.0;
	e->auto_crop = FALSE;
}

gboolean
ic_edit_has_crop (const IcEdit *e)
{
	return e->cx > 1e-4 || e->cy > 1e-4 || e->cw < 1.0 - 1e-4 || e->ch < 1.0 - 1e-4;
}

gboolean
ic_edit_is_identity (const IcEdit *e)
{
	return !e->flip_h && !e->flip_v && e->rot90 == 0 &&
	       fabs (e->fine) < 1e-6 && !ic_edit_has_crop (e);
}

double
ic_edit_total_angle (const IcEdit *e)
{
	return e->rot90 * 90.0 + e->fine;
}

void
ic_edit_bbox (const IcEdit *e, double w, double h, double *bw, double *bh)
{
	double a = ic_edit_total_angle (e) * G_PI / 180.0;
	double c = fabs (cos (a)), s = fabs (sin (a));
	*bw = w * c + h * s;
	*bh = w * s + h * c;
}

void
ic_edit_auto_crop (IcEdit *e, double w, double h)
{
	double W = (e->rot90 % 2) ? h : w;
	double H = (e->rot90 % 2) ? w : h;
	double a = e->fine * G_PI / 180.0;
	double c = fabs (cos (a)), s = fabs (sin (a));
	double bw = W * c + H * s, bh = W * s + H * c;
	double k = MIN (W / (W * c + H * s), H / (W * s + H * c));

	e->cw = CLAMP (k * W / bw, 0.0, 1.0);
	e->ch = CLAMP (k * H / bh, 0.0, 1.0);
	e->cx = (1.0 - e->cw) / 2.0;
	e->cy = (1.0 - e->ch) / 2.0;
}

/* ------------------------------------------------------------------ */
/* Formats                                                              */
/* ------------------------------------------------------------------ */

static const char *format_ext[IC_FORMAT_N] = {
	NULL, "jpg", "png", "webp", "avif", "gif", "pdf"
};

const char *
ic_format_extension (IcFormat f)
{
	return (f >= 0 && f < IC_FORMAT_N) ? format_ext[f] : NULL;
}

const char *
ic_format_label (IcFormat f)
{
	switch (f) {
	case IC_FORMAT_KEEP: return _("Keep original");
	case IC_FORMAT_JPEG: return "JPEG";
	case IC_FORMAT_PNG:  return "PNG";
	case IC_FORMAT_WEBP: return "WebP";
	case IC_FORMAT_AVIF: return "AVIF";
	case IC_FORMAT_GIF:  return "GIF";
	case IC_FORMAT_PDF:  return "PDF";
	default:             return "?";
	}
}

static gboolean
is_jpeg_ext (const char *ext)
{
	return g_strcmp0 (ext, "jpg") == 0 || g_strcmp0 (ext, "jpeg") == 0 ||
	       g_strcmp0 (ext, "jpe") == 0 || g_strcmp0 (ext, "jfif") == 0;
}

gboolean
ic_extension_has_quality (const char *ext)
{
	return is_jpeg_ext (ext) || g_strcmp0 (ext, "webp") == 0 ||
	       g_strcmp0 (ext, "avif") == 0 || g_strcmp0 (ext, "heic") == 0 ||
	       g_strcmp0 (ext, "pdf") == 0;
}

gboolean
ic_extension_has_alpha (const char *ext)
{
	return !(is_jpeg_ext (ext) || g_strcmp0 (ext, "pdf") == 0 ||
	         g_strcmp0 (ext, "bmp") == 0);
}

gboolean
ic_extension_has_target_size (const char *ext)
{
	return is_jpeg_ext (ext) || g_strcmp0 (ext, "webp") == 0;
}

char *
ic_path_extension (const char *path)
{
	g_autofree char *base = g_path_get_basename (path);
	const char *dot = strrchr (base, '.');
	if (dot == NULL || dot == base)
		return g_strdup ("");
	return g_ascii_strdown (dot + 1, -1);
}

char *
ic_output_extension (const IcSettings *s, const char *src_path)
{
	if (s->format == IC_FORMAT_KEEP) {
		/* keep the original spelling of the extension */
		g_autofree char *base = g_path_get_basename (src_path);
		const char *dot = strrchr (base, '.');
		if (dot == NULL || dot == base)
			return g_strdup ("png");
		return g_strdup (dot + 1);
	}
	return g_strdup (ic_format_extension (s->format));
}

/* ------------------------------------------------------------------ */
/* Profiles                                                             */
/* ------------------------------------------------------------------ */

#define PROFILE_PREFIX "Profile "

static const char *format_keys[IC_FORMAT_N] = {
	"keep", "jpeg", "png", "webp", "avif", "gif", "pdf"
};
static const char *resize_keys[IC_RESIZE_N] = {
	"none", "scale", "fit", "exact"
};

IcProfile *
ic_profile_new (const char *name, const IcSettings *s)
{
	IcProfile *p = g_new0 (IcProfile, 1);
	p->name = g_strdup (name);
	ic_settings_copy_into (&p->s, s);
	return p;
}

void
ic_profile_free (IcProfile *p)
{
	if (p == NULL)
		return;
	g_free (p->name);
	ic_settings_clear (&p->s);
	g_free (p);
}

static void
add_default (GPtrArray *arr, const char *name, IcFormat fmt, IcResizeMode resize,
             int w, int h, int quality, int target_kb, gboolean strip, const char *suffix)
{
	IcSettings s = {
		.format = fmt, .resize = resize, .scale = 50, .width = w, .height = h,
		.no_upscale = TRUE, .quality = quality, .target_kb = target_kb,
		.strip = strip, .output = IC_OUTPUT_SUFFIX, .suffix = (char *) suffix,
	};
	g_ptr_array_add (arr, ic_profile_new (name, &s));
}

GPtrArray *
ic_profiles_defaults (void)
{
	GPtrArray *arr = g_ptr_array_new_with_free_func ((GDestroyNotify) ic_profile_free);

	add_default (arr, _("Web"),           IC_FORMAT_WEBP, IC_RESIZE_FIT,  1280,  960, 80,    0, TRUE,  "-web");
	add_default (arr, _("Email"),         IC_FORMAT_JPEG, IC_RESIZE_FIT,  1024,  768, 75,    0, TRUE,  "-email");
	add_default (arr, _("Social media"),  IC_FORMAT_JPEG, IC_RESIZE_FIT,  2048, 2048, 85,    0, TRUE,  "-social");
	add_default (arr, _("Thumbnail"),     IC_FORMAT_JPEG, IC_RESIZE_FIT,   320,  320, 80,    0, TRUE,  "-thumb");
	add_default (arr, _("Under 1 MB"),    IC_FORMAT_JPEG, IC_RESIZE_NONE, 1920, 1080, 90, 1000, FALSE, "-1mb");
	add_default (arr, _("Lossless PNG"),  IC_FORMAT_PNG,  IC_RESIZE_NONE, 1920, 1080, 90,    0, FALSE, "");
	add_default (arr, _("Custom"),        IC_FORMAT_KEEP, IC_RESIZE_SCALE,1920, 1080, 90,    0, FALSE, "-resized");

	return arr;
}

static char *
profiles_path (void)
{
	return g_build_filename (g_get_user_config_dir (), PACKAGE, "profiles.ini", NULL);
}

static int
lookup_key (const char *const *keys, int n, const char *val, int fallback)
{
	for (int i = 0; i < n; i++)
		if (g_strcmp0 (keys[i], val) == 0)
			return i;
	return fallback;
}

static int
kf_int (GKeyFile *kf, const char *group, const char *key, int fallback)
{
	GError *err = NULL;
	int v = g_key_file_get_integer (kf, group, key, &err);
	if (err) {
		g_error_free (err);
		return fallback;
	}
	return v;
}

static gboolean
kf_bool (GKeyFile *kf, const char *group, const char *key, gboolean fallback)
{
	GError *err = NULL;
	gboolean v = g_key_file_get_boolean (kf, group, key, &err);
	if (err) {
		g_error_free (err);
		return fallback;
	}
	return v;
}

GPtrArray *
ic_profiles_load (char **last_used, gboolean *keep_ratio)
{
	g_autofree char *path = profiles_path ();
	g_autoptr (GKeyFile) kf = g_key_file_new ();
	GPtrArray *arr;
	gchar **groups;

	if (last_used)
		*last_used = NULL;
	if (keep_ratio)
		*keep_ratio = FALSE;

	if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
		return ic_profiles_defaults ();

	arr = g_ptr_array_new_with_free_func ((GDestroyNotify) ic_profile_free);
	groups = g_key_file_get_groups (kf, NULL);

	for (int i = 0; groups[i]; i++) {
		const char *g = groups[i];
		g_autofree char *fmt = NULL, *resize = NULL, *output = NULL, *suffix = NULL;
		IcSettings s = { 0 };

		if (!g_str_has_prefix (g, PROFILE_PREFIX))
			continue;

		fmt = g_key_file_get_string (kf, g, "format", NULL);
		resize = g_key_file_get_string (kf, g, "resize", NULL);
		output = g_key_file_get_string (kf, g, "output", NULL);
		suffix = g_key_file_get_string (kf, g, "suffix", NULL);

		s.format = lookup_key (format_keys, IC_FORMAT_N, fmt, IC_FORMAT_KEEP);
		s.resize = lookup_key (resize_keys, IC_RESIZE_N, resize, IC_RESIZE_NONE);
		s.scale = CLAMP (kf_int (kf, g, "scale", 50), 1, 1000);
		s.width = CLAMP (kf_int (kf, g, "width", 1920), 1, 100000);
		s.height = CLAMP (kf_int (kf, g, "height", 1080), 1, 100000);
		s.no_upscale = kf_bool (kf, g, "no-upscale", TRUE);
		s.quality = CLAMP (kf_int (kf, g, "quality", 90), 1, 100);
		s.target_kb = MAX (kf_int (kf, g, "target-size-kb", 0), 0);
		s.strip = kf_bool (kf, g, "strip-metadata", FALSE);
		s.output = g_strcmp0 (output, "replace") == 0 ? IC_OUTPUT_REPLACE : IC_OUTPUT_SUFFIX;
		s.suffix = suffix ? suffix : (char *) "";

		g_ptr_array_add (arr, ic_profile_new (g + strlen (PROFILE_PREFIX), &s));
	}
	g_strfreev (groups);

	if (last_used)
		*last_used = g_key_file_get_string (kf, "General", "last-profile", NULL);
	if (keep_ratio)
		*keep_ratio = kf_bool (kf, "General", "keep-ratio", FALSE);

	if (arr->len == 0) {
		g_ptr_array_unref (arr);
		return ic_profiles_defaults ();
	}
	return arr;
}

void
ic_profiles_save (GPtrArray *profiles, const char *last_used, gboolean keep_ratio)
{
	g_autofree char *path = profiles_path ();
	g_autofree char *dir = g_path_get_dirname (path);
	g_autoptr (GKeyFile) kf = g_key_file_new ();
	GError *err = NULL;

	if (last_used)
		g_key_file_set_string (kf, "General", "last-profile", last_used);
	g_key_file_set_boolean (kf, "General", "keep-ratio", keep_ratio);

	for (guint i = 0; i < profiles->len; i++) {
		IcProfile *p = g_ptr_array_index (profiles, i);
		g_autofree char *g = g_strconcat (PROFILE_PREFIX, p->name, NULL);

		g_key_file_set_string (kf, g, "format", format_keys[p->s.format]);
		g_key_file_set_string (kf, g, "resize", resize_keys[p->s.resize]);
		g_key_file_set_integer (kf, g, "scale", p->s.scale);
		g_key_file_set_integer (kf, g, "width", p->s.width);
		g_key_file_set_integer (kf, g, "height", p->s.height);
		g_key_file_set_boolean (kf, g, "no-upscale", p->s.no_upscale);
		g_key_file_set_integer (kf, g, "quality", p->s.quality);
		g_key_file_set_integer (kf, g, "target-size-kb", p->s.target_kb);
		g_key_file_set_boolean (kf, g, "strip-metadata", p->s.strip);
		g_key_file_set_string (kf, g, "output",
		                       p->s.output == IC_OUTPUT_REPLACE ? "replace" : "suffix");
		g_key_file_set_string (kf, g, "suffix", p->s.suffix ? p->s.suffix : "");
	}

	g_mkdir_with_parents (dir, 0700);
	if (!g_key_file_save_to_file (kf, path, &err)) {
		g_warning ("Could not save image converter profiles: %s", err->message);
		g_error_free (err);
	}
}

/* ------------------------------------------------------------------ */
/* ImageMagick                                                          */
/* ------------------------------------------------------------------ */

static gboolean
have_magick7 (void)
{
	static int cached = -1;
	if (cached < 0) {
		g_autofree char *p = g_find_program_in_path ("magick");
		cached = p != NULL;
	}
	return cached;
}

const char *
ic_magick_convert (void)
{
	return have_magick7 () ? "magick" : "convert";
}

void
ic_build_ops (GPtrArray *argv,
              const IcSettings *s,
              const IcEdit *e,
              const char *in_path,
              int src_w, int src_h,
              const char *out_ext,
              gboolean first_frame)
{
	g_autofree char *in_ext = ic_path_extension (in_path);
	g_autofree char *out_lower = g_ascii_strdown (out_ext, -1);
	gboolean animated = !first_frame && g_strcmp0 (in_ext, "gif") == 0 &&
	                    g_strcmp0 (out_lower, "gif") == 0;
	double angle = ic_edit_total_angle (e);
	IcEdit local;

	if (e->auto_crop && src_w > 0 && src_h > 0) {
		local = *e;
		ic_edit_auto_crop (&local, src_w, src_h);
		e = &local;
	}

	if (first_frame || (!animated && (g_strcmp0 (in_ext, "gif") == 0 ||
	                                  g_strcmp0 (in_ext, "tif") == 0 ||
	                                  g_strcmp0 (in_ext, "tiff") == 0 ||
	                                  g_strcmp0 (in_ext, "ico") == 0)))
		g_ptr_array_add (argv, g_strconcat (in_path, "[0]", NULL));
	else
		g_ptr_array_add (argv, g_strdup (in_path));

	if (animated)
		g_ptr_array_add (argv, g_strdup ("-coalesce"));

	g_ptr_array_add (argv, g_strdup ("-auto-orient"));

	g_ptr_array_add (argv, g_strdup ("-background"));
	g_ptr_array_add (argv, g_strdup (ic_extension_has_alpha (out_lower) ? "none" : "white"));

	if (e->flip_h)
		g_ptr_array_add (argv, g_strdup ("-flop"));
	if (e->flip_v)
		g_ptr_array_add (argv, g_strdup ("-flip"));

	if (fabs (angle) > 1e-6) {
		char buf[G_ASCII_DTOSTR_BUF_SIZE];
		g_ptr_array_add (argv, g_strdup ("-rotate"));
		g_ptr_array_add (argv, g_strdup (g_ascii_formatd (buf, sizeof buf, "%.3f", angle)));
		g_ptr_array_add (argv, g_strdup ("+repage"));
	}

	if (ic_edit_has_crop (e) && src_w > 0 && src_h > 0) {
		double bw, bh;
		int cw, ch, ox, oy;

		ic_edit_bbox (e, src_w, src_h, &bw, &bh);
		cw = MAX (1, (int) round (e->cw * bw));
		ch = MAX (1, (int) round (e->ch * bh));
		/* Offsets relative to the centre, so that a slightly different
		 * bounding box computed by ImageMagick does not shift the crop. */
		ox = (int) round ((e->cx + e->cw / 2.0 - 0.5) * bw);
		oy = (int) round ((e->cy + e->ch / 2.0 - 0.5) * bh);

		g_ptr_array_add (argv, g_strdup ("-gravity"));
		g_ptr_array_add (argv, g_strdup ("center"));
		g_ptr_array_add (argv, g_strdup ("-crop"));
		g_ptr_array_add (argv, g_strdup_printf ("%dx%d%+d%+d", cw, ch, ox, oy));
		g_ptr_array_add (argv, g_strdup ("+repage"));
		g_ptr_array_add (argv, g_strdup ("+gravity"));
	}

	switch (s->resize) {
	case IC_RESIZE_SCALE:
		if (s->scale != 100) {
			g_ptr_array_add (argv, g_strdup ("-resize"));
			g_ptr_array_add (argv, g_strdup_printf ("%d%%", s->scale));
		}
		break;
	case IC_RESIZE_FIT:
		g_ptr_array_add (argv, g_strdup ("-resize"));
		g_ptr_array_add (argv, g_strdup_printf ("%dx%d%s", s->width, s->height,
		                                        s->no_upscale ? ">" : ""));
		break;
	case IC_RESIZE_EXACT:
		/* Fill the box, then trim what sticks out (centred): the crop
		 * rectangle is locked to this ratio in the UI, so this only trims
		 * other selected images whose proportions differ. Never distorts. */
		g_ptr_array_add (argv, g_strdup ("-resize"));
		g_ptr_array_add (argv, g_strdup_printf ("%dx%d^", s->width, s->height));
		g_ptr_array_add (argv, g_strdup ("-gravity"));
		g_ptr_array_add (argv, g_strdup ("center"));
		g_ptr_array_add (argv, g_strdup ("-extent"));
		g_ptr_array_add (argv, g_strdup_printf ("%dx%d", s->width, s->height));
		g_ptr_array_add (argv, g_strdup ("+gravity"));
		g_ptr_array_add (argv, g_strdup ("+repage"));
		break;
	default:
		break;
	}

	if (!ic_extension_has_alpha (out_lower)) {
		g_ptr_array_add (argv, g_strdup ("-alpha"));
		g_ptr_array_add (argv, g_strdup ("remove"));
	}

	if (s->strip)
		g_ptr_array_add (argv, g_strdup ("-strip"));

	if (ic_extension_has_quality (out_lower)) {
		g_ptr_array_add (argv, g_strdup ("-quality"));
		g_ptr_array_add (argv, g_strdup_printf ("%d", s->quality));
	}

	if (s->target_kb > 0 && ic_extension_has_target_size (out_lower)) {
		g_ptr_array_add (argv, g_strdup ("-define"));
		if (g_strcmp0 (out_lower, "webp") == 0)
			g_ptr_array_add (argv, g_strdup_printf ("webp:target-size=%d", s->target_kb * 1000));
		else
			g_ptr_array_add (argv, g_strdup_printf ("jpeg:extent=%dkb", s->target_kb));
	}

	if (animated) {
		g_ptr_array_add (argv, g_strdup ("-layers"));
		g_ptr_array_add (argv, g_strdup ("optimize"));
	}
}

gboolean
ic_identify (const char *path, int *w, int *h)
{
	g_autofree char *in = g_strconcat (path, "[0]", NULL);
	g_autofree char *out = NULL;
	g_auto (GStrv) parts = NULL;
	g_autoptr (GPtrArray) argv = g_ptr_array_new ();
	int status = 0;

	if (have_magick7 ()) {
		g_ptr_array_add (argv, "magick");
		g_ptr_array_add (argv, "identify");
	} else {
		g_ptr_array_add (argv, "identify");
	}
	g_ptr_array_add (argv, "-format");
	g_ptr_array_add (argv, "%w %h %[orientation]");
	g_ptr_array_add (argv, in);
	g_ptr_array_add (argv, NULL);

	if (!g_spawn_sync (NULL, (char **) argv->pdata, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
	                   NULL, NULL, &out, NULL, &status, NULL) || status != 0 || out == NULL)
		return FALSE;

	parts = g_strsplit (g_strstrip (out), " ", 3);
	if (g_strv_length (parts) < 2)
		return FALSE;

	*w = atoi (parts[0]);
	*h = atoi (parts[1]);

	/* EXIF orientations 5..8 (LeftTop, RightTop, RightBottom, LeftBottom)
	 * swap width and height once -auto-orient is applied. */
	if (parts[2] && (g_str_has_prefix (parts[2], "Left") || g_str_has_prefix (parts[2], "Right"))) {
		int t = *w;
		*w = *h;
		*h = t;
	}
	return *w > 0 && *h > 0;
}
