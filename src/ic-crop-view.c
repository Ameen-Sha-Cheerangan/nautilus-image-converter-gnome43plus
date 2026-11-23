/*
 *  ic-crop-view.c — interactive crop / rotate canvas
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 */

#include <config.h>

#include "ic-crop-view.h"

#include <math.h>

#define MARGIN      18.0
#define HANDLE      14.0  /* hit radius in pixels */
#define HANDLE_SIZE 10.0
#define MIN_PX      16.0

enum {
	ZONE_NONE   = 0,
	ZONE_LEFT   = 1 << 0,
	ZONE_RIGHT  = 1 << 1,
	ZONE_TOP    = 1 << 2,
	ZONE_BOTTOM = 1 << 3,
	ZONE_MOVE   = 1 << 4,
	ZONE_NEW    = 1 << 5,
};

enum {
	SIGNAL_CHANGED,
	SIGNAL_EDIT_FINISHED,
	N_SIGNALS
};
static guint signals[N_SIGNALS];

struct _IcCropView {
	GtkWidget parent_instance;

	IcEdit     *edit;
	GdkTexture *texture;
	int         img_w, img_h;
	double      aspect;     /* pixel ratio, 0 = free */

	/* drag state */
	int    zone;
	double start_x, start_y;
	double r0x, r0y, r0w, r0h;
	gboolean dragging;
};

G_DEFINE_TYPE (IcCropView, ic_crop_view, GTK_TYPE_WIDGET)

/* Geometry of the rotated image's bounding box inside the widget. */
typedef struct {
	double bw, bh;     /* bbox in image pixels */
	double scale;      /* widget px per image px */
	double ox, oy;     /* top-left of bbox in widget */
	double dw, dh;     /* bbox size in widget px */
} Layout;

static gboolean
compute_layout (IcCropView *self, Layout *l)
{
	double width = gtk_widget_get_width (GTK_WIDGET (self));
	double height = gtk_widget_get_height (GTK_WIDGET (self));

	if (self->texture == NULL || self->img_w <= 0 || self->img_h <= 0)
		return FALSE;

	ic_edit_bbox (self->edit, self->img_w, self->img_h, &l->bw, &l->bh);
	l->scale = MIN ((width - 2 * MARGIN) / l->bw, (height - 2 * MARGIN) / l->bh);
	if (l->scale <= 0)
		return FALSE;
	l->dw = l->bw * l->scale;
	l->dh = l->bh * l->scale;
	l->ox = (width - l->dw) / 2.0;
	l->oy = (height - l->dh) / 2.0;
	return TRUE;
}

static void
crop_rect_widget (IcCropView *self, const Layout *l, graphene_rect_t *r)
{
	IcEdit *e = self->edit;
	graphene_rect_init (r,
	                    l->ox + e->cx * l->dw,
	                    l->oy + e->cy * l->dh,
	                    e->cw * l->dw,
	                    e->ch * l->dh);
}

/* ------------------------------------------------------------------ */
/* Aspect ratio                                                         */
/* ------------------------------------------------------------------ */

/* Normalised ratio (cw/ch) corresponding to the locked pixel ratio. */
static double
normalised_ratio (IcCropView *self)
{
	double bw, bh;
	ic_edit_bbox (self->edit, self->img_w, self->img_h, &bw, &bh);
	return self->aspect * bh / bw;
}

static void
apply_aspect (IcCropView *self)
{
	IcEdit *e = self->edit;
	double rn, cxm, cym, w, h;

	if (self->aspect <= 0 || self->img_w <= 0)
		return;

	rn = normalised_ratio (self);
	cxm = e->cx + e->cw / 2.0;
	cym = e->cy + e->ch / 2.0;
	w = e->cw;
	h = e->ch;
	if (w / h > rn)
		w = h * rn;
	else
		h = w / rn;

	e->cw = w;
	e->ch = h;
	e->cx = CLAMP (cxm - w / 2.0, 0.0, 1.0 - w);
	e->cy = CLAMP (cym - h / 2.0, 0.0, 1.0 - h);
}

void
ic_crop_view_set_aspect (IcCropView *self, double ratio, gboolean strict)
{
	IcEdit *e = self->edit;

	if (ratio > 0 && self->img_w > 0 && !strict) {
		double bw, bh;
		ic_edit_bbox (e, self->img_w, self->img_h, &bw, &bh);
		/* follow the orientation of the current crop */
		gboolean portrait = e->cw * bw < e->ch * bh;
		if ((portrait && ratio > 1.0) || (!portrait && ratio < 1.0))
			ratio = 1.0 / ratio;
	}
	self->aspect = ratio;
	apply_aspect (self);
	gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ------------------------------------------------------------------ */
/* Drawing                                                              */
/* ------------------------------------------------------------------ */

static void
append_rect (GtkSnapshot *snapshot, double x, double y, double w, double h, const GdkRGBA *c)
{
	graphene_rect_t r;
	if (w <= 0 || h <= 0)
		return;
	graphene_rect_init (&r, x, y, w, h);
	gtk_snapshot_append_color (snapshot, c, &r);
}

static void
ic_crop_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
	IcCropView *self = IC_CROP_VIEW (widget);
	IcEdit *e = self->edit;
	Layout l;
	graphene_rect_t img, cr;
	graphene_point_t p;
	const GdkRGBA shade = { 0, 0, 0, 0.6 };
	const GdkRGBA white = { 1, 1, 1, 0.95 };
	const GdkRGBA guide = { 1, 1, 1, 0.35 };

	if (!compute_layout (self, &l))
		return;

	/* the image, flipped then rotated, centred in the bbox */
	gtk_snapshot_save (snapshot);
	graphene_point_init (&p, l.ox + l.dw / 2.0, l.oy + l.dh / 2.0);
	gtk_snapshot_translate (snapshot, &p);
	gtk_snapshot_rotate (snapshot, (float) ic_edit_total_angle (e));
	gtk_snapshot_scale (snapshot,
	                    (float) (e->flip_h ? -l.scale : l.scale),
	                    (float) (e->flip_v ? -l.scale : l.scale));
	graphene_rect_init (&img, -self->img_w / 2.0, -self->img_h / 2.0, self->img_w, self->img_h);
	gtk_snapshot_append_texture (snapshot, self->texture, &img);
	gtk_snapshot_restore (snapshot);

	/* shade everything outside the crop */
	crop_rect_widget (self, &l, &cr);
	append_rect (snapshot, l.ox, l.oy, l.dw, cr.origin.y - l.oy, &shade);
	append_rect (snapshot, l.ox, cr.origin.y + cr.size.height, l.dw,
	             l.oy + l.dh - (cr.origin.y + cr.size.height), &shade);
	append_rect (snapshot, l.ox, cr.origin.y, cr.origin.x - l.ox, cr.size.height, &shade);
	append_rect (snapshot, cr.origin.x + cr.size.width, cr.origin.y,
	             l.ox + l.dw - (cr.origin.x + cr.size.width), cr.size.height, &shade);

	/* rule-of-thirds guides */
	for (int i = 1; i < 3; i++) {
		append_rect (snapshot, cr.origin.x + cr.size.width * i / 3.0 - 0.5, cr.origin.y,
		             1, cr.size.height, &guide);
		append_rect (snapshot, cr.origin.x, cr.origin.y + cr.size.height * i / 3.0 - 0.5,
		             cr.size.width, 1, &guide);
	}

	/* border */
	{
		GskRoundedRect rr;
		const float widths[4] = { 1.5f, 1.5f, 1.5f, 1.5f };
		const GdkRGBA colors[4] = { white, white, white, white };
		gsk_rounded_rect_init_from_rect (&rr, &cr, 0);
		gtk_snapshot_append_border (snapshot, &rr, widths, colors);
	}

	/* corner and edge handles */
	{
		double xs[3] = { cr.origin.x, cr.origin.x + cr.size.width / 2.0, cr.origin.x + cr.size.width };
		double ys[3] = { cr.origin.y, cr.origin.y + cr.size.height / 2.0, cr.origin.y + cr.size.height };
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++) {
				if (i == 1 && j == 1)
					continue;
				if (self->aspect > 0 && (i == 1 || j == 1))
					continue;
				append_rect (snapshot, xs[i] - HANDLE_SIZE / 2.0, ys[j] - HANDLE_SIZE / 2.0,
				             HANDLE_SIZE, HANDLE_SIZE, &white);
			}
	}
}

/* ------------------------------------------------------------------ */
/* Interaction                                                          */
/* ------------------------------------------------------------------ */

static int
hit_test (IcCropView *self, double x, double y)
{
	Layout l;
	graphene_rect_t cr;
	int zone = ZONE_NONE;
	double x1, y1, x2, y2;
	gboolean in_x, in_y;

	if (!compute_layout (self, &l))
		return ZONE_NONE;

	crop_rect_widget (self, &l, &cr);
	x1 = cr.origin.x;
	y1 = cr.origin.y;
	x2 = x1 + cr.size.width;
	y2 = y1 + cr.size.height;
	in_x = x > x1 - HANDLE && x < x2 + HANDLE;
	in_y = y > y1 - HANDLE && y < y2 + HANDLE;

	if (in_y && fabs (x - x1) < HANDLE)
		zone |= ZONE_LEFT;
	else if (in_y && fabs (x - x2) < HANDLE)
		zone |= ZONE_RIGHT;
	if (in_x && fabs (y - y1) < HANDLE)
		zone |= ZONE_TOP;
	else if (in_x && fabs (y - y2) < HANDLE)
		zone |= ZONE_BOTTOM;

	if (self->aspect > 0) {
		/* only corners can resize when the ratio is locked */
		gboolean corner = (zone & (ZONE_LEFT | ZONE_RIGHT)) && (zone & (ZONE_TOP | ZONE_BOTTOM));
		if (!corner)
			zone = ZONE_NONE;
	}

	if (zone == ZONE_NONE) {
		if (x > x1 && x < x2 && y > y1 && y < y2)
			zone = ZONE_MOVE;
		else if (x >= l.ox && x <= l.ox + l.dw && y >= l.oy && y <= l.oy + l.dh)
			zone = ZONE_NEW;
	}
	return zone;
}

static const char *
zone_cursor (int zone)
{
	switch (zone) {
	case ZONE_LEFT | ZONE_TOP:     return "nw-resize";
	case ZONE_RIGHT | ZONE_TOP:    return "ne-resize";
	case ZONE_LEFT | ZONE_BOTTOM:  return "sw-resize";
	case ZONE_RIGHT | ZONE_BOTTOM: return "se-resize";
	case ZONE_LEFT:                return "w-resize";
	case ZONE_RIGHT:               return "e-resize";
	case ZONE_TOP:                 return "n-resize";
	case ZONE_BOTTOM:              return "s-resize";
	case ZONE_MOVE:                return "move";
	case ZONE_NEW:                 return "crosshair";
	default:                       return NULL;
	}
}

static void
motion_cb (GtkEventControllerMotion *ctrl, double x, double y, IcCropView *self)
{
	if (self->dragging)
		return;
	gtk_widget_set_cursor_from_name (GTK_WIDGET (self), zone_cursor (hit_test (self, x, y)));
}

static void
drag_begin_cb (GtkGestureDrag *gesture, double x, double y, IcCropView *self)
{
	IcEdit *e = self->edit;
	Layout l;

	self->zone = hit_test (self, x, y);
	if (self->zone == ZONE_NONE || !compute_layout (self, &l)) {
		gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
		return;
	}
	self->dragging = TRUE;
	self->start_x = (x - l.ox) / l.dw;
	self->start_y = (y - l.oy) / l.dh;
	self->r0x = e->cx;
	self->r0y = e->cy;
	self->r0w = e->cw;
	self->r0h = e->ch;
}

/* Rectangle spanned by fixed anchor (ax, ay) and pointer (px, py). */
static void
span_rect (IcCropView *self, double ax, double ay, double px, double py, double minw, double minh)
{
	IcEdit *e = self->edit;
	double w, h;

	px = CLAMP (px, 0.0, 1.0);
	py = CLAMP (py, 0.0, 1.0);
	w = MAX (fabs (px - ax), minw);
	h = MAX (fabs (py - ay), minh);

	if (self->aspect > 0) {
		double rn = normalised_ratio (self);
		if (w / h > rn)
			w = h * rn;
		else
			h = w / rn;
	}

	/* keep inside the image when the minimum size pushes it out */
	w = MIN (w, px < ax ? ax : 1.0 - ax);
	h = MIN (h, py < ay ? ay : 1.0 - ay);
	if (self->aspect > 0) {
		double rn = normalised_ratio (self);
		if (w / h > rn)
			w = h * rn;
		else
			h = w / rn;
	}

	e->cx = px < ax ? ax - w : ax;
	e->cy = py < ay ? ay - h : ay;
	e->cw = w;
	e->ch = h;
}

static void
drag_update_cb (GtkGestureDrag *gesture, double dx, double dy, IcCropView *self)
{
	IcEdit *e = self->edit;
	Layout l;
	double ndx, ndy, minw, minh;
	int z = self->zone;

	if (!self->dragging || !compute_layout (self, &l))
		return;

	ndx = dx / l.dw;
	ndy = dy / l.dh;
	minw = MIN_PX / l.dw;
	minh = MIN_PX / l.dh;

	if (z == ZONE_MOVE) {
		e->cx = CLAMP (self->r0x + ndx, 0.0, 1.0 - e->cw);
		e->cy = CLAMP (self->r0y + ndy, 0.0, 1.0 - e->ch);
	} else if (z == ZONE_NEW) {
		span_rect (self, self->start_x, self->start_y,
		           self->start_x + ndx, self->start_y + ndy, minw, minh);
	} else if ((z & (ZONE_LEFT | ZONE_RIGHT)) && (z & (ZONE_TOP | ZONE_BOTTOM))) {
		double ax = (z & ZONE_LEFT) ? self->r0x + self->r0w : self->r0x;
		double ay = (z & ZONE_TOP) ? self->r0y + self->r0h : self->r0y;
		double px = (z & ZONE_LEFT) ? self->r0x + ndx : self->r0x + self->r0w + ndx;
		double py = (z & ZONE_TOP) ? self->r0y + ndy : self->r0y + self->r0h + ndy;
		/* do not let the corner cross over its anchor */
		px = (z & ZONE_LEFT) ? MIN (px, ax - minw) : MAX (px, ax + minw);
		py = (z & ZONE_TOP) ? MIN (py, ay - minh) : MAX (py, ay + minh);
		span_rect (self, ax, ay, px, py, minw, minh);
	} else {
		if (z & ZONE_LEFT) {
			double right = self->r0x + self->r0w;
			e->cx = CLAMP (self->r0x + ndx, 0.0, right - minw);
			e->cw = right - e->cx;
		} else if (z & ZONE_RIGHT) {
			e->cw = CLAMP (self->r0w + ndx, minw, 1.0 - self->r0x);
		}
		if (z & ZONE_TOP) {
			double bottom = self->r0y + self->r0h;
			e->cy = CLAMP (self->r0y + ndy, 0.0, bottom - minh);
			e->ch = bottom - e->cy;
		} else if (z & ZONE_BOTTOM) {
			e->ch = CLAMP (self->r0h + ndy, minh, 1.0 - self->r0y);
		}
	}

	gtk_widget_queue_draw (GTK_WIDGET (self));
	g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
drag_end_cb (GtkGestureDrag *gesture, double dx, double dy, IcCropView *self)
{
	IcEdit *e = self->edit;

	if (!self->dragging)
		return;
	self->dragging = FALSE;

	/* a simple click outside the crop restores the previous rectangle */
	if (self->zone == ZONE_NEW && fabs (dx) < 3 && fabs (dy) < 3) {
		e->cx = self->r0x;
		e->cy = self->r0y;
		e->cw = self->r0w;
		e->ch = self->r0h;
		gtk_widget_queue_draw (GTK_WIDGET (self));
		return;
	}

	g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
	g_signal_emit (self, signals[SIGNAL_EDIT_FINISHED], 0);
}

/* ------------------------------------------------------------------ */

static void
ic_crop_view_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                      int *minimum, int *natural, int *min_baseline, int *nat_baseline)
{
	*minimum = 200;
	*natural = orientation == GTK_ORIENTATION_HORIZONTAL ? 640 : 480;
}

static void
ic_crop_view_dispose (GObject *object)
{
	IcCropView *self = IC_CROP_VIEW (object);
	g_clear_object (&self->texture);
	G_OBJECT_CLASS (ic_crop_view_parent_class)->dispose (object);
}

static void
ic_crop_view_class_init (IcCropViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->dispose = ic_crop_view_dispose;
	widget_class->snapshot = ic_crop_view_snapshot;
	widget_class->measure = ic_crop_view_measure;

	signals[SIGNAL_CHANGED] =
		g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
		              0, NULL, NULL, NULL, G_TYPE_NONE, 0);
	signals[SIGNAL_EDIT_FINISHED] =
		g_signal_new ("edit-finished", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
		              0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
ic_crop_view_init (IcCropView *self)
{
	GtkGesture *drag = gtk_gesture_drag_new ();
	GtkEventController *motion = gtk_event_controller_motion_new ();

	gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
	g_signal_connect (drag, "drag-begin", G_CALLBACK (drag_begin_cb), self);
	g_signal_connect (drag, "drag-update", G_CALLBACK (drag_update_cb), self);
	g_signal_connect (drag, "drag-end", G_CALLBACK (drag_end_cb), self);
	gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));

	g_signal_connect (motion, "motion", G_CALLBACK (motion_cb), self);
	gtk_widget_add_controller (GTK_WIDGET (self), motion);

	gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
	gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
}

GtkWidget *
ic_crop_view_new (IcEdit *edit)
{
	IcCropView *self = g_object_new (IC_TYPE_CROP_VIEW, NULL);
	self->edit = edit;
	return GTK_WIDGET (self);
}

void
ic_crop_view_set_image (IcCropView *self, GdkTexture *texture, int w, int h)
{
	g_set_object (&self->texture, texture);
	self->img_w = w;
	self->img_h = h;
	gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
ic_crop_view_fit_ratio (IcCropView *self, double ratio)
{
	IcEdit *e = self->edit;
	double bw, bh, rn, w, h, mx, my;
	double rx = 0.0, ry = 0.0, rw = 1.0, rh = 1.0;

	if (ratio <= 0 || self->img_w <= 0 || self->img_h <= 0)
		return;

	if (fabs (e->fine) > 1e-6) {
		IcEdit t = *e;
		ic_edit_auto_crop (&t, self->img_w, self->img_h);
		rx = t.cx; ry = t.cy; rw = t.cw; rh = t.ch;
	}

	ic_edit_bbox (e, self->img_w, self->img_h, &bw, &bh);
	rn = ratio * bh / bw;
	w = rw;
	h = rh;
	if (w / h > rn)
		w = h * rn;
	else
		h = w / rn;

	mx = e->cx + e->cw / 2.0;
	my = e->cy + e->ch / 2.0;
	e->cw = w;
	e->ch = h;
	e->cx = CLAMP (mx - w / 2.0, rx, rx + rw - w);
	e->cy = CLAMP (my - h / 2.0, ry, ry + rh - h);
	gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
ic_crop_view_update (IcCropView *self)
{
	gtk_widget_queue_draw (GTK_WIDGET (self));
}
