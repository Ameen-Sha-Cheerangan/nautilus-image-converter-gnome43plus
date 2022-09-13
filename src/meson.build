libnautilus_image_converter_sources = [
  'image-converter.c',
  'nautilus-image-converter.c',
  'nautilus-image-converter.h',
  'nautilus-image-resizer.c',
  'nautilus-image-resizer.h',
  'nautilus-image-rotator.c',
  'nautilus-image-rotator.h',
]

libnautilus_image_converter_dependencies = [
  glib,
  gtk,
  libnautilus_extension,
]

libnautilus_image_converter = library('nautilus-image-converter',
  libnautilus_image_converter_sources,
  dependencies: libnautilus_image_converter_dependencies,
  include_directories: include_dir,
  install: true,
  install_dir: nautilus_extension_dir,
)
