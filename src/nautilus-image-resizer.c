/*
 *  nautilus-image-resizer.c
 *
 *  Copyright (C) 2004-2008 Jürg Billeter
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  Author: Jürg Billeter <j@bitron.ch>
 *
 */

#include <config.h> /* for GETTEXT_PACKAGE */

#include "nautilus-image-resizer.h"

#include <string.h>
#include <math.h>

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

#include <nautilus-extension.h>

typedef struct _NautilusImageResizerPrivate NautilusImageResizerPrivate;

struct _NautilusImageResizerPrivate
{
	GList *files;

	gchar *suffix;

	int images_resized;
	int images_total;

	gchar *size;

	GtkDialog *resize_dialog;
	GtkCheckButton *default_size_radiobutton;
	GtkDropDown *size_combobox;
	GtkCheckButton *custom_pct_radiobutton;
	GtkSpinButton *pct_spinbutton;
	GtkCheckButton *custom_size_radiobutton;
	GtkSpinButton *width_spinbutton;
	GtkSpinButton *height_spinbutton;
	GtkCheckButton *append_radiobutton;
	GtkEntry *name_entry;
	GtkCheckButton *inplace_radiobutton;

	GtkWidget *progress_dialog;
	GtkWidget *progress_bar;
	GtkWidget *progress_label;

	GtkCheckButton *target_size_radiobutton;
	GtkSpinButton *target_size_spinbutton;
	GtkDropDown *target_size_unit_combobox;
	GtkCheckButton *pad_target_size_checkbutton;

	gint target_size_kb;
	gboolean use_target_size;
	gboolean pad_target_size;

	/* Scratch path that the (possibly multi-write) target-size tool writes
	 * to. We only ever rename FROM this scratch path ONTO the path Nautilus
	 * is actually watching, once, after the external process has fully
	 * exited — so Nautilus's file monitor only ever sees a single clean
	 * write/rename on the visible path, same as the plain -resize path. */
	gchar *scratch_filename;
};

#define NAUTILUS_IMAGE_RESIZER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE((o), NAUTILUS_TYPE_IMAGE_RESIZER, NautilusImageResizerPrivate))

G_DEFINE_TYPE_WITH_PRIVATE(NautilusImageResizer, nautilus_image_resizer, G_TYPE_OBJECT)

enum
{
	PROP_FILES = 1,
};

typedef enum
{
	/* Place Signal Types Here */
	SIGNAL_TYPE_EXAMPLE,
	LAST_SIGNAL
} NautilusImageResizerSignalType;

static void
nautilus_image_resizer_finalize(GObject *object)
{
	NautilusImageResizer *dialog = NAUTILUS_IMAGE_RESIZER(object);
	NautilusImageResizerPrivate *priv = nautilus_image_resizer_get_instance_private(dialog);

	g_free(priv->suffix);
	g_free(priv->scratch_filename);

	G_OBJECT_CLASS(nautilus_image_resizer_parent_class)->finalize(object);
}

static void
nautilus_image_resizer_set_property(GObject *object,
									guint property_id,
									const GValue *value,
									GParamSpec *pspec)
{
	NautilusImageResizer *dialog = NAUTILUS_IMAGE_RESIZER(object);
	NautilusImageResizerPrivate *priv = nautilus_image_resizer_get_instance_private(dialog);

	switch (property_id)
	{
	case PROP_FILES:
		priv->files = g_value_get_pointer(value);
		priv->images_total = g_list_length(priv->files);
		break;
	default:
		/* We don't have any other property... */
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void
nautilus_image_resizer_get_property(GObject *object,
									guint property_id,
									GValue *value,
									GParamSpec *pspec)
{
	NautilusImageResizer *self = NAUTILUS_IMAGE_RESIZER(object);
	NautilusImageResizerPrivate *priv = nautilus_image_resizer_get_instance_private(self);

	switch (property_id)
	{
	case PROP_FILES:
		g_value_set_pointer(value, priv->files);
		break;
	default:
		/* We don't have any other property... */
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void
nautilus_image_resizer_class_init(NautilusImageResizerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GParamSpec *files_param_spec;

	object_class->finalize = nautilus_image_resizer_finalize;
	object_class->set_property = nautilus_image_resizer_set_property;
	object_class->get_property = nautilus_image_resizer_get_property;

	files_param_spec = g_param_spec_pointer("files",
											"Files",
											"Set selected files",
											G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE);

	g_object_class_install_property(object_class,
									PROP_FILES,
									files_param_spec);
}

static void run_op(NautilusImageResizer *resizer);

static GFile *
nautilus_image_resizer_transform_filename(NautilusImageResizer *resizer, GFile *orig_file)
{
	NautilusImageResizerPrivate *priv = nautilus_image_resizer_get_instance_private(resizer);

	GFile *parent_file, *new_file;
	char *basename, *extension, *new_basename;

	g_return_val_if_fail(G_IS_FILE(orig_file), NULL);

	parent_file = g_file_get_parent(orig_file);

	basename = g_strdup(g_file_get_basename(orig_file));

	extension = g_strdup(strrchr(basename, '.'));
	if (extension != NULL)
		basename[strlen(basename) - strlen(extension)] = '\0';

	new_basename = g_strdup_printf("%s%s%s", basename,
								   priv->suffix == NULL ? ".tmp" : priv->suffix,
								   extension == NULL ? "" : extension);
	g_free(basename);
	g_free(extension);

	new_file = g_file_get_child(parent_file, new_basename);

	g_object_unref(parent_file);
	g_free(new_basename);

	return new_file;
}

/* Build a private scratch path next to `path`, preserving its extension
 * (tools like `convert`/`jpegoptim` may infer format from the extension).
 * e.g. "/dir/photo.tmp.jpg" -> "/dir/photo.tmp.scratch.jpg" */
static gchar *
build_scratch_filename(const gchar *path)
{
	gchar *dir = g_path_get_dirname(path);
	gchar *base = g_path_get_basename(path);
	gchar *dot = strrchr(base, '.');
	gchar *new_base;
	gchar *result;

	if (dot != NULL)
	{
		gchar *prefix = g_strndup(base, dot - base);
		new_base = g_strdup_printf("%s.scratch%s", prefix, dot);
		g_free(prefix);
	}
	else
	{
		new_base = g_strdup_printf("%s.scratch", base);
	}

	result = g_build_filename(dir, new_base, NULL);

	g_free(dir);
	g_free(base);
	g_free(new_base);

	return result;
}

static void
retry_dialog_cb(GtkDialog *dialog,
				gint response_id,
				gpointer user_data)
{
	NautilusImageResizer *resizer = NAUTILUS_IMAGE_RESIZER(user_data);
	NautilusImageResizerPrivate *priv = nautilus_image_resizer_get_instance_private(resizer);

	gtk_window_destroy(GTK_WINDOW(dialog));

	if (response_id == GTK_RESPONSE_CANCEL)
	{
		gtk_window_destroy(GTK_WINDOW(priv->progress_dialog));
		return;
	}
	else if (response_id == 1)
	{
		priv->images_resized++;
		priv->files = priv->files->next;
	}

	if (priv->files != NULL)
	{
		/* process next image */
		run_op(resizer);
	}
	else
	{
		/* cancel/terminate operation */
		gtk_window_destroy(GTK_WINDOW(priv->progress_dialog));
	}
}

static void
pad_file_to_target_size(const gchar *filepath, gint target_size_kb)
{
	if (filepath == NULL || target_size_kb <= 0)
		return;

	GStatBuf st;
	if (g_stat(filepath, &st) == 0)
	{
		goffset target_bytes = (goffset)target_size_kb * 1000;
		if (st.st_size < target_bytes)
		{
			goffset pad_needed = target_bytes - st.st_size;
			FILE *f = g_fopen(filepath, "ab");
			if (f != NULL)
			{
				char buffer[4096] = {0};
				while (pad_needed > 0)
				{
					size_t to_write = (pad_needed > sizeof(buffer)) ? sizeof(buffer) : (size_t)pad_needed;
					size_t written = fwrite(buffer, 1, to_write, f);
					if (written == 0)
						break;
					pad_needed -= written;
				}
				fclose(f);
			}
		}
	}
}

static void
op_finished(GPid pid, gint status, gpointer data)
{
	NautilusImageResizer *resizer = NAUTILUS_IMAGE_RESIZER(data);
	NautilusImageResizerPrivate *priv = nautilus_image_resizer_get_instance_private(resizer);

	NautilusFileInfo *file = NAUTILUS_FILE_INFO(priv->files->data);

	if (status != 0)
	{
		/* resizing failed */
		char *name = nautilus_file_info_get_name(file);
		GtkWidget *msg_dialog = gtk_message_dialog_new(GTK_WINDOW(priv->progress_dialog),
													   GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
													   GTK_BUTTONS_NONE,
													   "'%s' cannot be resized. Check whether you have permission to write to this folder.",
													   name);
		g_free(name);

		gtk_dialog_add_button(GTK_DIALOG(msg_dialog), _("_Skip"), 1);
		gtk_dialog_add_button(GTK_DIALOG(msg_dialog), _("_Cancel"), GTK_RESPONSE_CANCEL);
		gtk_dialog_add_button(GTK_DIALOG(msg_dialog), _("_Retry"), 0);
		gtk_dialog_set_default_response(GTK_DIALOG(msg_dialog), 0);

		g_signal_connect(msg_dialog, "response", G_CALLBACK(retry_dialog_cb), data);
		gtk_widget_show(msg_dialog);
		return;
	}
	else
	{
		GFile *orig_location = nautilus_file_info_get_location(file);
		GFile *new_location = nautilus_image_resizer_transform_filename(resizer, orig_location);

		if (priv->use_target_size && priv->pad_target_size)
		{
			if (priv->scratch_filename != NULL)
			{
				pad_file_to_target_size(priv->scratch_filename, priv->target_size_kb);
			}
			else
			{
				char *new_filename = g_file_get_path(new_location);
				pad_file_to_target_size(new_filename, priv->target_size_kb);
				g_free(new_filename);
			}
		}

		if (priv->scratch_filename != NULL)
		{
			/* The target-size tool (jpegoptim / convert -define extent)
			 * writes iteratively and may touch its output file several
			 * times while converging on a size. It was pointed at a
			 * throwaway scratch path, not the path Nautilus is watching.
			 * Now that the process has fully exited, do ONE clean atomic
			 * rename from scratch onto the real visible path, so Nautilus's
			 * file monitor only ever sees a single write on it — same as
			 * the plain -resize path. */
			GFile *scratch_location = g_file_new_for_path(priv->scratch_filename);
			g_file_move(scratch_location, new_location, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);
			g_object_unref(scratch_location);

			g_free(priv->scratch_filename);
			priv->scratch_filename = NULL;
		}

		if (priv->suffix == NULL)
		{
			/* resize image in place — covers normal resize AND target-size
			 * mode, since new_location now holds the final result and this
			 * move step puts it back over the original. */
			g_file_move(new_location, orig_location, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);

			/* Belt-and-braces: explicitly tell Nautilus this file's info is
			 * stale so it re-stats it, rather than relying solely on the
			 * directory monitor picking up the rename. */
			nautilus_file_info_invalidate_extension_info(file);
		}

		g_object_unref(orig_location);
		g_object_unref(new_location);
	}

	priv->images_resized++;
	priv->files = priv->files->next;

	if (priv->files != NULL)
	{
		/* process next image */
		run_op(resizer);
	}
	else
	{
		/* cancel/terminate operation */
		gtk_window_destroy(GTK_WINDOW(priv->progress_dialog));
	}
}
static void
run_op(NautilusImageResizer *resizer)
{
	NautilusImageResizerPrivate *priv =
		nautilus_image_resizer_get_instance_private(resizer);

	g_return_if_fail(priv->files != NULL);

	NautilusFileInfo *file = NAUTILUS_FILE_INFO(priv->files->data);

	GFile *orig_location = nautilus_file_info_get_location(file);
	char *filename = g_file_get_path(orig_location);
	GFile *new_location =
		nautilus_image_resizer_transform_filename(resizer, orig_location);
	char *new_filename = g_file_get_path(new_location);

	g_object_unref(orig_location);
	g_object_unref(new_location);

	/* FIXME: check whether new_uri already exists and provide
	 * "Replace All", "Skip", and "Replace" options.
	 */

	gchar *argv[6];
	pid_t pid;
	gboolean spawn_success = FALSE;

	/* Clear out any leftover scratch path from a previous run on this
	 * image (e.g. after a retry). */
	g_clear_pointer(&priv->scratch_filename, g_free);

	if (priv->use_target_size)
	{
		gchar *mime_type = nautilus_file_info_get_mime_type(file);
		gchar *jpegoptim_path = g_find_program_in_path("jpegoptim");
		gchar *pngquant_path = g_find_program_in_path("pngquant");

		gboolean jpegoptim_available = (jpegoptim_path != NULL);
		gboolean pngquant_available = (pngquant_path != NULL);
		g_free(jpegoptim_path);
		g_free(pngquant_path);

		/* Both jpegoptim/pngquant and convert write their output to a
		 * private scratch path instead; op_finished() does a single clean
		 * atomic rename from scratch onto new_filename only after the
		 * external process has fully exited. */
		priv->scratch_filename = build_scratch_filename(new_filename);

		if ((g_strcmp0(mime_type, "image/jpeg") == 0 || g_strcmp0(mime_type, "image/pjpeg") == 0) &&
			jpegoptim_available)
		{
			gchar *size_arg =
				g_strdup_printf("--size=%dk", priv->target_size_kb);

			gchar *filename_q = g_shell_quote(filename);
			gchar *scratch_q = g_shell_quote(priv->scratch_filename);

			gchar *command =
				g_strdup_printf(
					"cp %s %s && "
					"(jpegoptim %s %s || true)",
					filename_q,
					scratch_q,
					size_arg,
					scratch_q);

			argv[0] = "/bin/sh";
			argv[1] = "-c";
			argv[2] = command;
			argv[3] = NULL;

			spawn_success =
				g_spawn_async(NULL,
							  argv,
							  NULL,
							  G_SPAWN_DO_NOT_REAP_CHILD,
							  NULL,
							  NULL,
							  &pid,
							  NULL);

			g_free(filename_q);
			g_free(scratch_q);
			g_free(command);
			g_free(size_arg);
		}
		else if (g_strcmp0(mime_type, "image/jpeg") == 0 || g_strcmp0(mime_type, "image/pjpeg") == 0)
		{
			gchar *define_arg =
				g_strdup_printf("jpeg:extent=%dkb", priv->target_size_kb);

			argv[0] = "/usr/bin/convert";
			argv[1] = filename;
			argv[2] = "-define";
			argv[3] = define_arg;
			argv[4] = priv->scratch_filename;
			argv[5] = NULL;

			spawn_success =
				g_spawn_async(NULL,
							  argv,
							  NULL,
							  G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
							  NULL,
							  NULL,
							  &pid,
							  NULL);

			g_free(define_arg);
		}
		else if (g_strcmp0(mime_type, "image/png") == 0)
		{
			GStatBuf st;
			goffset file_size_bytes = 0;
			if (g_stat(filename, &st) == 0)
			{
				file_size_bytes = st.st_size;
			}

			gint orig_size_kb = (gint)(file_size_bytes / 1000);
			if (orig_size_kb <= 0) orig_size_kb = 1;

			gint ratio = (priv->target_size_kb * 100) / orig_size_kb;

			gchar *filename_q = g_shell_quote(filename);
			gchar *scratch_q = g_shell_quote(priv->scratch_filename);
			gchar *command = NULL;

			if (pngquant_available)
			{
				if (ratio >= 100)
				{
					/* Target size is larger than original: minimal/lossless touch */
					command = g_strdup_printf(
						"pngquant --quality=85-100 --strip --force --output %s %s || cp %s %s",
						scratch_q, filename_q, filename_q, scratch_q);
				}
				else if (ratio >= 65)
				{
					/* Light reduction: high quality palette quantization */
					command = g_strdup_printf(
						"pngquant --quality=60-90 --strip --force --output %s %s || cp %s %s",
						scratch_q, filename_q, filename_q, scratch_q);
				}
				else if (ratio >= 30)
				{
					/* Medium reduction: dynamic quality mapping */
					gint max_q = CLAMP(ratio + 10, 30, 75);
					gint min_q = CLAMP(max_q - 25, 15, 50);
					command = g_strdup_printf(
						"pngquant --quality=%d-%d --strip --force --output %s %s || cp %s %s",
						min_q, max_q, scratch_q, filename_q, filename_q, scratch_q);
				}
				else
				{
					/* Heavy reduction: maximum palette quantization without modifying resolution */
					command = g_strdup_printf(
						"pngquant --quality=10-35 --strip --force --output %s %s || cp %s %s",
						scratch_q, filename_q, filename_q, scratch_q);
				}
			}
			else
			{
				/* Fallback when pngquant is not installed: 8-bit palette conversion (preserves resolution) */
				command = g_strdup_printf(
					"/usr/bin/convert %s -strip PNG8:%s || cp %s %s",
					filename_q, scratch_q, filename_q, scratch_q);
			}

			argv[0] = "/bin/sh";
			argv[1] = "-c";
			argv[2] = command;
			argv[3] = NULL;

			spawn_success =
				g_spawn_async(NULL,
							  argv,
							  NULL,
							  G_SPAWN_DO_NOT_REAP_CHILD,
							  NULL,
							  NULL,
							  &pid,
							  NULL);

			g_free(filename_q);
			g_free(scratch_q);
			g_free(command);
		}
		else
		{
			g_clear_pointer(&priv->scratch_filename, g_free);
			g_warning("Unsupported file type for target size: %s", mime_type);
			spawn_success = FALSE;
		}

		g_free(mime_type);
	}
	else
	{
		/* Existing resize behaviour */

		argv[0] = "/usr/bin/convert";
		argv[1] = filename;
		argv[2] = "-resize";
		argv[3] = priv->size;
		argv[4] = new_filename;
		argv[5] = NULL;

		spawn_success =
			g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, NULL);
	}

	if (!spawn_success)
	{
		g_clear_pointer(&priv->scratch_filename, g_free);
		g_free(filename);
		g_free(new_filename);
		g_warning("Failed to spawn command");
		/* FIXME: We should probably call op_finished with an error */
		return;
	}

	g_free(filename);
	g_free(new_filename);

	g_child_watch_add(pid, op_finished, resizer);

	char *tmp;

	gtk_progress_bar_set_fraction(
		GTK_PROGRESS_BAR(priv->progress_bar),
		(double)(priv->images_resized + 1) / priv->images_total);

	tmp = g_strdup_printf(_("Resizing image: %d of %d"), priv->images_resized + 1, priv->images_total);

	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(priv->progress_bar), tmp);
	g_free(tmp);

	char *name = nautilus_file_info_get_name(file);

	tmp = g_strdup_printf(_("<i>Resizing \"%s\"</i>"), name);

	g_free(name);

	gtk_label_set_markup(GTK_LABEL(priv->progress_label), tmp);

	g_free(tmp);
}

static void
nautilus_image_resizer_response_cb(GtkDialog *dialog, gint response_id, gpointer user_data)
{
	NautilusImageResizer *resizer = NAUTILUS_IMAGE_RESIZER(user_data);
	NautilusImageResizerPrivate *priv =
		nautilus_image_resizer_get_instance_private(resizer);

	if (response_id == GTK_RESPONSE_OK)
	{
		/* Reset operation-specific state */
		priv->use_target_size = FALSE;
		priv->pad_target_size = FALSE;

		g_clear_pointer(&priv->suffix, g_free);
		g_clear_pointer(&priv->size, g_free);

		if (gtk_check_button_get_active(priv->append_radiobutton))
		{
			if (strlen(gtk_editable_get_text(GTK_EDITABLE(priv->name_entry))) == 0)
			{
				GtkWidget *msg_dialog =
					gtk_message_dialog_new(GTK_WINDOW(dialog),
										   GTK_DIALOG_MODAL,
										   GTK_MESSAGE_ERROR,
										   GTK_BUTTONS_OK,
										   _("Please enter a valid filename suffix!"));

				gtk_window_set_transient_for(GTK_WINDOW(msg_dialog),
											 GTK_WINDOW(priv->resize_dialog));

				g_signal_connect(msg_dialog,
								 "response",
								 G_CALLBACK(gtk_window_destroy),
								 NULL);

				gtk_widget_show(msg_dialog);
				return;
			}

			priv->suffix =
				g_strdup(gtk_editable_get_text(GTK_EDITABLE(priv->name_entry)));
		}

		if (gtk_check_button_get_active(priv->default_size_radiobutton))
		{
			GtkStringObject *item =
				GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(priv->size_combobox));
			if (item != NULL)
			{
				priv->size = g_strdup(gtk_string_object_get_string(item));
			}
		}
		else if (gtk_check_button_get_active(priv->custom_pct_radiobutton))
		{
			priv->size =
				g_strdup_printf("%d%%",
								(int)gtk_spin_button_get_value(priv->pct_spinbutton));
		}
		else if (gtk_check_button_get_active(priv->custom_size_radiobutton))
		{
			priv->size =
				g_strdup_printf("%dx%d",
								(int)gtk_spin_button_get_value(priv->width_spinbutton),
								(int)gtk_spin_button_get_value(priv->height_spinbutton));
		}
		else if (gtk_check_button_get_active(priv->target_size_radiobutton))
		{
			gint size_val =
				gtk_spin_button_get_value_as_int(priv->target_size_spinbutton);

			guint active_unit =
				gtk_drop_down_get_selected(priv->target_size_unit_combobox);

			/* KB = index 0, MB = index 1 (using base 1000 to match GNOME/Nautilus SI formatting) */
			priv->target_size_kb =
				(active_unit == 1) ? size_val * 1000 : size_val;

			priv->pad_target_size =
				gtk_check_button_get_active(priv->pad_target_size_checkbutton);

			priv->use_target_size = TRUE;
		}

		gtk_widget_show(priv->progress_dialog);
		run_op(resizer);
	}

	gtk_window_destroy(GTK_WINDOW(dialog));
}

static void
nautilus_image_resizer_init(NautilusImageResizer *resizer)
{
	NautilusImageResizerPrivate *priv = nautilus_image_resizer_get_instance_private(resizer);

	GtkBuilder *ui;
	gchar *path;
	guint result;
	GError *err = NULL;
	GtkWidget *progress_box;

	/* Let's create our gtkbuilder and load the xml file */
	ui = gtk_builder_new();
	gtk_builder_set_translation_domain(ui, GETTEXT_PACKAGE);
	path = g_build_filename(DATADIR, PACKAGE, "nautilus-image-resize.ui", NULL);
	result = gtk_builder_add_from_file(ui, path, &err);
	g_free(path);

	/* If we're unable to load the xml file */
	if (result == 0)
	{
		g_warning("%s", err->message);
		g_error_free(err);
		return;
	}

	/* Grab some widgets */
	priv->resize_dialog = GTK_DIALOG(gtk_builder_get_object(ui, "resize_dialog"));
	priv->default_size_radiobutton =
		GTK_CHECK_BUTTON(gtk_builder_get_object(ui, "default_size_radiobutton"));
	priv->size_combobox = GTK_DROP_DOWN(gtk_builder_get_object(ui, "comboboxtext_size"));
	priv->custom_pct_radiobutton =
		GTK_CHECK_BUTTON(gtk_builder_get_object(ui, "custom_pct_radiobutton"));
	priv->pct_spinbutton = GTK_SPIN_BUTTON(gtk_builder_get_object(ui, "pct_spinbutton"));
	priv->custom_size_radiobutton =
		GTK_CHECK_BUTTON(gtk_builder_get_object(ui, "custom_size_radiobutton"));
	priv->width_spinbutton = GTK_SPIN_BUTTON(gtk_builder_get_object(ui, "width_spinbutton"));
	priv->height_spinbutton = GTK_SPIN_BUTTON(gtk_builder_get_object(ui, "height_spinbutton"));
	priv->append_radiobutton = GTK_CHECK_BUTTON(gtk_builder_get_object(ui, "append_radiobutton"));
	priv->name_entry = GTK_ENTRY(gtk_builder_get_object(ui, "name_entry"));
	priv->inplace_radiobutton = GTK_CHECK_BUTTON(gtk_builder_get_object(ui, "inplace_radiobutton"));

	priv->progress_dialog = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(priv->progress_dialog), _("Resizing…"));
	progress_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_window_set_child(GTK_WINDOW(priv->progress_dialog), progress_box);
	priv->progress_label = gtk_label_new("");
	priv->progress_bar = gtk_progress_bar_new();
	priv->target_size_radiobutton =
		GTK_CHECK_BUTTON(gtk_builder_get_object(ui, "target_size_radiobutton"));

	priv->target_size_spinbutton =
		GTK_SPIN_BUTTON(gtk_builder_get_object(ui, "target_size_spinbutton"));

	priv->target_size_unit_combobox =
		GTK_DROP_DOWN(gtk_builder_get_object(ui, "target_size_unit_combobox"));
	gtk_drop_down_set_selected(priv->target_size_unit_combobox, 0);

	priv->pad_target_size_checkbutton =
		GTK_CHECK_BUTTON(gtk_builder_get_object(ui, "pad_target_size_checkbutton"));

	/* Bind input sensitivity to their respective radio buttons */
	g_object_bind_property(priv->default_size_radiobutton, "active",
						   priv->size_combobox, "sensitive",
						   G_BINDING_SYNC_CREATE);
	g_object_bind_property(priv->custom_pct_radiobutton, "active",
						   priv->pct_spinbutton, "sensitive",
						   G_BINDING_SYNC_CREATE);
	g_object_bind_property(priv->custom_size_radiobutton, "active",
						   priv->width_spinbutton, "sensitive",
						   G_BINDING_SYNC_CREATE);
	g_object_bind_property(priv->custom_size_radiobutton, "active",
						   priv->height_spinbutton, "sensitive",
						   G_BINDING_SYNC_CREATE);
	g_object_bind_property(priv->target_size_radiobutton, "active",
						   priv->target_size_spinbutton, "sensitive",
						   G_BINDING_SYNC_CREATE);
	g_object_bind_property(priv->target_size_radiobutton, "active",
						   priv->target_size_unit_combobox, "sensitive",
						   G_BINDING_SYNC_CREATE);
	g_object_bind_property(priv->target_size_radiobutton, "active",
						   priv->pad_target_size_checkbutton, "sensitive",
						   G_BINDING_SYNC_CREATE);
	g_object_bind_property(priv->append_radiobutton, "active",
						   priv->name_entry, "sensitive",
						   G_BINDING_SYNC_CREATE);

	gtk_box_append(GTK_BOX(progress_box), priv->progress_bar);
	gtk_box_append(GTK_BOX(progress_box), priv->progress_label);
	priv->target_size_kb = 50;
	priv->use_target_size = FALSE;
	priv->pad_target_size = FALSE;
	priv->scratch_filename = NULL;
	/* Connect signal */
	g_signal_connect(G_OBJECT(priv->resize_dialog), "response",
					 (GCallback)nautilus_image_resizer_response_cb,
					 resizer);
}

NautilusImageResizer *
nautilus_image_resizer_new(GList *files)
{
	return g_object_new(NAUTILUS_TYPE_IMAGE_RESIZER, "files", files, NULL);
}

void nautilus_image_resizer_show_dialog(NautilusImageResizer *resizer)
{
	NautilusImageResizerPrivate *priv = nautilus_image_resizer_get_instance_private(resizer);

	gtk_widget_show(GTK_WIDGET(priv->resize_dialog));
}
