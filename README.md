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
window, locking/blanking the screen, losing the screensaver/compositor, closing
the portal session or losing the portal frontend ends sharing. Pressed remote
keys and buttons are released before the compositor session is stopped.

Sessions are bound to the creating frontend connection and application ID.
Remote input is accepted only after consent and successful startup, only for
the granted device types and, for absolute motion, only for a node belonging
to the session. Permissions are not persisted. Touch input and libei are not
advertised.
