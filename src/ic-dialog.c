/*
 *  ic-dialog.c — the "Resize/convert image" window
 *
 *  One window that combines resizing, compression, format conversion,
 *  cropping and rotation, with a live preview of the result and
 *  editable profiles.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 */

#include <config.h>

#include "ic-dialog.h"
#include "ic-settings.h"
#include "ic-crop-view.h"

#include <math.h>
#include <string.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <nautilus-extension.h>

#define THUMB_SIZE "1600x1600>"
#define PREVIEW_DELAY_MS 350

typedef struct {
	char             *path;
	NautilusFileInfo *info;
	GdkTexture       *thumb;
	int               w, h;     /* oriented full-size dimensions */
	goffset           size;
	gboolean          loading;
	gboolean          failed;
} IcImage;

typedef struct {
	GPtrArray   *images;        /* IcImage* */
	guint        current;

	IcSettings   s;
	IcEdit       e;

	GPtrArray   *profiles;      /* IcProfile* */
	guint        profile_idx;
	gboolean     updating;      /* widgets are being set programmatically */

	char        *tmpdir;
	GCancellable *cancellable;  /* cancelled when the window goes away */
	gboolean     closed;

	/* preview */
	guint        preview_source;
	int          preview_serial;
	GSubprocess *preview_proc;
	GCancellable *preview_cancel;

	/* main widgets */
	GtkWidget *window;
	GtkWidget *stack;
	GtkWidget *crop_view;
	GtkWidget *crop_spinner;
	GtkWidget *preview_picture;
	GtkWidget *preview_scrolled;
	GtkWidget *preview_spinner;
	GtkWidget *preview_error;
	GtkWidget *edit_toolbar;
	GtkWidget *preview_toolbar;
	GtkWidget *zoom_toggle;
	GtkWidget *straighten_scale;
	GtkWidget *aspect_dd;
	GtkWidget *nav_box;
	GtkWidget *nav_label;
	GtkWidget *prev_btn, *next_btn;
	GtkWidget *orig_label;
	GtkWidget *result_label;
	GtkWidget *convert_btn;

	/* settings widgets */
	GtkWidget *profile_dd;
	GtkStringList *profile_model;
	GtkWidget *profile_modified;
	GtkWidget *profile_save_btn;
	GtkWidget *profile_menu_btn;
	GtkWidget *profile_name_entry;
	GtkWidget *profile_delete_btn;
	GtkWidget *resize_dd;
	GtkWidget *scale_row_label, *scale_spin;
	GtkWidget *wh_row_label, *wh_box, *width_spin, *height_spin;
	GtkWidget *preset_label, *preset_dd;
	int        preset_w, preset_h;   /* size the preset menu was last matched against */
	GtkWidget *noup_check;
	GtkWidget *format_dd;
	GtkWidget *quality_label, *quality_scale;
	GtkWidget *target_check, *target_spin, *target_box;
	GtkWidget *strip_check;
	GtkWidget *combine_check;
	GtkWidget *suffix_radio, *suffix_entry, *replace_radio;

	/* processing */
	gboolean     processing;
	guint        proc_index;
	int          proc_failed;
	GSubprocess *proc;
	GCancellable *proc_cancel;
	char        *proc_tmp;
	char        *proc_final;
	char        *proc_trash;
	GtkWidget   *progress_window;
	GtkWidget   *progress_bar;
	GtkWidget   *progress_label;
} IcDialog;

/* Crop ratios. 0 = free, -1 = the image's own ratio. Except in strict mode,
 * a ratio follows the orientation of the crop (4:3 also gives 3:4). */
static const double aspect_values[] = {
	0.0, -1.0, 1.0, 4.0 / 3.0, 3.0 / 2.0, 16.0 / 9.0, 4.0 / 5.0, 1.91
};
#define ASPECT_FREE 0
#define ASPECT_FIRST_FIXED 2

static void schedule_preview (IcDialog *d);
static void run_preview (IcDialog *d);
static void process_next (IcDialog *d);
static void update_info (IcDialog *d);
static void apply_aspect_from_dropdown (IcDialog *d);
static void refresh_crop_constraint (IcDialog *d);
static void fit_crop_to_output (IcDialog *d);
static gboolean size_linked (IcDialog *d);
static int spin_int (GtkWidget *spin);
static void sync_preset (IcDialog *d);

/* ------------------------------------------------------------------ */
/* Lifetime                                                             */
/* ------------------------------------------------------------------ */

static void
image_free (IcImage *im)
{
	g_free (im->path);
	g_clear_object (&im->info);
	g_clear_object (&im->thumb);
	g_free (im);
}

static void
remove_dir (const char *path)
{
	GDir *dir = g_dir_open (path, 0, NULL);
	const char *name;

	if (dir == NULL)
		return;
	while ((name = g_dir_read_name (dir)) != NULL) {
		g_autofree char *p = g_build_filename (path, name, NULL);
		g_unlink (p);
	}
	g_dir_close (dir);
	g_rmdir (path);
}

static void
dialog_clear (IcDialog *d)
{
	g_ptr_array_unref (d->images);
	g_ptr_array_unref (d->profiles);
	ic_settings_clear (&d->s);
	if (d->tmpdir)
		remove_dir (d->tmpdir);
	g_free (d->tmpdir);
	g_clear_object (&d->cancellable);
	g_clear_object (&d->preview_cancel);
	g_clear_object (&d->preview_proc);
	g_clear_object (&d->proc);
	g_clear_object (&d->proc_cancel);
	g_free (d->proc_tmp);
	g_free (d->proc_final);
	g_free (d->proc_trash);
}

static IcDialog *
dialog_ref (IcDialog *d)
{
	return g_rc_box_acquire (d);
}

static void
dialog_unref (IcDialog *d)
{
	g_rc_box_release_full (d, (GDestroyNotify) dialog_clear);
}

static IcImage *
current_image (IcDialog *d)
{
	return g_ptr_array_index (d->images, d->current);
}

/* Tear down the UI. Running jobs keep their own reference. */
static void
dialog_close (IcDialog *d)
{
	if (d->closed)
		return;
	d->closed = TRUE;

	g_cancellable_cancel (d->cancellable);
	if (d->preview_source) {
		g_source_remove (d->preview_source);
		d->preview_source = 0;
	}
	if (d->preview_proc)
		g_subprocess_force_exit (d->preview_proc);

	if (d->progress_window)
		gtk_window_destroy (GTK_WINDOW (d->progress_window));
	d->progress_window = NULL;
	if (d->window)
		gtk_window_destroy (GTK_WINDOW (d->window));
	d->window = NULL;

	dialog_unref (d);
}

/* ------------------------------------------------------------------ */
/* Subprocess helpers                                                   */
/* ------------------------------------------------------------------ */

static GSubprocess *
spawn (GPtrArray *argv, GError **error)
{
	g_ptr_array_add (argv, NULL);
	return g_subprocess_newv ((const char *const *) argv->pdata,
	                          G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
	                          error);
}

static GPtrArray *
new_argv (void)
{
	GPtrArray *argv = g_ptr_array_new_with_free_func (g_free);
	g_ptr_array_add (argv, g_strdup (ic_magick_convert ()));
	return argv;
}

static char *
first_line (const char *text)
{
	g_auto (GStrv) lines = NULL;
	if (text == NULL)
		return g_strdup ("");
	lines = g_strsplit (text, "\n", 2);
	return g_strdup (lines[0] ? g_strstrip (lines[0]) : "");
}

/* ------------------------------------------------------------------ */
/* Thumbnails of the source images                                      */
/* ------------------------------------------------------------------ */

typedef struct {
	IcDialog *d;
	guint     idx;
	char     *png;
} ThumbJob;

static void
show_current_image (IcDialog *d)
{
	IcImage *im = current_image (d);

	gtk_widget_set_visible (d->crop_spinner, im->loading);
	if (d->e.auto_crop && im->w > 0)
		ic_edit_auto_crop (&d->e, im->w, im->h);
	if (im->thumb) {
		ic_crop_view_set_image (IC_CROP_VIEW (d->crop_view), im->thumb, im->w, im->h);
		refresh_crop_constraint (d);
	} else
		ic_crop_view_set_image (IC_CROP_VIEW (d->crop_view), NULL, 0, 0);
	update_info (d);
}

static void
thumb_done (GObject *source, GAsyncResult *res, gpointer data)
{
	ThumbJob *job = data;
	IcDialog *d = job->d;
	IcImage *im = g_ptr_array_index (d->images, job->idx);
	g_autofree char *out = NULL;
	g_autofree char *err = NULL;
	g_autoptr (GError) error = NULL;

	g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, &out, &err, &error);
	im->loading = FALSE;

	if (!d->closed) {
		if (error == NULL && g_subprocess_get_successful (G_SUBPROCESS (source)) &&
		    out != NULL && sscanf (out, "%d %d", &im->w, &im->h) == 2) {
			im->thumb = gdk_texture_new_from_filename (job->png, NULL);
		}
		if (im->thumb == NULL) {
			im->failed = TRUE;
			g_warning ("Could not load %s: %s", im->path, err ? err : (error ? error->message : ""));
		}
		g_unlink (job->png);

		if (job->idx == d->current) {
			show_current_image (d);
			run_preview (d);
		}
	}

	g_free (job->png);
	g_free (job);
	dialog_unref (d);
}

static void
load_thumb (IcDialog *d, guint idx)
{
	IcImage *im = g_ptr_array_index (d->images, idx);
	g_autoptr (GPtrArray) argv = NULL;
	g_autoptr (GError) error = NULL;
	g_autoptr (GSubprocess) proc = NULL;
	ThumbJob *job;

	if (im->thumb || im->loading || im->failed)
		return;

	job = g_new0 (ThumbJob, 1);
	job->d = dialog_ref (d);
	job->idx = idx;
	job->png = g_strdup_printf ("%s/thumb-%u.png", d->tmpdir, idx);

	argv = new_argv ();
	g_ptr_array_add (argv, g_strconcat (im->path, "[0]", NULL));
	g_ptr_array_add (argv, g_strdup ("-auto-orient"));
	g_ptr_array_add (argv, g_strdup ("-format"));
	g_ptr_array_add (argv, g_strdup ("%w %h\n"));
	g_ptr_array_add (argv, g_strdup ("-write"));
	g_ptr_array_add (argv, g_strdup ("info:-"));
	g_ptr_array_add (argv, g_strdup ("-thumbnail"));
	g_ptr_array_add (argv, g_strdup (THUMB_SIZE));
	g_ptr_array_add (argv, g_strconcat ("png:", job->png, NULL));

	proc = spawn (argv, &error);
	if (proc == NULL) {
		g_warning ("Could not run ImageMagick: %s", error->message);
		im->failed = TRUE;
		dialog_unref (job->d);
		g_free (job->png);
		g_free (job);
		return;
	}

	im->loading = TRUE;
	g_subprocess_communicate_utf8_async (proc, NULL, d->cancellable, thumb_done, job);
}

/* ------------------------------------------------------------------ */
/* Info line                                                            */
/* ------------------------------------------------------------------ */

static void
update_info (IcDialog *d)
{
	IcImage *im = current_image (d);
	g_autofree char *name = g_path_get_basename (im->path);
	g_autofree char *size = g_format_size (im->size);
	g_autofree char *text = NULL;

	if (im->thumb)
		text = g_strdup_printf (_("Original: %d × %d px · %s"), im->w, im->h, size);
	else if (im->failed)
		text = g_strdup_printf (_("Original: %s (cannot be previewed)"), size);
	else
		text = g_strdup_printf (_("Original: %s"), size);
	gtk_label_set_text (GTK_LABEL (d->orig_label), text);

	if (d->images->len > 1) {
		g_autofree char *nav = g_strdup_printf ("%u / %u  ·  %s", d->current + 1, d->images->len, name);
		gtk_label_set_text (GTK_LABEL (d->nav_label), nav);
	} else {
		gtk_label_set_text (GTK_LABEL (d->nav_label), name);
	}
	gtk_widget_set_sensitive (d->prev_btn, d->current > 0);
	gtk_widget_set_sensitive (d->next_btn, d->current + 1 < d->images->len);
}

/* ------------------------------------------------------------------ */
/* Preview of the result                                                */
/* ------------------------------------------------------------------ */

typedef struct {
	IcDialog *d;
	int       serial;
	char     *out;        /* file produced in the target format */
	char     *display;    /* file to display (may be a PNG re-encode) */
	goffset   size;
	char     *ext;
} PreviewJob;

static void
preview_job_free (PreviewJob *job)
{
	if (job->out)
		g_unlink (job->out);
	if (job->display && g_strcmp0 (job->display, job->out) != 0)
		g_unlink (job->display);
	g_free (job->out);
	g_free (job->display);
	g_free (job->ext);
	dialog_unref (job->d);
	g_free (job);
}

static void
preview_failed (IcDialog *d, const char *message)
{
	g_autofree char *text = g_strdup_printf (_("Preview failed: %s"), message);
	gtk_widget_set_visible (d->preview_spinner, FALSE);
	gtk_label_set_text (GTK_LABEL (d->preview_error), text);
	gtk_widget_set_visible (d->preview_error, TRUE);
	gtk_label_set_text (GTK_LABEL (d->result_label), _("Result: —"));
}

static void
preview_show (PreviewJob *job, GdkTexture *texture)
{
	IcDialog *d = job->d;
	IcImage *im = current_image (d);
	g_autofree char *size = g_format_size (job->size);
	g_autofree char *text = NULL;
	g_autofree char *fmt = g_ascii_strup (job->ext, -1);
	int w = gdk_texture_get_width (texture);
	int h = gdk_texture_get_height (texture);

	gtk_picture_set_paintable (GTK_PICTURE (d->preview_picture), GDK_PAINTABLE (texture));
	gtk_widget_set_visible (d->preview_spinner, FALSE);
	gtk_widget_set_visible (d->preview_error, FALSE);

	if (d->s.format == IC_FORMAT_PDF) {
		text = g_strdup_printf (_("Result: %d × %d px · PDF page"), w, h);
	} else if (im->size > 0) {
		double pct = 100.0 * ((double) job->size - (double) im->size) / (double) im->size;
		text = g_strdup_printf (_("Result: %d × %d px · %s · %s (%+.0f%%)"), w, h, size, fmt, pct);
	} else {
		text = g_strdup_printf (_("Result: %d × %d px · %s · %s"), w, h, size, fmt);
	}
	gtk_label_set_text (GTK_LABEL (d->result_label), text);
}

static void
preview_png_done (GObject *source, GAsyncResult *res, gpointer data)
{
	PreviewJob *job = data;
	IcDialog *d = job->d;
	g_autofree char *err = NULL;
	g_autoptr (GError) error = NULL;
	g_autoptr (GdkTexture) texture = NULL;

	g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, NULL, &err, &error);
	if (source == (GObject *) d->preview_proc)
		g_clear_object (&d->preview_proc);

	if (d->closed || job->serial != d->preview_serial) {
		preview_job_free (job);
		return;
	}

	if (error == NULL && g_subprocess_get_successful (G_SUBPROCESS (source)))
		texture = gdk_texture_new_from_filename (job->display, &error);

	if (texture)
		preview_show (job, texture);
	else
		preview_failed (d, error ? error->message : _("cannot display the result"));

	preview_job_free (job);
}

static void
preview_done (GObject *source, GAsyncResult *res, gpointer data)
{
	PreviewJob *job = data;
	IcDialog *d = job->d;
	g_autofree char *err = NULL;
	g_autoptr (GError) error = NULL;
	g_autoptr (GdkTexture) texture = NULL;
	GStatBuf st;

	g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, NULL, &err, &error);
	if (source == (GObject *) d->preview_proc)
		g_clear_object (&d->preview_proc);

	if (d->closed || job->serial != d->preview_serial) {
		preview_job_free (job);
		return;
	}

	if (error != NULL || !g_subprocess_get_successful (G_SUBPROCESS (source))) {
		g_autofree char *line = first_line (err);
		preview_failed (d, error ? error->message : line);
		preview_job_free (job);
		return;
	}

	job->size = g_stat (job->out, &st) == 0 ? st.st_size : 0;

	/* GTK can show PNG, JPEG and TIFF itself, other formats if a pixbuf loader exists */
	texture = gdk_texture_new_from_filename (job->out, NULL);
	if (texture) {
		preview_show (job, texture);
		preview_job_free (job);
		return;
	}

	/* re-encode as PNG just for displaying it */
	{
		g_autoptr (GPtrArray) argv = new_argv ();
		g_autoptr (GError) spawn_error = NULL;

		job->display = g_strdup_printf ("%s/preview-%d-display.png", d->tmpdir, job->serial);
		g_ptr_array_add (argv, g_strdup (job->out));
		g_ptr_array_add (argv, g_strconcat ("png:", job->display, NULL));
		d->preview_proc = spawn (argv, &spawn_error);
		if (d->preview_proc == NULL) {
			preview_failed (d, spawn_error->message);
			preview_job_free (job);
			return;
		}
		g_subprocess_communicate_utf8_async (d->preview_proc, NULL, d->preview_cancel,
		                                     preview_png_done, job);
	}
}

static void
run_preview (IcDialog *d)
{
	IcImage *im = current_image (d);
	g_autoptr (GPtrArray) argv = NULL;
	g_autoptr (GError) error = NULL;
	g_autofree char *ext = NULL;
	PreviewJob *job;

	if (d->preview_source) {
		g_source_remove (d->preview_source);
		d->preview_source = 0;
	}
	if (d->closed || d->processing)
		return;

	/* stop whatever preview is still running */
	d->preview_serial++;
	if (d->preview_cancel)
		g_cancellable_cancel (d->preview_cancel);
	g_clear_object (&d->preview_cancel);
	if (d->preview_proc)
		g_subprocess_force_exit (d->preview_proc);
	g_clear_object (&d->preview_proc);

	if (im->failed) {
		preview_failed (d, _("the image could not be read"));
		return;
	}
	if (im->thumb == NULL) {
		load_thumb (d, d->current);
		return;  /* thumb_done() calls us again */
	}

	ext = ic_output_extension (&d->s, im->path);
	if (g_ascii_strcasecmp (ext, "pdf") == 0) {
		g_free (ext);
		ext = g_strdup ("png");
	}

	job = g_new0 (PreviewJob, 1);
	job->d = dialog_ref (d);
	job->serial = d->preview_serial;
	job->ext = g_strdup (d->s.format == IC_FORMAT_PDF ? "pdf" : ext);
	job->out = g_strdup_printf ("%s/preview-%d.%s", d->tmpdir, job->serial, ext);

	argv = new_argv ();
	ic_build_ops (argv, &d->s, &d->e, im->path, im->w, im->h, ext, TRUE);
	g_ptr_array_add (argv, g_strdup (job->out));

	d->preview_proc = spawn (argv, &error);
	if (d->preview_proc == NULL) {
		preview_failed (d, error->message);
		preview_job_free (job);
		return;
	}

	d->preview_cancel = g_cancellable_new ();
	gtk_widget_set_visible (d->preview_spinner, TRUE);
	gtk_label_set_text (GTK_LABEL (d->result_label), _("Result: computing…"));
	g_subprocess_communicate_utf8_async (d->preview_proc, NULL, d->preview_cancel, preview_done, job);
}

static gboolean
preview_timeout_cb (gpointer data)
{
	IcDialog *d = data;
	d->preview_source = 0;
	run_preview (d);
	return G_SOURCE_REMOVE;
}

static void
schedule_preview (IcDialog *d)
{
	if (d->closed)
		return;
	if (d->preview_source)
		g_source_remove (d->preview_source);
	d->preview_source = g_timeout_add (PREVIEW_DELAY_MS, preview_timeout_cb, d);
}

/* ------------------------------------------------------------------ */
/* Settings <-> widgets                                                 */
/* ------------------------------------------------------------------ */

static IcProfile *
current_profile (IcDialog *d)
{
	if (d->profile_idx >= d->profiles->len)
		return NULL;
	return g_ptr_array_index (d->profiles, d->profile_idx);
}

static void
update_sensitivity (IcDialog *d)
{
	IcImage *im = current_image (d);
	g_autofree char *ext = ic_output_extension (&d->s, im->path);
	g_autofree char *lext = g_ascii_strdown (ext, -1);
	IcProfile *p = current_profile (d);
	gboolean modified = p == NULL || !ic_settings_equal (&d->s, &p->s);
	gboolean combine;

	gtk_widget_set_visible (d->scale_row_label, d->s.resize == IC_RESIZE_SCALE);
	gtk_widget_set_visible (d->scale_spin, d->s.resize == IC_RESIZE_SCALE);
	gtk_widget_set_visible (d->wh_row_label, d->s.resize == IC_RESIZE_FIT || d->s.resize == IC_RESIZE_EXACT);
	gtk_widget_set_visible (d->wh_box, d->s.resize == IC_RESIZE_FIT || d->s.resize == IC_RESIZE_EXACT);
	gtk_widget_set_visible (d->noup_check, d->s.resize == IC_RESIZE_FIT);
	gtk_widget_set_visible (d->preset_label, d->s.resize == IC_RESIZE_FIT || d->s.resize == IC_RESIZE_EXACT);
	gtk_widget_set_visible (d->preset_dd, d->s.resize == IC_RESIZE_FIT || d->s.resize == IC_RESIZE_EXACT);
	sync_preset (d);

	gtk_widget_set_tooltip_text (d->aspect_dd, size_linked (d)
	                             ? _("Crop aspect ratio. The crop box and the output width × height are linked: changing one updates the other.")
	                             : _("Crop aspect ratio"));

	gtk_widget_set_sensitive (d->quality_label, ic_extension_has_quality (lext));
	gtk_widget_set_sensitive (d->quality_scale, ic_extension_has_quality (lext));
	gtk_widget_set_sensitive (d->target_box, ic_extension_has_target_size (lext));
	gtk_widget_set_sensitive (d->target_spin, d->s.target_kb > 0);

	gtk_widget_set_visible (d->combine_check, d->s.format == IC_FORMAT_PDF && d->images->len > 1);
	combine = gtk_widget_get_visible (d->combine_check) &&
	          gtk_check_button_get_active (GTK_CHECK_BUTTON (d->combine_check));

	gtk_widget_set_sensitive (d->suffix_entry, d->s.output == IC_OUTPUT_SUFFIX);
	gtk_widget_set_sensitive (d->suffix_radio, !combine);
	gtk_widget_set_sensitive (d->replace_radio, !combine);

	gtk_widget_set_visible (d->profile_modified, modified);
	gtk_widget_set_sensitive (d->profile_save_btn, modified && p != NULL);
	gtk_widget_set_sensitive (d->profile_delete_btn, d->profiles->len > 1);

	if (combine)
		gtk_button_set_label (GTK_BUTTON (d->convert_btn), _("Create PDF"));
	else if (d->images->len > 1) {
		g_autofree char *label = g_strdup_printf (ngettext ("Convert %u Image", "Convert %u Images",
		                                                    d->images->len), d->images->len);
		gtk_button_set_label (GTK_BUTTON (d->convert_btn), label);
	} else
		gtk_button_set_label (GTK_BUTTON (d->convert_btn), _("Convert"));
}

static const struct {
	int w, h;
	const char *name;
} size_presets[] = {
	{    0,    0, N_("Custom") },
	{  640,  480, "640 × 480 — 4:3 (VGA)" },
	{  800,  600, "800 × 600 — 4:3 (SVGA)" },
	{ 1024,  768, "1024 × 768 — 4:3 (XGA)" },
	{ 1280,  960, "1280 × 960 — 4:3" },
	{ 1600, 1200, "1600 × 1200 — 4:3" },
	{ 1280,  720, "1280 × 720 — 16:9 (HD)" },
	{ 1920, 1080, "1920 × 1080 — 16:9 (Full HD)" },
	{ 2560, 1440, "2560 × 1440 — 16:9 (QHD)" },
	{ 3840, 2160, "3840 × 2160 — 16:9 (4K)" },
	{ 1080, 1080, N_("1080 × 1080 — 1:1 (square post)") },
	{ 1080, 1350, N_("1080 × 1350 — 4:5 (portrait post)") },
	{ 1080, 1920, N_("1080 × 1920 — 9:16 (story)") },
	{ 1200,  630, N_("1200 × 630 — 1.91:1 (link preview)") },
};

/* Show the preset matching the current width/height, or "Custom". */
static void
sync_preset (IcDialog *d)
{
	guint idx = 0;

	/* Keep an explicit "Custom" (set when the crop goes back to Free)
	 * until the numbers change. */
	if (d->s.width == d->preset_w && d->s.height == d->preset_h)
		return;
	d->preset_w = d->s.width;
	d->preset_h = d->s.height;

	for (guint i = 1; i < G_N_ELEMENTS (size_presets); i++)
		if (size_presets[i].w == d->s.width && size_presets[i].h == d->s.height) {
			idx = i;
			break;
		}
	if (gtk_drop_down_get_selected (GTK_DROP_DOWN (d->preset_dd)) != idx) {
		gboolean was = d->updating;
		d->updating = TRUE;
		gtk_drop_down_set_selected (GTK_DROP_DOWN (d->preset_dd), idx);
		d->updating = was;
	}
}

static void settings_changed (IcDialog *d);

static void
preset_cb (GObject *o, GParamSpec *pspec, IcDialog *d)
{
	guint idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (d->preset_dd));

	if (d->updating || idx == 0 || idx >= G_N_ELEMENTS (size_presets))
		return;
	d->updating = TRUE;
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->width_spin), size_presets[idx].w);
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->height_spin), size_presets[idx].h);
	d->updating = FALSE;
	settings_changed (d);
}

static void
swap_size_cb (GtkButton *b, IcDialog *d)
{
	int w = spin_int (d->width_spin);
	int h = spin_int (d->height_spin);

	d->updating = TRUE;
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->width_spin), h);
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->height_spin), w);
	d->updating = FALSE;
	settings_changed (d);
}

/* The value being typed, before the spin button commits it on Enter. */
static int
spin_int (GtkWidget *spin)
{
	const char *text = gtk_editable_get_text (GTK_EDITABLE (spin));
	char *end = NULL;
	gint64 v = g_ascii_strtoll (text, &end, 10);
	GtkAdjustment *adj = gtk_spin_button_get_adjustment (GTK_SPIN_BUTTON (spin));

	if (end != text && *end == '\0' &&
	    v >= gtk_adjustment_get_lower (adj) && v <= gtk_adjustment_get_upper (adj))
		return (int) v;
	return gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (spin));
}

static void
settings_from_widgets (IcDialog *d)
{
	IcSettings *s = &d->s;

	s->resize = gtk_drop_down_get_selected (GTK_DROP_DOWN (d->resize_dd));
	s->scale = spin_int (d->scale_spin);
	s->width = spin_int (d->width_spin);
	s->height = spin_int (d->height_spin);
	s->no_upscale = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->noup_check));
	s->format = gtk_drop_down_get_selected (GTK_DROP_DOWN (d->format_dd));
	s->quality = (int) round (gtk_range_get_value (GTK_RANGE (d->quality_scale)));
	s->target_kb = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->target_check))
	               ? gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (d->target_spin)) : 0;
	s->strip = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->strip_check));
	s->output = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->replace_radio))
	            ? IC_OUTPUT_REPLACE : IC_OUTPUT_SUFFIX;
	g_free (s->suffix);
	s->suffix = g_strdup (gtk_editable_get_text (GTK_EDITABLE (d->suffix_entry)));

	if (s->resize >= IC_RESIZE_N)
		s->resize = IC_RESIZE_NONE;
	if (s->format >= IC_FORMAT_N)
		s->format = IC_FORMAT_KEEP;
}

static void
settings_to_widgets (IcDialog *d, const IcSettings *s)
{
	d->updating = TRUE;
	gtk_drop_down_set_selected (GTK_DROP_DOWN (d->resize_dd), s->resize);
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->scale_spin), s->scale);
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->width_spin), s->width);
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->height_spin), s->height);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (d->noup_check), s->no_upscale);
	gtk_drop_down_set_selected (GTK_DROP_DOWN (d->format_dd), s->format);
	gtk_range_set_value (GTK_RANGE (d->quality_scale), s->quality);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (d->target_check), s->target_kb > 0);
	if (s->target_kb > 0)
		gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->target_spin), s->target_kb);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (d->strip_check), s->strip);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (s->output == IC_OUTPUT_REPLACE
	                                               ? d->replace_radio : d->suffix_radio), TRUE);
	gtk_editable_set_text (GTK_EDITABLE (d->suffix_entry), s->suffix ? s->suffix : "");
	d->updating = FALSE;

	settings_from_widgets (d);
}

static void
settings_changed (IcDialog *d)
{
	IcSettings old = { 0 };

	if (d->updating)
		return;

	ic_settings_copy_into (&old, &d->s);
	settings_from_widgets (d);
	update_sensitivity (d);

	if ((size_linked (d) && (old.width != d->s.width || old.height != d->s.height)) ||
	    (d->s.resize == IC_RESIZE_EXACT && old.resize != d->s.resize)) {
		/* typed / picked an output size: the crop box takes its shape */
		fit_crop_to_output (d);
		ic_crop_view_update (IC_CROP_VIEW (d->crop_view));
	} else if (old.resize != d->s.resize) {
		apply_aspect_from_dropdown (d);
		ic_crop_view_update (IC_CROP_VIEW (d->crop_view));
	}

	/* the output name/mode does not change the preview */
	old.output = d->s.output;
	g_free (old.suffix);
	old.suffix = g_strdup (d->s.suffix);
	if (!ic_settings_equal (&old, &d->s))
		schedule_preview (d);
	ic_settings_clear (&old);
}

static void on_setting_notify (GObject *o, GParamSpec *p, IcDialog *d) { settings_changed (d); }
static void on_setting_changed (GtkWidget *w, IcDialog *d) { settings_changed (d); }

/* ------------------------------------------------------------------ */
/* Profiles                                                             */
/* ------------------------------------------------------------------ */

static void
rebuild_profile_model (IcDialog *d)
{
	g_autoptr (GPtrArray) names = g_ptr_array_new ();

	for (guint i = 0; i < d->profiles->len; i++)
		g_ptr_array_add (names, ((IcProfile *) g_ptr_array_index (d->profiles, i))->name);
	g_ptr_array_add (names, NULL);

	d->updating = TRUE;
	gtk_string_list_splice (d->profile_model, 0,
	                        g_list_model_get_n_items (G_LIST_MODEL (d->profile_model)),
	                        (const char *const *) names->pdata);
	gtk_drop_down_set_selected (GTK_DROP_DOWN (d->profile_dd), d->profile_idx);
	d->updating = FALSE;
}

static void
select_profile (IcDialog *d, guint idx)
{
	IcProfile *p;

	if (idx >= d->profiles->len)
		idx = 0;
	d->profile_idx = idx;
	p = current_profile (d);

	d->updating = TRUE;
	gtk_drop_down_set_selected (GTK_DROP_DOWN (d->profile_dd), idx);
	d->updating = FALSE;

	settings_to_widgets (d, &p->s);
	update_sensitivity (d);
	refresh_crop_constraint (d);
	ic_crop_view_update (IC_CROP_VIEW (d->crop_view));
	schedule_preview (d);
}

static void
profile_selected_cb (GObject *o, GParamSpec *pspec, IcDialog *d)
{
	if (d->updating)
		return;
	select_profile (d, gtk_drop_down_get_selected (GTK_DROP_DOWN (d->profile_dd)));
}

static void
save_profiles (IcDialog *d)
{
	IcProfile *p = current_profile (d);
	ic_profiles_save (d->profiles, p ? p->name : NULL);
}

static void
profile_save_cb (GtkButton *b, IcDialog *d)
{
	IcProfile *p = current_profile (d);
	if (p == NULL)
		return;
	ic_settings_copy_into (&p->s, &d->s);
	save_profiles (d);
	update_sensitivity (d);
}

static void
profile_save_as_cb (GtkWidget *w, IcDialog *d)
{
	g_autofree char *name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (d->profile_name_entry))));
	guint idx;

	if (name[0] == '\0')
		return;

	for (idx = 0; idx < d->profiles->len; idx++) {
		IcProfile *p = g_ptr_array_index (d->profiles, idx);
		if (g_strcmp0 (p->name, name) == 0) {
			ic_settings_copy_into (&p->s, &d->s);
			break;
		}
	}
	if (idx == d->profiles->len)
		g_ptr_array_add (d->profiles, ic_profile_new (name, &d->s));

	d->profile_idx = idx;
	rebuild_profile_model (d);
	save_profiles (d);
	update_sensitivity (d);

	gtk_editable_set_text (GTK_EDITABLE (d->profile_name_entry), "");
	gtk_menu_button_popdown (GTK_MENU_BUTTON (d->profile_menu_btn));
}

static void
profile_delete_cb (GtkButton *b, IcDialog *d)
{
	if (d->profiles->len <= 1)
		return;
	g_ptr_array_remove_index (d->profiles, d->profile_idx);
	d->profile_idx = MIN (d->profile_idx, d->profiles->len - 1);
	rebuild_profile_model (d);
	select_profile (d, d->profile_idx);
	save_profiles (d);
	gtk_menu_button_popdown (GTK_MENU_BUTTON (d->profile_menu_btn));
}

static void
profile_restore_cb (GtkButton *b, IcDialog *d)
{
	g_ptr_array_unref (d->profiles);
	d->profiles = ic_profiles_defaults ();
	d->profile_idx = 0;
	rebuild_profile_model (d);
	select_profile (d, 0);
	save_profiles (d);
	gtk_menu_button_popdown (GTK_MENU_BUTTON (d->profile_menu_btn));
}

/* ------------------------------------------------------------------ */
/* Crop & rotate tools                                                  */
/* ------------------------------------------------------------------ */

/* In "Fit within" and "Exact size" modes, the crop box and the
 * width × height are linked: editing one reshapes the other. */
static gboolean
size_linked (IcDialog *d)
{
	return d->s.resize == IC_RESIZE_FIT || d->s.resize == IC_RESIZE_EXACT;
}

/* Index of the fixed crop ratio matching w:h (either orientation), or -1. */
static int
aspect_index_for (int w, int h)
{
	double r;

	if (w <= 0 || h <= 0)
		return -1;
	r = (double) w / h;
	for (guint i = ASPECT_FIRST_FIXED; i < G_N_ELEMENTS (aspect_values); i++) {
		double a = aspect_values[i];
		if (fabs (r - a) / a < 0.01 || fabs (1.0 / r - a) / a < 0.01)
			return i;
	}
	return -1;
}

static void
set_aspect_dropdown (IcDialog *d, guint idx)
{
	if (gtk_drop_down_get_selected (GTK_DROP_DOWN (d->aspect_dd)) != idx) {
		gboolean was = d->updating;
		d->updating = TRUE;
		gtk_drop_down_set_selected (GTK_DROP_DOWN (d->aspect_dd), idx);
		d->updating = was;
	}
}

/* Linked mode: the output width × height follows the crop box's shape.
 * The long side keeps its length, so rotating by 90° swaps the numbers. */
static void
sync_output_from_crop (IcDialog *d)
{
	IcImage *im = current_image (d);
	double bw, bh, r;
	int w, h, longest;

	if (!size_linked (d) || d->updating || im->w <= 0 || im->h <= 0 ||
	    d->e.cw <= 0 || d->e.ch <= 0)
		return;

	ic_edit_bbox (&d->e, im->w, im->h, &bw, &bh);
	r = (d->e.cw * bw) / (d->e.ch * bh);
	longest = MAX (d->s.width, d->s.height);
	if (r >= 1.0) {
		w = longest;
		h = (int) round (longest / r);
	} else {
		w = (int) round (longest * r);
		h = longest;
	}
	w = CLAMP (w, 1, 100000);
	h = CLAMP (h, 1, 100000);
	if (w == d->s.width && h == d->s.height)
		return;

	d->updating = TRUE;
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->width_spin), w);
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->height_spin), h);
	d->updating = FALSE;
	d->s.width = w;
	d->s.height = h;
	update_sensitivity (d);
}

static void
apply_aspect_from_dropdown (IcDialog *d)
{
	IcImage *im = current_image (d);
	guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (d->aspect_dd));
	double ratio = i < G_N_ELEMENTS (aspect_values) ? aspect_values[i] : 0.0;

	if (ratio < 0) {
		/* "Original": the ratio of the (rotated) source */
		if (im->w > 0 && im->h > 0)
			ratio = (d->e.rot90 % 2) ? (double) im->h / im->w : (double) im->w / im->h;
		else
			ratio = 0;
	}
	if (ratio > 0)
		d->e.auto_crop = FALSE;
	if (ratio > 0 && size_linked (d) && aspect_index_for (d->s.width, d->s.height) == (int) i) {
		ic_crop_view_set_aspect (IC_CROP_VIEW (d->crop_view),
		                         (double) d->s.width / d->s.height, TRUE);
		return;
	}
	ic_crop_view_set_aspect (IC_CROP_VIEW (d->crop_view), ratio, FALSE);
}

/* Exact size: the output width × height takes over the crop box's shape.
 * The ratio drop-down goes back to "Free" since the numbers now decide. */
static void
fit_crop_to_output (IcDialog *d)
{
	int idx;
	double ratio;

	if (d->s.width <= 0 || d->s.height <= 0)
		return;
	ratio = (double) d->s.width / d->s.height;
	idx = aspect_index_for (d->s.width, d->s.height);

	d->e.auto_crop = FALSE;
	ic_crop_view_set_aspect (IC_CROP_VIEW (d->crop_view), 0, FALSE);
	ic_crop_view_fit_ratio (IC_CROP_VIEW (d->crop_view), ratio);
	if (idx >= 0) {
		/* a standard ratio (e.g. a preset): select it and lock the crop box */
		set_aspect_dropdown (d, idx);
		ic_crop_view_set_aspect (IC_CROP_VIEW (d->crop_view), ratio, TRUE);
	} else {
		set_aspect_dropdown (d, ASPECT_FREE);  /* keep it free to drag */
	}
}

static void
refresh_crop_constraint (IcDialog *d)
{
	if (d->s.resize == IC_RESIZE_EXACT)
		fit_crop_to_output (d);
	else
		apply_aspect_from_dropdown (d);
}

static void
edit_changed (IcDialog *d)
{
	sync_output_from_crop (d);
	ic_crop_view_update (IC_CROP_VIEW (d->crop_view));
	schedule_preview (d);
}

static void
set_straighten_value (IcDialog *d, double v)
{
	d->updating = TRUE;
	gtk_range_set_value (GTK_RANGE (d->straighten_scale), v);
	d->updating = FALSE;
}

static void
rotate_cb (GtkButton *b, IcDialog *d)
{
	gboolean cw = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "cw"));
	IcEdit *e = &d->e;
	double x = e->cx, y = e->cy, w = e->cw, h = e->ch;

	if (cw) {
		e->rot90 = (e->rot90 + 1) % 4;
		e->cx = 1.0 - (y + h);
		e->cy = x;
	} else {
		e->rot90 = (e->rot90 + 3) % 4;
		e->cx = y;
		e->cy = 1.0 - (x + w);
	}
	e->cw = h;
	e->ch = w;
	apply_aspect_from_dropdown (d);
	edit_changed (d);
}

/* Mirroring the result = mirroring the source and negating the rotation. */
static void
flip_cb (GtkButton *b, IcDialog *d)
{
	gboolean horizontal = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "horizontal"));
	IcEdit *e = &d->e;

	if (horizontal) {
		e->flip_h = !e->flip_h;
		e->cx = 1.0 - e->cx - e->cw;
	} else {
		e->flip_v = !e->flip_v;
		e->cy = 1.0 - e->cy - e->ch;
	}
	e->rot90 = (4 - e->rot90) % 4;
	e->fine = -e->fine;
	set_straighten_value (d, e->fine);
	edit_changed (d);
}

static void
straighten_cb (GtkRange *range, IcDialog *d)
{
	IcImage *im = current_image (d);

	if (d->updating)
		return;
	d->e.fine = gtk_range_get_value (range);
	if (im->w > 0)
		ic_edit_auto_crop (&d->e, im->w, im->h);
	d->e.auto_crop = fabs (d->e.fine) > 1e-6;
	apply_aspect_from_dropdown (d);
	edit_changed (d);
}

static void
aspect_cb (GObject *o, GParamSpec *pspec, IcDialog *d)
{
	if (d->updating)
		return;
	apply_aspect_from_dropdown (d);
	sync_output_from_crop (d);
	if (size_linked (d) &&
	    gtk_drop_down_get_selected (GTK_DROP_DOWN (d->aspect_dd)) == ASPECT_FREE) {
		/* no fixed ratio any more: no fixed resolution either */
		gboolean was = d->updating;
		d->updating = TRUE;
		gtk_drop_down_set_selected (GTK_DROP_DOWN (d->preset_dd), 0);
		d->updating = was;
	}
	schedule_preview (d);
}

static void
reset_edit_cb (GtkButton *b, IcDialog *d)
{
	ic_edit_reset (&d->e);
	set_straighten_value (d, 0);
	gtk_drop_down_set_selected (GTK_DROP_DOWN (d->aspect_dd), 0);
	edit_changed (d);
}

static void
crop_finished_cb (IcCropView *view, IcDialog *d)
{
	d->e.auto_crop = FALSE;
	schedule_preview (d);
}

/* ------------------------------------------------------------------ */
/* Navigation and view switching                                        */
/* ------------------------------------------------------------------ */

static void
navigate_cb (GtkButton *b, IcDialog *d)
{
	int delta = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "delta"));
	int idx = (int) d->current + delta;

	if (idx < 0 || idx >= (int) d->images->len)
		return;
	d->current = idx;
	gtk_picture_set_paintable (GTK_PICTURE (d->preview_picture), NULL);
	show_current_image (d);
	update_sensitivity (d);
	load_thumb (d, d->current);
	run_preview (d);
}

static void
page_changed_cb (GObject *o, GParamSpec *pspec, IcDialog *d)
{
	const char *page = gtk_stack_get_visible_child_name (GTK_STACK (d->stack));
	gboolean edit = g_strcmp0 (page, "edit") == 0;
	gtk_widget_set_visible (d->edit_toolbar, edit);
	gtk_widget_set_visible (d->preview_toolbar, !edit);
}

static void
zoom_cb (GtkToggleButton *t, IcDialog *d)
{
	gboolean actual = gtk_toggle_button_get_active (t);
	gtk_picture_set_can_shrink (GTK_PICTURE (d->preview_picture), !actual);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (d->preview_scrolled),
	                                actual ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER,
	                                actual ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER);
}

/* ------------------------------------------------------------------ */
/* Processing                                                           */
/* ------------------------------------------------------------------ */

static void
finish_processing (IcDialog *d)
{
	d->processing = FALSE;
	dialog_close (d);
}

enum { CHOICE_CANCEL, CHOICE_SKIP, CHOICE_RETRY };

static void
error_button_cb (GtkButton *b, IcDialog *d)
{
	int choice = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "choice"));
	GtkWidget *win = gtk_widget_get_ancestor (GTK_WIDGET (b), GTK_TYPE_WINDOW);
	gboolean batch = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "batch"));

	gtk_window_destroy (GTK_WINDOW (win));

	if (!batch || choice == CHOICE_CANCEL) {
		finish_processing (d);
		return;
	}
	if (choice == CHOICE_SKIP)
		d->proc_index++;
	process_next (d);
}

static void
show_error (IcDialog *d, const char *primary, const char *secondary, gboolean batch)
{
	GtkWidget *win = gtk_window_new ();
	GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
	GtkWidget *title = gtk_label_new (primary);
	GtkWidget *detail = gtk_label_new (secondary);
	GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	struct { const char *label; int choice; } defs[] = {
		{ N_("_Cancel"), CHOICE_CANCEL }, { N_("_Skip"), CHOICE_SKIP }, { N_("_Retry"), CHOICE_RETRY },
	};

	gtk_window_set_title (GTK_WINDOW (win), _("Conversion failed"));
	gtk_window_set_modal (GTK_WINDOW (win), TRUE);
	gtk_window_set_resizable (GTK_WINDOW (win), FALSE);
	gtk_window_set_deletable (GTK_WINDOW (win), FALSE);
	if (d->progress_window)
		gtk_window_set_transient_for (GTK_WINDOW (win), GTK_WINDOW (d->progress_window));

	gtk_widget_set_margin_top (box, 18);
	gtk_widget_set_margin_bottom (box, 18);
	gtk_widget_set_margin_start (box, 18);
	gtk_widget_set_margin_end (box, 18);
	gtk_widget_add_css_class (title, "title-4");
	gtk_label_set_wrap (GTK_LABEL (title), TRUE);
	gtk_label_set_max_width_chars (GTK_LABEL (title), 50);
	gtk_label_set_xalign (GTK_LABEL (title), 0);
	gtk_label_set_wrap (GTK_LABEL (detail), TRUE);
	gtk_label_set_max_width_chars (GTK_LABEL (detail), 60);
	gtk_label_set_selectable (GTK_LABEL (detail), TRUE);
	gtk_label_set_xalign (GTK_LABEL (detail), 0);
	gtk_widget_add_css_class (detail, "dim-label");
	gtk_widget_set_halign (buttons, GTK_ALIGN_END);

	for (guint i = 0; i < G_N_ELEMENTS (defs); i++) {
		GtkWidget *btn;
		if (!batch && defs[i].choice != CHOICE_CANCEL)
			continue;
		btn = gtk_button_new_with_mnemonic (batch ? _(defs[i].label) : _("_Close"));
		g_object_set_data (G_OBJECT (btn), "choice", GINT_TO_POINTER (defs[i].choice));
		g_object_set_data (G_OBJECT (btn), "batch", GINT_TO_POINTER (batch));
		g_signal_connect (btn, "clicked", G_CALLBACK (error_button_cb), d);
		if (defs[i].choice == CHOICE_RETRY)
			gtk_widget_add_css_class (btn, "suggested-action");
		gtk_box_append (GTK_BOX (buttons), btn);
	}

	gtk_box_append (GTK_BOX (box), title);
	if (secondary && *secondary)
		gtk_box_append (GTK_BOX (box), detail);
	gtk_box_append (GTK_BOX (box), buttons);
	gtk_window_set_child (GTK_WINDOW (win), box);
	gtk_window_present (GTK_WINDOW (win));
}

static void
progress_cancel_cb (GtkButton *b, IcDialog *d)
{
	g_cancellable_cancel (d->proc_cancel);
	if (d->proc)
		g_subprocess_force_exit (d->proc);
}

static void
create_progress_window (IcDialog *d)
{
	GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
	GtkWidget *cancel = gtk_button_new_with_mnemonic (_("_Cancel"));

	d->progress_window = gtk_window_new ();
	gtk_window_set_title (GTK_WINDOW (d->progress_window), _("Converting images"));
	gtk_window_set_default_size (GTK_WINDOW (d->progress_window), 420, -1);
	gtk_window_set_resizable (GTK_WINDOW (d->progress_window), FALSE);
	gtk_window_set_deletable (GTK_WINDOW (d->progress_window), FALSE);

	d->progress_label = gtk_label_new ("");
	gtk_label_set_ellipsize (GTK_LABEL (d->progress_label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_xalign (GTK_LABEL (d->progress_label), 0);
	d->progress_bar = gtk_progress_bar_new ();
	gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (d->progress_bar), TRUE);

	gtk_widget_set_halign (cancel, GTK_ALIGN_END);
	g_signal_connect (cancel, "clicked", G_CALLBACK (progress_cancel_cb), d);

	gtk_widget_set_margin_top (box, 18);
	gtk_widget_set_margin_bottom (box, 18);
	gtk_widget_set_margin_start (box, 18);
	gtk_widget_set_margin_end (box, 18);
	gtk_box_append (GTK_BOX (box), d->progress_label);
	gtk_box_append (GTK_BOX (box), d->progress_bar);
	gtk_box_append (GTK_BOX (box), cancel);
	gtk_window_set_child (GTK_WINDOW (d->progress_window), box);
	gtk_window_present (GTK_WINDOW (d->progress_window));
}

/* Oriented size of an image, from the cache or by asking ImageMagick. */
static gboolean
image_dimensions (IcImage *im, int *w, int *h)
{
	if (im->w > 0 && im->h > 0) {
		*w = im->w;
		*h = im->h;
		return TRUE;
	}
	if (ic_identify (im->path, w, h)) {
		im->w = *w;
		im->h = *h;
		return TRUE;
	}
	return FALSE;
}

static char *
strip_extension (const char *basename)
{
	const char *dot = strrchr (basename, '.');
	if (dot == NULL || dot == basename)
		return g_strdup (basename);
	return g_strndup (basename, dot - basename);
}

typedef struct {
	IcDialog *d;
	char     *label;
} ProcJob;

static void
proc_done (GObject *source, GAsyncResult *res, gpointer data)
{
	ProcJob *job = data;
	IcDialog *d = job->d;
	g_autofree char *err = NULL;
	g_autoptr (GError) error = NULL;
	gboolean cancelled;

	g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, NULL, &err, &error);
	g_clear_object (&d->proc);
	cancelled = g_cancellable_is_cancelled (d->proc_cancel);

	if (cancelled || error != NULL || !g_subprocess_get_successful (G_SUBPROCESS (source))) {
		g_unlink (d->proc_tmp);
		if (cancelled) {
			finish_processing (d);
		} else {
			g_autofree char *primary = g_strdup_printf (_("“%s” could not be converted."), job->label);
			show_error (d, primary, error ? error->message : (err ? g_strstrip (err) : ""),
			            d->proc_index != G_MAXUINT);
		}
	} else {
		g_autoptr (GFile) tmp = g_file_new_for_path (d->proc_tmp);
		g_autoptr (GFile) final = g_file_new_for_path (d->proc_final);
		g_autoptr (GError) move_error = NULL;

		if (!g_file_move (tmp, final, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &move_error)) {
			g_autofree char *primary = g_strdup_printf (_("“%s” could not be saved."), job->label);
			g_unlink (d->proc_tmp);
			show_error (d, primary, move_error->message, d->proc_index != G_MAXUINT);
		} else {
			if (d->proc_trash) {
				g_autoptr (GFile) orig = g_file_new_for_path (d->proc_trash);
				if (!g_file_trash (orig, NULL, NULL))
					g_file_delete (orig, NULL, NULL);
			}
			if (d->proc_index == G_MAXUINT) {
				finish_processing (d);  /* combined PDF */
			} else {
				IcImage *im = g_ptr_array_index (d->images, d->proc_index);
				if (im->info)
					nautilus_file_info_invalidate_extension_info (im->info);
				d->proc_index++;
				process_next (d);
			}
		}
	}

	g_free (job->label);
	g_free (job);
	dialog_unref (d);
}

static void
start_job (IcDialog *d, GPtrArray *argv, const char *label)
{
	g_autoptr (GError) error = NULL;
	ProcJob *job;

	g_clear_object (&d->proc);
	d->proc = spawn (argv, &error);
	if (d->proc == NULL) {
		show_error (d, _("ImageMagick could not be started."), error->message, FALSE);
		return;
	}

	job = g_new0 (ProcJob, 1);
	job->d = dialog_ref (d);
	job->label = g_strdup (label);
	g_subprocess_communicate_utf8_async (d->proc, NULL, d->proc_cancel, proc_done, job);
}

static void
process_next (IcDialog *d)
{
	IcImage *im;
	int w = 0, h = 0;
	g_autofree char *dir = NULL, *base = NULL, *stem = NULL, *ext = NULL;
	g_autofree char *final_base = NULL, *tmp_base = NULL, *text = NULL;
	g_autoptr (GPtrArray) argv = NULL;
	gboolean same_file;

	if (d->proc_index >= d->images->len) {
		finish_processing (d);
		return;
	}

	im = g_ptr_array_index (d->images, d->proc_index);
	dir = g_path_get_dirname (im->path);
	base = g_path_get_basename (im->path);
	stem = strip_extension (base);
	ext = ic_output_extension (&d->s, im->path);

	text = g_strdup_printf (_("%u of %u"), d->proc_index + 1, d->images->len);
	gtk_progress_bar_set_text (GTK_PROGRESS_BAR (d->progress_bar), text);
	gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (d->progress_bar),
	                               (double) d->proc_index / d->images->len);
	gtk_label_set_text (GTK_LABEL (d->progress_label), base);

	if (ic_edit_has_crop (&d->e) && !image_dimensions (im, &w, &h)) {
		g_autofree char *primary = g_strdup_printf (_("“%s” could not be read."), base);
		show_error (d, primary, "", TRUE);
		return;
	}

	if (d->s.output == IC_OUTPUT_REPLACE) {
		final_base = g_strdup_printf ("%s.%s", stem, ext);
	} else {
		final_base = g_strdup_printf ("%s%s.%s", stem, d->s.suffix ? d->s.suffix : "", ext);
		if (g_strcmp0 (final_base, base) == 0) {
			g_free (final_base);
			final_base = g_strdup_printf ("%s-converted.%s", stem, ext);
		}
	}
	same_file = g_strcmp0 (final_base, base) == 0;

	g_free (d->proc_final);
	g_free (d->proc_tmp);
	g_clear_pointer (&d->proc_trash, g_free);
	d->proc_final = g_build_filename (dir, final_base, NULL);
	tmp_base = g_strdup_printf (".%s.converting.%s", stem, ext);
	d->proc_tmp = g_build_filename (dir, tmp_base, NULL);
	if (d->s.output == IC_OUTPUT_REPLACE && !same_file)
		d->proc_trash = g_strdup (im->path);

	argv = new_argv ();
	ic_build_ops (argv, &d->s, &d->e, im->path, w, h, ext, FALSE);
	g_ptr_array_add (argv, g_strdup (d->proc_tmp));
	start_job (d, argv, base);
}

static void
process_combined (IcDialog *d)
{
	IcImage *first = g_ptr_array_index (d->images, 0);
	IcImage *last = g_ptr_array_index (d->images, d->images->len - 1);
	g_autofree char *dir = g_path_get_dirname (first->path);
	g_autofree char *b1 = g_path_get_basename (first->path);
	g_autofree char *b2 = g_path_get_basename (last->path);
	g_autofree char *s1 = strip_extension (b1), *s2 = strip_extension (b2);
	g_autofree char *final_base = g_strdup_printf ("%s_to_%s.pdf", s1, s2);
	g_autofree char *tmp_base = g_strdup_printf (".%s.converting.pdf", final_base);
	g_autoptr (GPtrArray) argv = new_argv ();

	d->proc_index = G_MAXUINT;  /* marks the combined job */
	d->proc_final = g_build_filename (dir, final_base, NULL);
	d->proc_tmp = g_build_filename (dir, tmp_base, NULL);

	gtk_label_set_text (GTK_LABEL (d->progress_label), final_base);
	gtk_progress_bar_set_text (GTK_PROGRESS_BAR (d->progress_bar), _("Combining images into a PDF…"));
	gtk_progress_bar_pulse (GTK_PROGRESS_BAR (d->progress_bar));

	for (guint i = 0; i < d->images->len; i++) {
		IcImage *im = g_ptr_array_index (d->images, i);
		int w = 0, h = 0;
		if (ic_edit_has_crop (&d->e))
			image_dimensions (im, &w, &h);
		g_ptr_array_add (argv, g_strdup ("("));
		ic_build_ops (argv, &d->s, &d->e, im->path, w, h, "pdf", TRUE);
		g_ptr_array_add (argv, g_strdup (")"));
	}
	g_ptr_array_add (argv, g_strdup (d->proc_tmp));
	start_job (d, argv, final_base);
}

static void
convert_cb (GtkButton *b, IcDialog *d)
{
	gboolean combine = gtk_widget_get_visible (d->combine_check) &&
	                   gtk_check_button_get_active (GTK_CHECK_BUTTON (d->combine_check));

	settings_from_widgets (d);
	save_profiles (d);  /* remembers the last used profile */

	/* stop the preview */
	if (d->preview_source) {
		g_source_remove (d->preview_source);
		d->preview_source = 0;
	}
	if (d->preview_proc)
		g_subprocess_force_exit (d->preview_proc);

	d->processing = TRUE;
	d->proc_cancel = g_cancellable_new ();
	gtk_widget_set_visible (d->window, FALSE);
	create_progress_window (d);

	if (combine)
		process_combined (d);
	else {
		d->proc_index = 0;
		process_next (d);
	}
}

static gboolean
close_request_cb (GtkWindow *window, IcDialog *d)
{
	if (!d->processing) {
		d->window = NULL;  /* GTK destroys it */
		dialog_close (d);
	}
	return FALSE;
}

static void
cancel_cb (GtkButton *b, IcDialog *d)
{
	gtk_window_close (GTK_WINDOW (d->window));
}

/* ------------------------------------------------------------------ */
/* UI construction                                                      */
/* ------------------------------------------------------------------ */

static void
ensure_css (void)
{
	static gboolean done = FALSE;
	static const char *css =
		".ic-canvas { background-color: #242424; }\n"
		".ic-canvas label { color: #eeeeee; }\n"
		".ic-sidebar { padding: 18px; }\n";
	GtkCssProvider *provider;

	if (done)
		return;
	done = TRUE;
	provider = gtk_css_provider_new ();
#if GTK_CHECK_VERSION (4, 12, 0)
	gtk_css_provider_load_from_string (provider, css);
#else
	gtk_css_provider_load_from_data (provider, css, -1);
#endif
	gtk_style_context_add_provider_for_display (gdk_display_get_default (),
	                                            GTK_STYLE_PROVIDER (provider),
	                                            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref (provider);
}

static GtkWidget *
icon_button (const char *icon, const char *tooltip)
{
	GtkWidget *b = gtk_button_new_from_icon_name (icon);
	gtk_widget_set_tooltip_text (b, tooltip);
	return b;
}

static GtkWidget *
string_dropdown (const char *const *items)
{
	return gtk_drop_down_new_from_strings (items);
}

static GtkWidget *
heading (const char *text)
{
	GtkWidget *l = gtk_label_new (text);
	gtk_label_set_xalign (GTK_LABEL (l), 0);
	gtk_widget_add_css_class (l, "heading");
	gtk_widget_set_margin_top (l, 12);
	return l;
}

static GtkWidget *
row_label (const char *text)
{
	GtkWidget *l = gtk_label_new_with_mnemonic (text);
	gtk_label_set_xalign (GTK_LABEL (l), 0);
	gtk_widget_add_css_class (l, "dim-label");
	return l;
}

static GtkWidget *
build_profile_popover (IcDialog *d)
{
	GtkWidget *popover = gtk_popover_new ();
	GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget *entry_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *add = gtk_button_new_with_label (_("Save"));
	GtkWidget *restore = gtk_button_new_with_label (_("Restore default profiles"));
	GtkWidget *label = gtk_label_new (_("Save current settings as a new profile:"));

	gtk_label_set_xalign (GTK_LABEL (label), 0);
	d->profile_name_entry = gtk_entry_new ();
	gtk_entry_set_placeholder_text (GTK_ENTRY (d->profile_name_entry), _("Profile name"));
	gtk_widget_set_hexpand (d->profile_name_entry, TRUE);
	g_signal_connect (d->profile_name_entry, "activate", G_CALLBACK (profile_save_as_cb), d);
	g_signal_connect (add, "clicked", G_CALLBACK (profile_save_as_cb), d);
	gtk_widget_add_css_class (add, "suggested-action");
	gtk_box_append (GTK_BOX (entry_box), d->profile_name_entry);
	gtk_box_append (GTK_BOX (entry_box), add);

	d->profile_delete_btn = gtk_button_new_with_label (_("Delete this profile"));
	gtk_widget_add_css_class (d->profile_delete_btn, "flat");
	gtk_widget_add_css_class (d->profile_delete_btn, "destructive-action");
	g_signal_connect (d->profile_delete_btn, "clicked", G_CALLBACK (profile_delete_cb), d);

	gtk_widget_add_css_class (restore, "flat");
	g_signal_connect (restore, "clicked", G_CALLBACK (profile_restore_cb), d);

	gtk_box_append (GTK_BOX (box), label);
	gtk_box_append (GTK_BOX (box), entry_box);
	gtk_box_append (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
	gtk_box_append (GTK_BOX (box), d->profile_delete_btn);
	gtk_box_append (GTK_BOX (box), restore);
	gtk_popover_set_child (GTK_POPOVER (popover), box);
	return popover;
}

static GtkWidget *
build_sidebar (IcDialog *d)
{
	GtkWidget *grid = gtk_grid_new ();
	GtkWidget *scrolled = gtk_scrolled_window_new ();
	GtkWidget *profile_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *l;
	int row = 0;

	const char *resize_items[] = { _("Original size"), _("Scale by percentage"),
	                               _("Fit within"), _("Exact size"), NULL };
	const char *format_items[IC_FORMAT_N + 1];
	for (int i = 0; i < IC_FORMAT_N; i++)
		format_items[i] = ic_format_label (i);
	format_items[IC_FORMAT_N] = NULL;

	gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
	gtk_widget_add_css_class (grid, "ic-sidebar");

	/* --- profile --- */
	gtk_grid_attach (GTK_GRID (grid), heading (_("Profile")), 0, row++, 2, 1);

	d->profile_model = gtk_string_list_new (NULL);
	d->profile_dd = gtk_drop_down_new (G_LIST_MODEL (d->profile_model), NULL);
	gtk_widget_set_hexpand (d->profile_dd, TRUE);
	d->profile_save_btn = icon_button ("document-save-symbolic", _("Save changes to this profile"));
	d->profile_menu_btn = gtk_menu_button_new ();
	gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (d->profile_menu_btn), "view-more-symbolic");
	gtk_widget_set_tooltip_text (d->profile_menu_btn, _("Manage profiles"));
	gtk_menu_button_set_popover (GTK_MENU_BUTTON (d->profile_menu_btn), build_profile_popover (d));
	gtk_widget_add_css_class (profile_box, "linked");
	gtk_box_append (GTK_BOX (profile_box), d->profile_dd);
	gtk_box_append (GTK_BOX (profile_box), d->profile_save_btn);
	gtk_box_append (GTK_BOX (profile_box), d->profile_menu_btn);
	gtk_grid_attach (GTK_GRID (grid), profile_box, 0, row++, 2, 1);
	g_signal_connect (d->profile_dd, "notify::selected", G_CALLBACK (profile_selected_cb), d);
	g_signal_connect (d->profile_save_btn, "clicked", G_CALLBACK (profile_save_cb), d);

	d->profile_modified = gtk_label_new (_("Modified — save to keep these settings in the profile"));
	gtk_label_set_xalign (GTK_LABEL (d->profile_modified), 0);
	gtk_label_set_wrap (GTK_LABEL (d->profile_modified), TRUE);
	gtk_widget_add_css_class (d->profile_modified, "dim-label");
	gtk_widget_add_css_class (d->profile_modified, "caption");
	gtk_grid_attach (GTK_GRID (grid), d->profile_modified, 0, row++, 2, 1);

	/* --- size --- */
	gtk_grid_attach (GTK_GRID (grid), heading (_("Size")), 0, row++, 2, 1);

	d->resize_dd = string_dropdown (resize_items);
	gtk_widget_set_hexpand (d->resize_dd, TRUE);
	gtk_grid_attach (GTK_GRID (grid), row_label (_("Resize")), 0, row, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), d->resize_dd, 1, row++, 1, 1);

	d->scale_row_label = row_label (_("Scale (%)"));
	d->scale_spin = gtk_spin_button_new_with_range (1, 1000, 5);
	gtk_grid_attach (GTK_GRID (grid), d->scale_row_label, 0, row, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), d->scale_spin, 1, row++, 1, 1);

	{
		const char *items[G_N_ELEMENTS (size_presets) + 1];
		for (guint i = 0; i < G_N_ELEMENTS (size_presets); i++)
			items[i] = _(size_presets[i].name);
		items[G_N_ELEMENTS (size_presets)] = NULL;
		d->preset_label = row_label (_("Common size"));
		d->preset_dd = string_dropdown (items);
		g_signal_connect (d->preset_dd, "notify::selected", G_CALLBACK (preset_cb), d);
		gtk_grid_attach (GTK_GRID (grid), d->preset_label, 0, row, 1, 1);
		gtk_grid_attach (GTK_GRID (grid), d->preset_dd, 1, row++, 1, 1);
	}

	d->wh_row_label = row_label (_("Width × height"));
	d->wh_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	d->width_spin = gtk_spin_button_new_with_range (1, 100000, 10);
	d->height_spin = gtk_spin_button_new_with_range (1, 100000, 10);
	gtk_widget_set_hexpand (d->width_spin, TRUE);
	gtk_widget_set_hexpand (d->height_spin, TRUE);
	gtk_box_append (GTK_BOX (d->wh_box), d->width_spin);
	gtk_box_append (GTK_BOX (d->wh_box), gtk_label_new ("×"));
	gtk_box_append (GTK_BOX (d->wh_box), d->height_spin);
	{
		GtkWidget *swap = icon_button ("object-rotate-right-symbolic", _("Swap width and height"));
		gtk_widget_add_css_class (swap, "flat");
		g_signal_connect (swap, "clicked", G_CALLBACK (swap_size_cb), d);
		gtk_box_append (GTK_BOX (d->wh_box), swap);
	}
	gtk_grid_attach (GTK_GRID (grid), d->wh_row_label, 0, row, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), d->wh_box, 1, row++, 1, 1);

	d->noup_check = gtk_check_button_new_with_label (_("Don’t enlarge smaller images"));
	gtk_grid_attach (GTK_GRID (grid), d->noup_check, 0, row++, 2, 1);

	/* --- format --- */
	gtk_grid_attach (GTK_GRID (grid), heading (_("Format & compression")), 0, row++, 2, 1);

	d->format_dd = string_dropdown (format_items);
	gtk_grid_attach (GTK_GRID (grid), row_label (_("Format")), 0, row, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), d->format_dd, 1, row++, 1, 1);

	d->quality_label = row_label (_("Quality"));
	d->quality_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 1, 100, 1);
	gtk_scale_set_draw_value (GTK_SCALE (d->quality_scale), TRUE);
	gtk_scale_set_value_pos (GTK_SCALE (d->quality_scale), GTK_POS_RIGHT);
	gtk_scale_set_digits (GTK_SCALE (d->quality_scale), 0);
	gtk_grid_attach (GTK_GRID (grid), d->quality_label, 0, row, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), d->quality_scale, 1, row++, 1, 1);

	d->target_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	d->target_check = gtk_check_button_new_with_label (_("Max. file size (kB)"));
	d->target_spin = gtk_spin_button_new_with_range (1, 1000000, 50);
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->target_spin), 500);
	gtk_widget_set_hexpand (d->target_spin, TRUE);
	gtk_widget_set_tooltip_text (d->target_box, _("JPEG and WebP only. Lowers the quality until the file fits."));
	gtk_box_append (GTK_BOX (d->target_box), d->target_check);
	gtk_box_append (GTK_BOX (d->target_box), d->target_spin);
	gtk_grid_attach (GTK_GRID (grid), d->target_box, 0, row++, 2, 1);

	d->strip_check = gtk_check_button_new_with_label (_("Remove metadata (EXIF, GPS…)"));
	gtk_grid_attach (GTK_GRID (grid), d->strip_check, 0, row++, 2, 1);

	d->combine_check = gtk_check_button_new_with_label (_("Combine all images into a single PDF"));
	gtk_grid_attach (GTK_GRID (grid), d->combine_check, 0, row++, 2, 1);

	/* --- output --- */
	gtk_grid_attach (GTK_GRID (grid), heading (_("Output")), 0, row++, 2, 1);

	d->suffix_radio = gtk_check_button_new_with_label (_("New file, with suffix:"));
	d->suffix_entry = gtk_entry_new ();
	gtk_widget_set_hexpand (d->suffix_entry, TRUE);
	gtk_grid_attach (GTK_GRID (grid), d->suffix_radio, 0, row, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), d->suffix_entry, 1, row++, 1, 1);

	d->replace_radio = gtk_check_button_new_with_label (_("Replace the original"));
	gtk_check_button_set_group (GTK_CHECK_BUTTON (d->replace_radio), GTK_CHECK_BUTTON (d->suffix_radio));
	gtk_grid_attach (GTK_GRID (grid), d->replace_radio, 0, row++, 2, 1);

	l = gtk_label_new (_("When the format changes, the original is moved to the trash."));
	gtk_label_set_wrap (GTK_LABEL (l), TRUE);
	gtk_label_set_xalign (GTK_LABEL (l), 0);
	gtk_widget_add_css_class (l, "dim-label");
	gtk_widget_add_css_class (l, "caption");
	g_object_bind_property (d->replace_radio, "active", l, "visible", G_BINDING_SYNC_CREATE);
	gtk_grid_attach (GTK_GRID (grid), l, 0, row++, 2, 1);

	/* signals */
	g_signal_connect (d->resize_dd, "notify::selected", G_CALLBACK (on_setting_notify), d);
	g_signal_connect (d->format_dd, "notify::selected", G_CALLBACK (on_setting_notify), d);
	g_signal_connect (d->scale_spin, "value-changed", G_CALLBACK (on_setting_changed), d);
	g_signal_connect (d->width_spin, "value-changed", G_CALLBACK (on_setting_changed), d);
	g_signal_connect (d->height_spin, "value-changed", G_CALLBACK (on_setting_changed), d);
	/* react while typing, not only on Enter / focus out */
	g_signal_connect (d->scale_spin, "changed", G_CALLBACK (on_setting_changed), d);
	g_signal_connect (d->width_spin, "changed", G_CALLBACK (on_setting_changed), d);
	g_signal_connect (d->height_spin, "changed", G_CALLBACK (on_setting_changed), d);
	g_signal_connect (d->noup_check, "toggled", G_CALLBACK (on_setting_changed), d);
	g_signal_connect (d->quality_scale, "value-changed", G_CALLBACK (on_setting_changed), d);
	g_signal_connect (d->target_check, "toggled", G_CALLBACK (on_setting_changed), d);
	g_signal_connect (d->target_spin, "value-changed", G_CALLBACK (on_setting_changed), d);
	g_signal_connect (d->strip_check, "toggled", G_CALLBACK (on_setting_changed), d);
	g_signal_connect (d->combine_check, "toggled", G_CALLBACK (on_setting_changed), d);
	g_signal_connect (d->suffix_radio, "toggled", G_CALLBACK (on_setting_changed), d);
	g_signal_connect (d->replace_radio, "toggled", G_CALLBACK (on_setting_changed), d);
	g_signal_connect (d->suffix_entry, "changed", G_CALLBACK (on_setting_changed), d);

	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), grid);
	gtk_widget_set_size_request (scrolled, 360, -1);
	return scrolled;
}

static GtkWidget *
build_edit_toolbar (IcDialog *d)
{
	GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *rot_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *b;
	const char *aspect_items[] = { _("Free"), _("Original ratio"), _("Square (1:1)"),
	                               "4:3", "3:2", "16:9", "4:5", "1.91:1", NULL };

	gtk_widget_add_css_class (rot_box, "linked");

	b = icon_button ("object-rotate-left-symbolic", _("Rotate left"));
	g_object_set_data (G_OBJECT (b), "cw", GINT_TO_POINTER (FALSE));
	g_signal_connect (b, "clicked", G_CALLBACK (rotate_cb), d);
	gtk_box_append (GTK_BOX (rot_box), b);

	b = icon_button ("object-rotate-right-symbolic", _("Rotate right"));
	g_object_set_data (G_OBJECT (b), "cw", GINT_TO_POINTER (TRUE));
	g_signal_connect (b, "clicked", G_CALLBACK (rotate_cb), d);
	gtk_box_append (GTK_BOX (rot_box), b);

	b = icon_button ("object-flip-horizontal-symbolic", _("Flip horizontally"));
	g_object_set_data (G_OBJECT (b), "horizontal", GINT_TO_POINTER (TRUE));
	g_signal_connect (b, "clicked", G_CALLBACK (flip_cb), d);
	gtk_box_append (GTK_BOX (rot_box), b);

	b = icon_button ("object-flip-vertical-symbolic", _("Flip vertically"));
	g_object_set_data (G_OBJECT (b), "horizontal", GINT_TO_POINTER (FALSE));
	g_signal_connect (b, "clicked", G_CALLBACK (flip_cb), d);
	gtk_box_append (GTK_BOX (rot_box), b);

	gtk_box_append (GTK_BOX (box), rot_box);

	gtk_box_append (GTK_BOX (box), gtk_label_new (_("Straighten")));
	d->straighten_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, -45, 45, 0.1);
	gtk_scale_add_mark (GTK_SCALE (d->straighten_scale), 0, GTK_POS_BOTTOM, NULL);
	gtk_scale_set_draw_value (GTK_SCALE (d->straighten_scale), TRUE);
	gtk_scale_set_value_pos (GTK_SCALE (d->straighten_scale), GTK_POS_RIGHT);
	gtk_scale_set_digits (GTK_SCALE (d->straighten_scale), 1);
	gtk_widget_set_hexpand (d->straighten_scale, TRUE);
	gtk_widget_set_size_request (d->straighten_scale, 160, -1);
	gtk_range_set_value (GTK_RANGE (d->straighten_scale), 0);
	g_signal_connect (d->straighten_scale, "value-changed", G_CALLBACK (straighten_cb), d);
	gtk_box_append (GTK_BOX (box), d->straighten_scale);

	gtk_box_append (GTK_BOX (box), gtk_label_new (_("Crop")));
	d->aspect_dd = string_dropdown (aspect_items);
	gtk_widget_set_tooltip_text (d->aspect_dd, _("Crop aspect ratio"));
	g_signal_connect (d->aspect_dd, "notify::selected", G_CALLBACK (aspect_cb), d);
	gtk_box_append (GTK_BOX (box), d->aspect_dd);

	b = gtk_button_new_with_label (_("Reset"));
	gtk_widget_set_tooltip_text (b, _("Undo cropping, rotation and flipping"));
	g_signal_connect (b, "clicked", G_CALLBACK (reset_edit_cb), d);
	gtk_box_append (GTK_BOX (box), b);

	return box;
}

static GtkWidget *
build_main_area (IcDialog *d)
{
	GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *toolbars = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *bottom = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *info_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *overlay, *preview_overlay, *note;

	/* edit page */
	d->crop_view = ic_crop_view_new (&d->e);
	g_signal_connect (d->crop_view, "edit-finished", G_CALLBACK (crop_finished_cb), d);
	g_signal_connect_swapped (d->crop_view, "changed", G_CALLBACK (sync_output_from_crop), d);
	d->crop_spinner = gtk_spinner_new ();
	gtk_spinner_start (GTK_SPINNER (d->crop_spinner));
	gtk_widget_set_size_request (d->crop_spinner, 32, 32);
	gtk_widget_set_halign (d->crop_spinner, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (d->crop_spinner, GTK_ALIGN_CENTER);
	gtk_widget_set_can_target (d->crop_spinner, FALSE);
	overlay = gtk_overlay_new ();
	gtk_overlay_set_child (GTK_OVERLAY (overlay), d->crop_view);
	gtk_overlay_add_overlay (GTK_OVERLAY (overlay), d->crop_spinner);

	/* preview page */
	d->preview_picture = gtk_picture_new ();
	gtk_picture_set_can_shrink (GTK_PICTURE (d->preview_picture), TRUE);
	gtk_widget_set_halign (d->preview_picture, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (d->preview_picture, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top (d->preview_picture, 18);
	gtk_widget_set_margin_bottom (d->preview_picture, 18);
	gtk_widget_set_margin_start (d->preview_picture, 18);
	gtk_widget_set_margin_end (d->preview_picture, 18);
	d->preview_scrolled = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (d->preview_scrolled), GTK_POLICY_NEVER, GTK_POLICY_NEVER);
	gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (d->preview_scrolled), d->preview_picture);
	d->preview_spinner = gtk_spinner_new ();
	gtk_spinner_start (GTK_SPINNER (d->preview_spinner));
	gtk_widget_set_size_request (d->preview_spinner, 32, 32);
	gtk_widget_set_halign (d->preview_spinner, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (d->preview_spinner, GTK_ALIGN_CENTER);
	gtk_widget_set_can_target (d->preview_spinner, FALSE);
	d->preview_error = gtk_label_new ("");
	gtk_label_set_wrap (GTK_LABEL (d->preview_error), TRUE);
	gtk_widget_set_valign (d->preview_error, GTK_ALIGN_END);
	gtk_widget_set_margin_bottom (d->preview_error, 24);
	gtk_widget_set_visible (d->preview_error, FALSE);
	preview_overlay = gtk_overlay_new ();
	gtk_overlay_set_child (GTK_OVERLAY (preview_overlay), d->preview_scrolled);
	gtk_overlay_add_overlay (GTK_OVERLAY (preview_overlay), d->preview_spinner);
	gtk_overlay_add_overlay (GTK_OVERLAY (preview_overlay), d->preview_error);

	d->stack = gtk_stack_new ();
	gtk_stack_set_transition_type (GTK_STACK (d->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_titled (GTK_STACK (d->stack), overlay, "edit", _("Crop & Rotate"));
	gtk_stack_add_titled (GTK_STACK (d->stack), preview_overlay, "preview", _("Preview"));
	gtk_widget_set_vexpand (d->stack, TRUE);
	gtk_widget_add_css_class (d->stack, "ic-canvas");
	g_signal_connect (d->stack, "notify::visible-child-name", G_CALLBACK (page_changed_cb), d);

	/* toolbars */
	d->edit_toolbar = build_edit_toolbar (d);
	gtk_widget_set_hexpand (d->edit_toolbar, TRUE);
	d->preview_toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	d->zoom_toggle = gtk_toggle_button_new_with_label (_("Actual size (100%)"));
	gtk_widget_set_tooltip_text (d->zoom_toggle, _("Inspect compression artefacts at full resolution"));
	g_signal_connect (d->zoom_toggle, "toggled", G_CALLBACK (zoom_cb), d);
	gtk_box_append (GTK_BOX (d->preview_toolbar), d->zoom_toggle);
	gtk_widget_set_visible (d->preview_toolbar, FALSE);
	gtk_box_append (GTK_BOX (toolbars), d->edit_toolbar);
	gtk_box_append (GTK_BOX (toolbars), d->preview_toolbar);
	gtk_widget_set_size_request (toolbars, -1, 44);
	gtk_widget_set_valign (d->edit_toolbar, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (d->preview_toolbar, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top (toolbars, 8);
	gtk_widget_set_margin_bottom (toolbars, 4);
	gtk_widget_set_margin_start (toolbars, 12);
	gtk_widget_set_margin_end (toolbars, 12);

	/* bottom bar: navigation + sizes */
	d->nav_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class (d->nav_box, "linked");
	d->prev_btn = icon_button ("go-previous-symbolic", _("Previous image"));
	d->next_btn = icon_button ("go-next-symbolic", _("Next image"));
	g_object_set_data (G_OBJECT (d->prev_btn), "delta", GINT_TO_POINTER (-1));
	g_object_set_data (G_OBJECT (d->next_btn), "delta", GINT_TO_POINTER (1));
	g_signal_connect (d->prev_btn, "clicked", G_CALLBACK (navigate_cb), d);
	g_signal_connect (d->next_btn, "clicked", G_CALLBACK (navigate_cb), d);
	gtk_box_append (GTK_BOX (d->nav_box), d->prev_btn);
	gtk_box_append (GTK_BOX (d->nav_box), d->next_btn);
	gtk_widget_set_valign (d->nav_box, GTK_ALIGN_CENTER);
	gtk_widget_set_visible (d->nav_box, d->images->len > 1);

	d->nav_label = gtk_label_new ("");
	gtk_label_set_xalign (GTK_LABEL (d->nav_label), 0);
	gtk_label_set_ellipsize (GTK_LABEL (d->nav_label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_add_css_class (d->nav_label, "heading");
	d->orig_label = gtk_label_new ("");
	d->result_label = gtk_label_new ("");
	gtk_label_set_xalign (GTK_LABEL (d->orig_label), 0);
	gtk_label_set_xalign (GTK_LABEL (d->result_label), 0);
	gtk_label_set_ellipsize (GTK_LABEL (d->orig_label), PANGO_ELLIPSIZE_END);
	gtk_label_set_ellipsize (GTK_LABEL (d->result_label), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class (d->orig_label, "dim-label");
	gtk_box_append (GTK_BOX (info_box), d->nav_label);
	gtk_box_append (GTK_BOX (info_box), d->orig_label);
	gtk_box_append (GTK_BOX (info_box), d->result_label);
	gtk_widget_set_hexpand (info_box, TRUE);

	gtk_box_append (GTK_BOX (bottom), d->nav_box);
	gtk_box_append (GTK_BOX (bottom), info_box);
	gtk_widget_set_margin_top (bottom, 6);
	gtk_widget_set_margin_bottom (bottom, 10);
	gtk_widget_set_margin_start (bottom, 12);
	gtk_widget_set_margin_end (bottom, 12);

	gtk_box_append (GTK_BOX (vbox), d->stack);
	gtk_box_append (GTK_BOX (vbox), toolbars);
	if (d->images->len > 1) {
		g_autofree char *text = g_strdup_printf (_("Cropping and rotation apply to all %u selected images."),
		                                         d->images->len);
		note = gtk_label_new (text);
		gtk_widget_add_css_class (note, "caption");
		gtk_widget_add_css_class (note, "dim-label");
		gtk_label_set_xalign (GTK_LABEL (note), 0);
		gtk_widget_set_margin_start (note, 12);
		gtk_box_append (GTK_BOX (vbox), note);
	}
	gtk_box_append (GTK_BOX (vbox), bottom);
	gtk_widget_set_hexpand (vbox, TRUE);
	return vbox;
}

static void
build_window (IcDialog *d)
{
	GtkWidget *header = gtk_header_bar_new ();
	GtkWidget *switcher = gtk_stack_switcher_new ();
	GtkWidget *cancel = gtk_button_new_with_mnemonic (_("_Cancel"));
	GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *main_area, *sidebar;
	GtkEventController *shortcuts;

	ensure_css ();

	d->window = gtk_window_new ();
	gtk_window_set_title (GTK_WINDOW (d->window), _("Resize/convert image"));
	gtk_window_set_default_size (GTK_WINDOW (d->window), 1180, 760);

	d->convert_btn = gtk_button_new_with_label (_("Convert"));
	gtk_widget_add_css_class (d->convert_btn, "suggested-action");
	g_signal_connect (d->convert_btn, "clicked", G_CALLBACK (convert_cb), d);
	g_signal_connect (cancel, "clicked", G_CALLBACK (cancel_cb), d);

	main_area = build_main_area (d);
	sidebar = build_sidebar (d);

	gtk_stack_switcher_set_stack (GTK_STACK_SWITCHER (switcher), GTK_STACK (d->stack));
	gtk_header_bar_set_title_widget (GTK_HEADER_BAR (header), switcher);
	gtk_header_bar_set_show_title_buttons (GTK_HEADER_BAR (header), FALSE);
	gtk_header_bar_pack_start (GTK_HEADER_BAR (header), cancel);
	gtk_header_bar_pack_end (GTK_HEADER_BAR (header), d->convert_btn);
	gtk_window_set_titlebar (GTK_WINDOW (d->window), header);

	gtk_box_append (GTK_BOX (hbox), main_area);
	gtk_box_append (GTK_BOX (hbox), gtk_separator_new (GTK_ORIENTATION_VERTICAL));
	gtk_box_append (GTK_BOX (hbox), sidebar);
	gtk_window_set_child (GTK_WINDOW (d->window), hbox);

	shortcuts = gtk_shortcut_controller_new ();
	gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
		gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
		                  gtk_named_action_new ("window.close")));
	gtk_widget_add_controller (d->window, shortcuts);

	g_signal_connect (d->window, "close-request", G_CALLBACK (close_request_cb), d);
}

/* ------------------------------------------------------------------ */

void
ic_dialog_show (GList *files)
{
	IcDialog *d;
	g_autofree char *last = NULL;
	guint profile_idx = 0;

	if (files == NULL)
		return;

	d = g_rc_box_new0 (IcDialog);
	d->images = g_ptr_array_new_with_free_func ((GDestroyNotify) image_free);
	d->cancellable = g_cancellable_new ();
	d->tmpdir = g_dir_make_tmp ("nautilus-image-converter-XXXXXX", NULL);
	ic_edit_reset (&d->e);

	for (GList *l = files; l; l = l->next) {
		NautilusFileInfo *info = l->data;
		g_autoptr (GFile) loc = nautilus_file_info_get_location (info);
		char *path = g_file_get_path (loc);
		IcImage *im;
		GStatBuf st;

		if (path == NULL)
			continue;
		im = g_new0 (IcImage, 1);
		im->path = path;
		im->info = g_object_ref (info);
		im->size = g_stat (path, &st) == 0 ? st.st_size : 0;
		g_ptr_array_add (d->images, im);
	}

	if (d->images->len == 0 || d->tmpdir == NULL) {
		dialog_unref (d);
		return;
	}

	d->profiles = ic_profiles_load (&last);
	for (guint i = 0; last && i < d->profiles->len; i++)
		if (g_strcmp0 (((IcProfile *) g_ptr_array_index (d->profiles, i))->name, last) == 0)
			profile_idx = i;

	build_window (d);
	d->profile_idx = profile_idx;
	rebuild_profile_model (d);
	select_profile (d, profile_idx);
	show_current_image (d);

	gtk_window_present (GTK_WINDOW (d->window));
	load_thumb (d, 0);
}
