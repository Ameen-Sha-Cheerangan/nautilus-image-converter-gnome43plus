/*
 *  nautilus-image-format-changer.c
 *
 *  Copyright (C) 2004-2008 Jürg Billeter
 *  Copyright (C) 2026 Ameen Sha Cheerangan
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
 */

#include <config.h> /* for GETTEXT_PACKAGE */

#include "nautilus-image-format-changer.h"

#include <string.h>

#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

#include <nautilus-extension.h>

typedef struct _NautilusImageFormatChangerPrivate NautilusImageFormatChangerPrivate;

struct _NautilusImageFormatChangerPrivate
{
	GList *files;

	gchar *suffix;
	gchar *target_ext;
	gboolean delete_original;

	int images_converted;
	int images_total;

	GtkDialog *format_dialog;
	GtkDropDown *format_combobox;
	GtkCheckButton *append_radiobutton;
	GtkEntry *name_entry;
	GtkCheckButton *delete_orig_radiobutton;

	GtkWidget *progress_dialog;
	GtkWidget *progress_bar;
	GtkWidget *progress_label;
};

#define NAUTILUS_IMAGE_FORMAT_CHANGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE((o), NAUTILUS_TYPE_IMAGE_FORMAT_CHANGER, NautilusImageFormatChangerPrivate))

G_DEFINE_TYPE_WITH_PRIVATE(NautilusImageFormatChanger, nautilus_image_format_changer, G_TYPE_OBJECT)

enum
{
	PROP_FILES = 1,
};

static void
nautilus_image_format_changer_finalize(GObject *object)
{
	NautilusImageFormatChanger *dialog = NAUTILUS_IMAGE_FORMAT_CHANGER(object);
	NautilusImageFormatChangerPrivate *priv = nautilus_image_format_changer_get_instance_private(dialog);

	g_free(priv->suffix);
	g_free(priv->target_ext);

	G_OBJECT_CLASS(nautilus_image_format_changer_parent_class)->finalize(object);
}

static void
nautilus_image_format_changer_set_property(GObject *object,
										  guint property_id,
										  const GValue *value,
										  GParamSpec *pspec)
{
	NautilusImageFormatChanger *dialog = NAUTILUS_IMAGE_FORMAT_CHANGER(object);
	NautilusImageFormatChangerPrivate *priv = nautilus_image_format_changer_get_instance_private(dialog);

	switch (property_id)
	{
	case PROP_FILES:
		priv->files = g_value_get_pointer(value);
		priv->images_total = g_list_length(priv->files);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void
nautilus_image_format_changer_get_property(GObject *object,
										  guint property_id,
										  GValue *value,
										  GParamSpec *pspec)
{
	NautilusImageFormatChanger *self = NAUTILUS_IMAGE_FORMAT_CHANGER(object);
	NautilusImageFormatChangerPrivate *priv = nautilus_image_format_changer_get_instance_private(self);

	switch (property_id)
	{
	case PROP_FILES:
		g_value_set_pointer(value, priv->files);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void
nautilus_image_format_changer_class_init(NautilusImageFormatChangerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GParamSpec *files_param_spec;

	object_class->finalize = nautilus_image_format_changer_finalize;
	object_class->set_property = nautilus_image_format_changer_set_property;
	object_class->get_property = nautilus_image_format_changer_get_property;

	files_param_spec = g_param_spec_pointer("files",
											"Files",
											"Set selected files",
											G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE);

	g_object_class_install_property(object_class,
									PROP_FILES,
									files_param_spec);
}

static void run_op(NautilusImageFormatChanger *changer);

static GFile *
nautilus_image_format_changer_transform_filename(NautilusImageFormatChanger *changer, GFile *orig_file)
{
	NautilusImageFormatChangerPrivate *priv = nautilus_image_format_changer_get_instance_private(changer);

	GFile *parent_file, *new_file;
	char *basename, *extension, *new_basename;

	g_return_val_if_fail(G_IS_FILE(orig_file), NULL);

	parent_file = g_file_get_parent(orig_file);
	basename = g_strdup(g_file_get_basename(orig_file));

	extension = g_strdup(strrchr(basename, '.'));
	if (extension != NULL)
		basename[strlen(basename) - strlen(extension)] = '\0';

	if (priv->suffix != NULL && strlen(priv->suffix) > 0)
	{
		new_basename = g_strdup_printf("%s%s.%s", basename, priv->suffix, priv->target_ext);
	}
	else
	{
		new_basename = g_strdup_printf("%s.%s", basename, priv->target_ext);
	}

	g_free(basename);
	g_free(extension);

	new_file = g_file_get_child(parent_file, new_basename);

	g_object_unref(parent_file);
	g_free(new_basename);

	return new_file;
}

static void
retry_dialog_cb(GtkDialog *dialog,
				gint response_id,
				gpointer user_data)
{
	NautilusImageFormatChanger *changer = NAUTILUS_IMAGE_FORMAT_CHANGER(user_data);
	NautilusImageFormatChangerPrivate *priv = nautilus_image_format_changer_get_instance_private(changer);

	gtk_window_destroy(GTK_WINDOW(dialog));

	if (response_id == GTK_RESPONSE_CANCEL)
	{
		gtk_window_destroy(GTK_WINDOW(priv->progress_dialog));
		return;
	}
	else if (response_id == 1)
	{
		priv->images_converted++;
		priv->files = priv->files->next;
	}

	if (priv->files != NULL)
	{
		run_op(changer);
	}
	else
	{
		gtk_window_destroy(GTK_WINDOW(priv->progress_dialog));
	}
}

static void
op_finished(GPid pid, gint status, gpointer data)
{
	NautilusImageFormatChanger *changer = NAUTILUS_IMAGE_FORMAT_CHANGER(data);
	NautilusImageFormatChangerPrivate *priv = nautilus_image_format_changer_get_instance_private(changer);

	NautilusFileInfo *file = NAUTILUS_FILE_INFO(priv->files->data);

	if (status != 0)
	{
		char *name = nautilus_file_info_get_name(file);
		GtkWidget *msg_dialog = gtk_message_dialog_new(GTK_WINDOW(priv->progress_dialog),
													   GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
													   GTK_BUTTONS_NONE,
													   _("'%s' cannot be converted. Check whether you have permission to write to this folder."),
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
		GFile *new_location = nautilus_image_format_changer_transform_filename(changer, orig_location);

		if (priv->delete_original && !g_file_equal(orig_location, new_location))
		{
			g_file_delete(orig_location, NULL, NULL);
		}

		nautilus_file_info_invalidate_extension_info(file);

		g_object_unref(orig_location);
		g_object_unref(new_location);
	}

	priv->images_converted++;
	priv->files = priv->files->next;

	if (priv->files != NULL)
	{
		run_op(changer);
	}
	else
	{
		gtk_window_destroy(GTK_WINDOW(priv->progress_dialog));
	}
}

static void
run_op(NautilusImageFormatChanger *changer)
{
	NautilusImageFormatChangerPrivate *priv =
		nautilus_image_format_changer_get_instance_private(changer);

	g_return_if_fail(priv->files != NULL);

	NautilusFileInfo *file = NAUTILUS_FILE_INFO(priv->files->data);

	GFile *orig_location = nautilus_file_info_get_location(file);
	char *filename = g_file_get_path(orig_location);
	GFile *new_location =
		nautilus_image_format_changer_transform_filename(changer, orig_location);
	char *new_filename = g_file_get_path(new_location);

	g_object_unref(orig_location);
	g_object_unref(new_location);

	gchar *argv[4];
	pid_t pid;
	gboolean spawn_success = FALSE;

	argv[0] = "/usr/bin/convert";
	argv[1] = filename;
	argv[2] = new_filename;
	argv[3] = NULL;

	spawn_success =
		g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, NULL);

	if (!spawn_success)
	{
		g_free(filename);
		g_free(new_filename);
		g_warning("Failed to spawn convert command");
		return;
	}

	g_free(filename);
	g_free(new_filename);

	g_child_watch_add(pid, op_finished, changer);

	char *tmp;

	gtk_progress_bar_set_fraction(
		GTK_PROGRESS_BAR(priv->progress_bar),
		(double)(priv->images_converted + 1) / priv->images_total);

	tmp = g_strdup_printf(_("Converting image: %d of %d"), priv->images_converted + 1, priv->images_total);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(priv->progress_bar), tmp);
	g_free(tmp);

	char *name = nautilus_file_info_get_name(file);
	tmp = g_strdup_printf(_("<i>Converting \"%s\"</i>"), name);
	g_free(name);

	gtk_label_set_markup(GTK_LABEL(priv->progress_label), tmp);
	g_free(tmp);
}

static void
nautilus_image_format_changer_response_cb(GtkDialog *dialog, gint response_id, gpointer user_data)
{
	NautilusImageFormatChanger *changer = NAUTILUS_IMAGE_FORMAT_CHANGER(user_data);
	NautilusImageFormatChangerPrivate *priv =
		nautilus_image_format_changer_get_instance_private(changer);

	if (response_id == GTK_RESPONSE_OK)
	{
		g_clear_pointer(&priv->suffix, g_free);
		g_clear_pointer(&priv->target_ext, g_free);

		guint selected_idx = gtk_drop_down_get_selected(priv->format_combobox);
		const gchar *exts[] = {"webp", "png", "jpg", "avif", "gif"};
		if (selected_idx < G_N_ELEMENTS(exts))
		{
			priv->target_ext = g_strdup(exts[selected_idx]);
		}
		else
		{
			priv->target_ext = g_strdup("webp");
		}

		priv->delete_original = gtk_check_button_get_active(priv->delete_orig_radiobutton);

		if (gtk_check_button_get_active(priv->append_radiobutton))
		{
			const gchar *text = gtk_editable_get_text(GTK_EDITABLE(priv->name_entry));
			if (text != NULL && strlen(text) > 0)
			{
				priv->suffix = g_strdup(text);
			}
		}

		gtk_widget_show(priv->progress_dialog);
		run_op(changer);
	}

	gtk_window_destroy(GTK_WINDOW(dialog));
}

static void
nautilus_image_format_changer_init(NautilusImageFormatChanger *changer)
{
	NautilusImageFormatChangerPrivate *priv = nautilus_image_format_changer_get_instance_private(changer);

	GtkBuilder *ui;
	gchar *path;
	guint result;
	GError *err = NULL;
	GtkWidget *progress_box;

	ui = gtk_builder_new();
	gtk_builder_set_translation_domain(ui, GETTEXT_PACKAGE);
	path = g_build_filename(DATADIR, PACKAGE, "nautilus-image-format-change.ui", NULL);
	result = gtk_builder_add_from_file(ui, path, &err);
	g_free(path);

	if (result == 0)
	{
		g_warning("%s", err->message);
		g_error_free(err);
		return;
	}

	priv->format_dialog = GTK_DIALOG(gtk_builder_get_object(ui, "format_dialog"));
	priv->format_combobox = GTK_DROP_DOWN(gtk_builder_get_object(ui, "format_combobox"));
	gtk_drop_down_set_selected(priv->format_combobox, 0);
	priv->append_radiobutton = GTK_CHECK_BUTTON(gtk_builder_get_object(ui, "append_radiobutton"));
	priv->name_entry = GTK_ENTRY(gtk_builder_get_object(ui, "name_entry"));
	priv->delete_orig_radiobutton = GTK_CHECK_BUTTON(gtk_builder_get_object(ui, "delete_orig_radiobutton"));

	g_object_bind_property(priv->append_radiobutton, "active",
	                       priv->name_entry, "sensitive",
	                       G_BINDING_SYNC_CREATE);

	priv->progress_dialog = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(priv->progress_dialog), _("Converting…"));
	progress_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_window_set_child(GTK_WINDOW(priv->progress_dialog), progress_box);
	priv->progress_label = gtk_label_new("");
	priv->progress_bar = gtk_progress_bar_new();

	gtk_box_append(GTK_BOX(progress_box), priv->progress_bar);
	gtk_box_append(GTK_BOX(progress_box), priv->progress_label);

	priv->target_ext = g_strdup("webp");
	priv->suffix = NULL;
	priv->delete_original = FALSE;

	g_signal_connect(G_OBJECT(priv->format_dialog), "response",
					 (GCallback)nautilus_image_format_changer_response_cb,
					 changer);
}

NautilusImageFormatChanger *
nautilus_image_format_changer_new(GList *files)
{
	return g_object_new(NAUTILUS_TYPE_IMAGE_FORMAT_CHANGER, "files", files, NULL);
}

void
nautilus_image_format_changer_show_dialog(NautilusImageFormatChanger *changer)
{
	NautilusImageFormatChangerPrivate *priv = nautilus_image_format_changer_get_instance_private(changer);

	gtk_widget_show(GTK_WIDGET(priv->format_dialog));
}
