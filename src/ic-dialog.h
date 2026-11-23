/*
 *  ic-dialog.h — the "Resize/convert image" window
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 */

#ifndef IC_DIALOG_H
#define IC_DIALOG_H

#include <glib.h>

G_BEGIN_DECLS

/* files: list of NautilusFileInfo* (local images). The list is not kept. */
void ic_dialog_show (GList *files);

G_END_DECLS

#endif
