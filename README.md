# xdg-desktop-portal-xapp

A backend implementation for [xdg-desktop-portal](http://github.com/flatpak/xdg-desktop-portal)
that is using GTK and various pieces of Cinnamon/MATE/Xfce4 infrastructure.

## Cinnamon screen sharing and remote control

This branch reuses the ScreenCast implementation from upstream pull request
#31. It additionally implements RemoteDesktop version 1 using Muffin's
remote desktop session, with the selected ScreenCast streams associated by
`remote-desktop-session-id`.

Muffin must be built with `remote_desktop=true` and PipeWire must be available.
The Mint packaging currently disables that build option.

Every control session requires a local confirmation describing the requested
keyboard and pointer access. Sharing sources are selected separately. A
visible Stop sharing window remains while a session is active. Closing that
window, locking/leaving the graphical session, losing logind/the compositor,
closing the portal session or losing the portal frontend ends sharing.
Pressed remote keys and buttons are released before the compositor session
is stopped. The screensaver must report `LockedHint` through logind; missing
lock state fails closed. Logind owns the lock monitor, not the normally
short-lived screensaver process.

Sessions are bound to the creating frontend connection and application ID.
Remote input is accepted only after consent and successful startup, only for
the granted device types and, for absolute motion, only for a node belonging
to the session. Permissions are not persisted. Touch input and libei are not
advertised.

The interactive native regression test is
`python3 src/screencast/test-remote-desktop.py` inside Cinnamon X11. Accept
control and select a monitor in the real dialogs. It decodes PipeWire frames,
checks real GTK keyboard/pointer delivery after 35 seconds, and checks held
input release and rejection after closing the session. Repeat with `--decline`,
`--cancel`, `--stop`, and `--lock` for the local refusal and teardown paths.
The test requires PyGObject, GTK 3, GStreamer and `gstreamer1.0-pipewire`.
