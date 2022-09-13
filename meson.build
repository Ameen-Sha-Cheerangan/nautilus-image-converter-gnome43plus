project('nautilus-image-converter',
  'c',
  version: '0.3.1',
  meson_version: '>= 0.49',
)

i18n = import('i18n')
gnome = import('gnome')

gtk = dependency('gtk4', version: '>= 4.6.0')
glib = dependency('glib-2.0', version: '>= 2.28.0')
libnautilus_extension = dependency ('libnautilus-extension-4', version: '>= 43.rc')

nautilus_extension_dir = libnautilus_extension.get_pkgconfig_variable('extensiondir')
prefix_dir = get_option('prefix')
data_dir = prefix_dir / get_option('datadir')
message(data_dir)
locale_dir = data_dir / 'locale'
gettext_package = meson.project_name()

conf = configuration_data()
conf.set_quoted('GETTEXT_PACKAGE', gettext_package)
conf.set_quoted('PACKAGE', meson.project_name())
conf.set_quoted('DATADIR', data_dir)
conf.set_quoted('GNOMELOCALEDIR', locale_dir)
config_file = configure_file(
  output: 'config.h',
  configuration: conf,
)

include_dir = include_directories('.')

subdir('data')
subdir('po')
subdir('src')