libnautilus_image_converter_sources = [
  'image-converter.c',
  'nautilus-image-converter.c',
  'nautilus-image-converter.h',
  'ic-settings.c',
  'ic-settings.h',
  'ic-crop-view.c',
  'ic-crop-view.h',
  'ic-dialog.c',
  'ic-dialog.h',
]

libnautilus_image_converter_dependencies = [
  glib,
  gtk,
  libnautilus_extension,
  m_dep,
]

libnautilus_image_converter = library('nautilus-image-converter',
  libnautilus_image_converter_sources,
  dependencies: libnautilus_image_converter_dependencies,
  include_directories: include_dir,
  install: true,
  install_dir: nautilus_extension_dir,
)
