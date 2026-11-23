/*
 *  ic-crop-view.h — interactive crop / rotate canvas
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 */

#ifndef IC_CROP_VIEW_H
#define IC_CROP_VIEW_H

#include <gtk/gtk.h>
#include "ic-settings.h"

G_BEGIN_DECLS

#define IC_TYPE_CROP_VIEW (ic_crop_view_get_type ())
G_DECLARE_FINAL_TYPE (IcCropView, ic_crop_view, IC, CROP_VIEW, GtkWidget)

GtkWidget *ic_crop_view_new        (IcEdit *edit);
/* texture: a (possibly downscaled) display copy; w/h: full-size oriented dimensions */
void       ic_crop_view_set_image  (IcCropView *view, GdkTexture *texture, int w, int h);
/* Pixel aspect ratio (width / height) the crop must keep, or 0 for free. */
/* If strict is FALSE, the ratio follows the orientation of the current crop
 * (e.g. 4:3 becomes 3:4 on a portrait crop). */
void       ic_crop_view_set_aspect (IcCropView *view, double ratio, gboolean strict);
/* Reshape the crop to the largest rectangle of this pixel ratio that fits in
 * the image (minus the empty corners when straightened), keeping its centre.
 * Does not lock the ratio. */
void       ic_crop_view_fit_ratio  (IcCropView *view, double ratio);
/* Call after the IcEdit was changed from outside. */
void       ic_crop_view_update     (IcCropView *view);

G_END_DECLS

#endif
