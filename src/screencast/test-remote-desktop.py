#!/usr/bin/python3
"""Interactive native test: run inside the Cinnamon X11 session, then consent.

Requires PyGObject, GTK 3, GStreamer and gstreamer1.0-pipewire. No private
compositor APIs are used. --decline expects the local Cancel button;
--cancel closes the public pending request; --lock expects loginctl lock-session;
--stop expects the local Stop sharing button; --backend-loss expects the actual
backend process to exit. --client-loss disconnects this public portal client.
--view-only tests ScreenCast without input; --window tests selected-window capture.
Default: close the public session.
"""
import argparse
import hashlib
import json
import os
import sys
import time
import uuid

import gi

gi.require_version('Gtk', '3.0')
gi.require_version('Gdk', '3.0')
gi.require_version('Gst', '1.0')
from gi.repository import Gdk, Gio, GLib, Gst, Gtk

parser = argparse.ArgumentParser(description=__doc__)
mode = parser.add_mutually_exclusive_group()
mode.add_argument('--decline', action='store_true')
mode.add_argument('--cancel', action='store_true')
mode.add_argument('--lock', action='store_true')
mode.add_argument('--stop', action='store_true')
mode.add_argument('--backend-loss', action='store_true')
mode.add_argument('--client-loss', action='store_true')
mode.add_argument('--view-only', action='store_true')
mode.add_argument('--window', action='store_true')
args = parser.parse_args()
Gst.init(None)
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
name = 'org.freedesktop.portal.Desktop'
path = '/org/freedesktop/portal/desktop'
remote = 'org.freedesktop.portal.RemoteDesktop'
cast = 'org.freedesktop.portal.ScreenCast'
session = None
pipeline = None
pipewire_fd = None
closed = []
released = {'shift': 0, 'button': 0}
scrolls = []
status = 1


def record(stage, **details):
    print(json.dumps(dict(stage=stage, **details)), flush=True)


def spin(milliseconds):
    loop = GLib.MainLoop()
    GLib.timeout_add(milliseconds, lambda: (loop.quit(), False)[1])
    loop.run()


def call(interface, method, signature, values, object_path=path):
    return bus.call_sync(name, object_path, interface, method,
                         GLib.Variant(signature, values), None,
                         Gio.DBusCallFlags.NONE, 10000, None)


def request(interface, method, signature, values, cancel=False):
    responses = {}
    loop = GLib.MainLoop()

    def response(connection, sender, object_path, iface, signal, parameters):
        responses[object_path] = parameters.unpack()
        loop.quit()

    subscription = bus.signal_subscribe(name, 'org.freedesktop.portal.Request',
                                         'Response', None, None,
                                         Gio.DBusSignalFlags.NONE, response)
    try:
        handle = call(interface, method, signature, values).unpack()[0]
        record('request', method=method, handle=handle)
        if cancel:
            call('org.freedesktop.portal.Request', 'Close', '()', (), handle)
            return None, {}  # Request.Close deliberately has no Response signal.
        deadline = GLib.timeout_add_seconds(120, lambda: (loop.quit(), False)[1])
        while handle not in responses:
            loop.run()
            if handle not in responses:
                raise RuntimeError('Timed out waiting for ' + method)
        GLib.source_remove(deadline)
        result, details = responses[handle]
        record('response', method=method, result=result, details=details)
        return result, details
    finally:
        bus.signal_unsubscribe(subscription)


def options(**values):
    return dict(handle_token=GLib.Variant('s', 'mint' + uuid.uuid4().hex), **values)


def notify(method, signature, *values):
    call(remote, method, '(oa{sv}' + signature + ')', (session, {}, *values))


def sample_hash():
    sample = pipeline.get_by_name('frames').emit('try-pull-sample', 10 * Gst.SECOND)
    if sample is None:
        message = pipeline.get_bus().pop_filtered(Gst.MessageType.ERROR)
        raise RuntimeError('No real PipeWire frame: ' + str(message.parse_error() if message else 'timeout'))
    buffer = sample.get_buffer()
    digest = hashlib.sha256(buffer.extract_dup(0, buffer.get_size())).hexdigest()
    record('decoded-frame', caps=sample.get_caps().to_string(), bytes=buffer.get_size(), sha256=digest)
    return digest


window = Gtk.Window(title='Mint public portal input witness')
window.set_default_size(640, 420)
window.move(100, 100)
box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=20)
window.add(box)
entry = Gtk.Entry()
entry.set_placeholder_text('Remote keyboard nonce appears here')
box.pack_start(entry, False, False, 0)
area = Gtk.DrawingArea()
area.add_events(Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.BUTTON_RELEASE_MASK |
                Gdk.EventMask.SCROLL_MASK | Gdk.EventMask.SMOOTH_SCROLL_MASK)
area.connect('draw', lambda widget, cr: (cr.set_source_rgb(0.1, 0.65, 0.2), cr.paint(), False)[2])
area.connect('button-release-event', lambda widget, event: released.update(button=released['button'] + 1))
area.connect('scroll-event', lambda widget, event: scrolls.append(int(event.direction)))
box.pack_start(area, True, True, 0)
window.connect('key-release-event', lambda widget, event: released.update(shift=released['shift'] + (event.keyval == Gdk.KEY_Shift_L)))
window.show_all()


def run():
    global session, pipeline, pipewire_fd, status, bus
    try:
        result, data = request(cast if args.view_only else remote, 'CreateSession', '(a{sv})',
                               (options(session_handle_token=GLib.Variant('s', 's' + uuid.uuid4().hex)),))
        assert result == 0, data
        session = data['session_handle']
        bus.signal_subscribe(name, 'org.freedesktop.portal.Session', 'Closed', session,
                             None, Gio.DBusSignalFlags.NONE,
                             lambda *signal: closed.append(True))
        if not args.view_only:
            assert request(remote, 'SelectDevices', '(oa{sv})',
                           (session, options(types=GLib.Variant('u', 3))))[0] == 0
        assert request(cast, 'SelectSources', '(oa{sv})',
                       (session, options(types=GLib.Variant('u', 2 if args.window else 1),
                                         multiple=GLib.Variant('b', False),
                                         cursor_mode=GLib.Variant('u', 2))))[0] == 0
        record('local-consent-required')
        result, data = request(cast if args.view_only else remote, 'Start', '(osa{sv})',
                               (session, '', options()), args.cancel)
        if args.cancel or args.decline:
            assert result is None if args.cancel else result == 1, 'Consent was not cancelled'
            spin(500)
            assert closed, 'Pending consent cancellation did not close the session'
            try:
                notify('NotifyKeyboardKeysym', 'iu', ord('x'), 1)
            except GLib.Error:
                record('input-rejected-after-denial')
            else:
                raise AssertionError('Input accepted without consent')
            record('consent-denied', cancellation=args.cancel, closed=True)
            status = 0
            return False
        assert result == 0, data
        if not args.view_only:
            assert data['devices'] == 3, data
        node = data['streams'][0][0]
        reply, descriptors = bus.call_with_unix_fd_list_sync(
            name, path, cast, 'OpenPipeWireRemote', GLib.Variant('(oa{sv})', (session, {})),
            GLib.VariantType.new('(h)'), Gio.DBusCallFlags.NONE, 10000, None, None)
        pipewire_fd = descriptors.get(reply.unpack()[0])
        pipeline = Gst.parse_launch('pipewiresrc fd=%d path=%d do-timestamp=true ! videoconvert ! video/x-raw,format=RGB ! appsink name=frames max-buffers=1 drop=true' % (pipewire_fd, node))
        pipeline.set_state(Gst.State.PLAYING)
        first = sample_hash()
        if args.view_only or args.window:
            assert data['streams'][0][1]['source_type'] == (2 if args.window else 1), data
            call('org.freedesktop.portal.Session', 'Close', '()', (), session)
            record('capture-passed', view_only=args.view_only, window=args.window)
            status = 0
            return False
        # The screensaver exits normally after 30 seconds. Sharing must survive.
        spin(35000)
        assert not closed, 'Sharing ended when the idle screensaver exited'
        window.present()
        entry.grab_focus()
        spin(300)
        for character in 'mint2026':
            notify('NotifyKeyboardKeysym', 'iu', ord(character), 1)
            notify('NotifyKeyboardKeysym', 'iu', ord(character), 0)
        notify('NotifyKeyboardKeycode', 'iu', 30, 1)  # Linux KEY_A, not X keycode 38
        notify('NotifyKeyboardKeycode', 'iu', 30, 0)
        spin(300)
        assert entry.get_text() == 'mint2026a', entry.get_text()
        origin = window.get_window().get_origin()[-2:]
        point = area.translate_coordinates(window, area.get_allocated_width() // 2,
                                            area.get_allocated_height() // 2)
        x, y = origin[0] + point[0], origin[1] + point[1]
        notify('NotifyPointerMotionAbsolute', 'udd', node, float(x), float(y))
        notify('NotifyPointerButton', 'iu', 272, 1)
        notify('NotifyPointerButton', 'iu', 272, 0)
        spin(300)
        assert released['button'] > 0, 'Remote click did not reach the native witness'
        notify('NotifyPointerMotion', 'dd', 7.0, -5.0)
        spin(100)
        pointer = Gdk.Display.get_default().get_default_seat().get_pointer()
        position = pointer.get_position()
        assert position[-2] > x and position[-1] < y, 'Relative pointer motion did not reach X11'
        before_scroll = len(scrolls)
        notify('NotifyPointerAxisDiscrete', 'ui', 0, 1)
        spin(200)
        assert len(scrolls) > before_scroll, 'Discrete wheel did not reach the native witness'
        before_scroll = len(scrolls)
        notify('NotifyPointerAxis', 'dd', 0.0, 12.0)
        spin(200)
        assert len(scrolls) > before_scroll, 'Smooth wheel did not reach the native witness'
        second = sample_hash()
        assert first != second, 'Captured frames did not reflect the changed desktop'
        record('native-control-verified', keyboard=entry.get_text(), clicks=released['button'],
               scrolls=scrolls, lifetime_seconds=35)
        before = dict(released)
        notify('NotifyKeyboardKeycode', 'iu', 42, 1)  # Leave Shift held until teardown.
        notify('NotifyPointerButton', 'iu', 272, 1)
        spin(200)
        if args.lock or args.stop or args.backend_loss:
            record('awaiting-local-stop', lock=args.lock, backend_loss=args.backend_loss)
            deadline = time.monotonic() + 90
            while not closed and time.monotonic() < deadline:
                spin(100)
        elif args.client_loss:
            bus.set_exit_on_close(False)
            bus.close_sync(None)
            spin(500)
            bus = Gio.DBusConnection.new_for_address_sync(
                os.environ['DBUS_SESSION_BUS_ADDRESS'],
                Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
                None, None)
        else:
            call('org.freedesktop.portal.Session', 'Close', '()', (), session)
            spin(500)
        assert closed or not (args.lock or args.stop or args.backend_loss), 'No Closed signal after local stop'
        pointer = Gdk.Display.get_default().get_default_seat().get_pointer()
        held_mask = Gdk.ModifierType.SHIFT_MASK | Gdk.ModifierType.BUTTON1_MASK
        deadline = time.monotonic() + 5
        while True:
            _, _, _, modifiers = Gdk.get_default_root_window().get_device_position(pointer)
            if not modifiers & held_mask or time.monotonic() >= deadline:
                break
            spin(50)
        assert not modifiers & held_mask, 'Held input remains in the X server after teardown'
        # Locking changes event routing, so only assert widget delivery when unlocked.
        if not args.lock:
            assert released['shift'] > before['shift'], 'Held Shift was not released'
            assert released['button'] > before['button'], 'Held pointer button was not released'
        try:
            notify('NotifyKeyboardKeysym', 'iu', ord('x'), 1)
        except GLib.Error:
            record('input-rejected-after-close')
        else:
            raise AssertionError('Input accepted after session teardown')
        record('passed', lock=args.lock, local_stop=args.stop, backend_loss=args.backend_loss,
               client_loss=args.client_loss, releases=released)
        status = 0
    except Exception as error:
        record('failed', error=str(error))
    finally:
        if pipeline:
            pipeline.set_state(Gst.State.NULL)
        if pipewire_fd is not None:
            os.close(pipewire_fd)
        if session:
            try:
                call('org.freedesktop.portal.Session', 'Close', '()', (), session)
            except GLib.Error:
                pass
        Gtk.main_quit()
    return False


GLib.timeout_add(200, run)
Gtk.main()
sys.exit(status)
