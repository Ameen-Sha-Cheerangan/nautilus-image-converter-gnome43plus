/*
 *  ic-settings.h — conversion settings, profiles and ImageMagick command building
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 */

#ifndef IC_SETTINGS_H
#define IC_SETTINGS_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
	IC_FORMAT_KEEP = 0,
	IC_FORMAT_JPEG,
	IC_FORMAT_PNG,
	IC_FORMAT_WEBP,
	IC_FORMAT_AVIF,
	IC_FORMAT_GIF,
	IC_FORMAT_PDF,
	IC_FORMAT_N
} IcFormat;

typedef enum {
	IC_RESIZE_NONE = 0,
	IC_RESIZE_SCALE,
	IC_RESIZE_FIT,
	IC_RESIZE_EXACT,
	IC_RESIZE_N
} IcResizeMode;

typedef enum {
	IC_OUTPUT_SUFFIX = 0,
	IC_OUTPUT_REPLACE,
} IcOutputMode;

/* Everything a profile stores. */
typedef struct {
	IcFormat     format;
	IcResizeMode resize;
	int          scale;         /* percent */
	int          width;
	int          height;
	gboolean     no_upscale;
	int          quality;       /* 1..100, JPEG/WebP/AVIF */
	int          target_kb;     /* 0 = off; JPEG/WebP only */
	gboolean     strip;
	IcOutputMode output;
	char        *suffix;
} IcSettings;

/* Per-session visual edits (not stored in profiles).
 * The crop rectangle is normalised to the bounding box of the
 * flipped + rotated image, so it applies to images of any size. */
typedef struct {
	gboolean flip_h;   /* applied to the source before rotation (-flop) */
	gboolean flip_v;   /* (-flip) */
	int      rot90;    /* 0..3 quarter turns clockwise */
	double   fine;     /* extra clockwise angle, degrees (-45..45) */
	double   cx, cy, cw, ch;
	/* the crop was computed by ic_edit_auto_crop() after straightening:
	 * recompute it for each image, whatever its proportions */
	gboolean auto_crop;
} IcEdit;

typedef struct {
	char       *name;
	IcSettings  s;
} IcProfile;

void        ic_settings_copy_into   (IcSettings *dst, const IcSettings *src);
void        ic_settings_clear       (IcSettings *s);
gboolean    ic_settings_equal       (const IcSettings *a, const IcSettings *b);

void        ic_edit_reset           (IcEdit *e);
gboolean    ic_edit_is_identity     (const IcEdit *e);
gboolean    ic_edit_has_crop        (const IcEdit *e);
double      ic_edit_total_angle     (const IcEdit *e);
/* Bounding box of the rotated image, for a source of w x h (after EXIF orientation). */
void        ic_edit_bbox            (const IcEdit *e, double w, double h, double *bw, double *bh);
/* Largest centred crop of the original aspect ratio that avoids empty corners. */
void        ic_edit_auto_crop       (IcEdit *e, double w, double h);

const char *ic_format_extension     (IcFormat f);  /* NULL for KEEP */
const char *ic_format_label         (IcFormat f);
gboolean    ic_extension_has_quality(const char *ext);
gboolean    ic_extension_has_alpha  (const char *ext);
gboolean    ic_extension_has_target_size (const char *ext);

/* Lower-case extension of path (without dot), or "" */
char       *ic_path_extension       (const char *path);
/* Output extension for a given source path. */
char       *ic_output_extension     (const IcSettings *s, const char *src_path);

/* Profiles */
GPtrArray  *ic_profiles_load        (char **last_used);   /* of IcProfile* */
void        ic_profiles_save        (GPtrArray *profiles, const char *last_used);
GPtrArray  *ic_profiles_defaults    (void);
IcProfile  *ic_profile_new          (const char *name, const IcSettings *s);
void        ic_profile_free         (IcProfile *p);

/* ImageMagick */
const char *ic_magick_convert       (void);  /* "magick" or "convert" */

/* Appends the ImageMagick arguments that turn `in_path` into `out_path`.
 * src_w/src_h are the dimensions after EXIF orientation (only needed when
 * cropping). If `first_frame` is set, only the first frame is read. */
void        ic_build_ops            (GPtrArray *argv,
                                     const IcSettings *s,
                                     const IcEdit *e,
                                     const char *in_path,
                                     int src_w, int src_h,
                                     const char *out_ext,
                                     gboolean first_frame);

/* Synchronously reads the oriented dimensions of an image. */
gboolean    ic_identify             (const char *path, int *w, int *h);

G_END_DECLS

#endif
