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
#include "nautilus-image-resizer.h"
#include "nautilus-image-rotator.h"

#include <libnautilus-extension/nautilus-menu-provider.h>

#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>

#include <string.h> /* for strcmp */

static void nautilus_image_converter_instance_init (NautilusImageConverter      *img);
static void nautilus_image_converter_class_init    (NautilusImageConverterClass *class);
GList *     nautilus_image_converter_get_file_items (NautilusMenuProvider *provider,
						     GtkWidget            *window,
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
image_resize_callback (NautilusMenuItem *item,
			GList *files)
{
	NautilusImageResizer *resizer = nautilus_image_resizer_new (image_converter_filter_images (files));
	nautilus_image_resizer_show_dialog (resizer);
}

static void
image_rotate_callback (NautilusMenuItem *item,
			GList *files)
{
	NautilusImageRotator *rotator = nautilus_image_rotator_new (image_converter_filter_images (files));
	nautilus_image_rotator_show_dialog (rotator);
}

static GList *
nautilus_image_converter_get_background_items (NautilusMenuProvider *provider,
					     GtkWidget		  *window,
					     NautilusFileInfo	  *file_info)
{
	return NULL;
}

GList *
nautilus_image_converter_get_file_items (NautilusMenuProvider *provider,
				       GtkWidget            *window,
				       GList                *files)
{
	NautilusMenuItem *item;
	GList *file;
	GList *items = NULL;
	
	for (file = files; file != NULL; file = file->next) {
		if (image_converter_file_is_image (file->data)) {
			item = nautilus_menu_item_new ("NautilusImageConverter::resize",
				        _("_Resize Images..."),
				        _("Resize each selected image"),
				       "stock_position-size");
			g_signal_connect (item, "activate",
					  G_CALLBACK (image_resize_callback),
					  nautilus_file_info_list_copy (files));
					
			items = g_list_prepend (items, item);

			item = nautilus_menu_item_new ("NautilusImageConverter::rotate",
				        _("Ro_tate Images..."),
				        _("Rotate each selected image"),
				       "stock_rotate");
			g_signal_connect (item, "activate",
					  G_CALLBACK (image_rotate_callback),
					  nautilus_file_info_list_copy (files));

			items = g_list_prepend (items, item);
			
			items = g_list_reverse (items);

			return items;
		}
	}
	
	return NULL;
}

static void
nautilus_image_converter_menu_provider_iface_init (NautilusMenuProviderIface *iface)
{
	iface->get_background_items = nautilus_image_converter_get_background_items;
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
