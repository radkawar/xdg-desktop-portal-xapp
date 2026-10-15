libexecdir_conf = configuration_data()
libexecdir_conf.set('libexecdir', prefix / libexecdir)

# Portal
install_data(
  [ 'xapp.portal', 'xapp-screencast.portal', 'xapp-gnome-keyring.portal' ],
  install_dir: datadir / 'xdg-desktop-portal' / 'portals'
)
install_data(
  'x-cinnamon-portals.conf',
  install_dir: datadir / 'xdg-desktop-portal'
)
install_data(
  '50-lindm-native-selection.conf',
  install_dir: systemduserunitdir / 'xdg-desktop-portal.service.d'
)

# dbus service
service_name = 'org.freedesktop.impl.portal.desktop.xapp'
configure_file(
  input: service_name + '.service.in',
  output: service_name + '.service',
  configuration: libexecdir_conf,
  install_dir: dbus_service_dir,
)

filename = 'xdg-desktop-portal-xapp'
# systemd unit
configure_file(
  input: filename + '.service.in',
  output: filename + '.service',
  configuration: libexecdir_conf,
  install_dir: systemduserunitdir,
)

# Desktop file
configure_file(
  input: filename + '.desktop.in',
  output: filename + '.desktop',
  configuration: libexecdir_conf,
  install: true,
  install_dir: datadir / 'applications',
)
