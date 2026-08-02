#ifndef NAUTILUS_IMAGE_FORMAT_CHANGER_H
#define NAUTILUS_IMAGE_FORMAT_CHANGER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define NAUTILUS_TYPE_IMAGE_FORMAT_CHANGER         (nautilus_image_format_changer_get_type ())
#define NAUTILUS_IMAGE_FORMAT_CHANGER(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), NAUTILUS_TYPE_IMAGE_FORMAT_CHANGER, NautilusImageFormatChanger))
#define NAUTILUS_IMAGE_FORMAT_CHANGER_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), NAUTILUS_TYPE_IMAGE_FORMAT_CHANGER, NautilusImageFormatChangerClass))
#define NAUTILUS_IS_IMAGE_FORMAT_CHANGER(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), NAUTILUS_TYPE_IMAGE_FORMAT_CHANGER))
#define NAUTILUS_IS_IMAGE_FORMAT_CHANGER_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), NAUTILUS_TYPE_IMAGE_FORMAT_CHANGER))
#define NAUTILUS_IMAGE_FORMAT_CHANGER_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), NAUTILUS_TYPE_IMAGE_FORMAT_CHANGER, NautilusImageFormatChangerClass))

typedef struct _NautilusImageFormatChanger NautilusImageFormatChanger;
typedef struct _NautilusImageFormatChangerClass NautilusImageFormatChangerClass;

struct _NautilusImageFormatChanger {
	GObject parent;
};

struct _NautilusImageFormatChangerClass {
	GObjectClass parent_class;
};

GType nautilus_image_format_changer_get_type (void);
NautilusImageFormatChanger *nautilus_image_format_changer_new (GList *files);
void nautilus_image_format_changer_show_dialog (NautilusImageFormatChanger *dialog);

G_END_DECLS

#endif /* NAUTILUS_IMAGE_FORMAT_CHANGER_H */
