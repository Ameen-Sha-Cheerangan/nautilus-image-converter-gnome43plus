/*
 *  nautilus-image-converter.c
 * 
 *  Copyright (C) 2004-2005 Jürg Billeter
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

#include "nautilus-image-converter.h"
#include "ic-dialog.h"

#include <nautilus-extension.h>

#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>

#include <string.h> /* for strcmp */

static void nautilus_image_converter_instance_init (NautilusImageConverter      *img);
static void nautilus_image_converter_class_init    (NautilusImageConverterClass *class);
GList *     nautilus_image_converter_get_file_items (NautilusMenuProvider *provider,
						     GList                *files);

static GType image_converter_type = 0;

static gboolean
image_converter_file_is_image (NautilusFileInfo *file_info)
{
	gchar            *uri_scheme;
	gchar            *mime_type;
	gboolean	maybe_image;
	
	maybe_image = TRUE;
	uri_scheme = nautilus_file_info_get_uri_scheme (file_info);
	if (strcmp (uri_scheme, "file") != 0)
		maybe_image = FALSE;
	g_free (uri_scheme);

	mime_type = nautilus_file_info_get_mime_type (file_info);
	if (strncmp (mime_type, "image/", 6) != 0)
		maybe_image = FALSE;
	g_free (mime_type);
	
	return maybe_image;
}

static GList *
image_converter_filter_images (GList *files)
{
	GList *images;
	GList *file;
	
	images = NULL;
	
	for (file = files; file != NULL; file = file->next) {
		if (image_converter_file_is_image (file->data))
			images = g_list_prepend (images, file->data);
	}
	
	return images;
}

static void
image_convert_callback (NautilusMenuItem *item,
			GList *files)
{
	GList *images = image_converter_filter_images (files);

	images = g_list_reverse (images);  /* keep the selection order */
	ic_dialog_show (images);
	g_list_free (images);
}

static void
free_file_list (gpointer data, GClosure *closure)
{
	nautilus_file_info_list_free (data);
}

GList *
nautilus_image_converter_get_file_items (NautilusMenuProvider *provider,
				       GList                *files)
{
	NautilusMenuItem *item;
	GList *file;

	for (file = files; file != NULL; file = file->next) {
		if (image_converter_file_is_image (file->data)) {
			item = nautilus_menu_item_new ("NautilusImageConverter::convert",
				        _("Resize/convert image…"),
				        _("Resize, compress, crop, rotate or convert the selected images"),
				       "image-x-generic");
			g_signal_connect_data (item, "activate",
					       G_CALLBACK (image_convert_callback),
					       nautilus_file_info_list_copy (files),
					       free_file_list, 0);

			return g_list_append (NULL, item);
		}
	}

	return NULL;
}

static void
nautilus_image_converter_menu_provider_iface_init (NautilusMenuProviderInterface *iface)
{
	iface->get_file_items = nautilus_image_converter_get_file_items;
}

static void 
nautilus_image_converter_instance_init (NautilusImageConverter *img)
{
}

static void
nautilus_image_converter_class_init (NautilusImageConverterClass *class)
{
}

GType
nautilus_image_converter_get_type (void) 
{
	return image_converter_type;
}

void
nautilus_image_converter_register_type (GTypeModule *module)
{
	static const GTypeInfo info = {
		sizeof (NautilusImageConverterClass),
		(GBaseInitFunc) NULL,
		(GBaseFinalizeFunc) NULL,
		(GClassInitFunc) nautilus_image_converter_class_init,
		NULL, 
		NULL,
		sizeof (NautilusImageConverter),
		0,
		(GInstanceInitFunc) nautilus_image_converter_instance_init,
	};

	static const GInterfaceInfo menu_provider_iface_info = {
		(GInterfaceInitFunc) nautilus_image_converter_menu_provider_iface_init,
		NULL,
		NULL
	};

	image_converter_type = g_type_module_register_type (module,
						     G_TYPE_OBJECT,
						     "NautilusImageConverter",
						     &info, 0);

	g_type_module_add_interface (module,
				     image_converter_type,
				     NAUTILUS_TYPE_MENU_PROVIDER,
				     &menu_provider_iface_info);
}
