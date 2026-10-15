#!/usr/bin/python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Component regression only: run on a private xvfb-run display, never a user seat.

The separate test-remote-desktop.py exercises consented public portal assistance.
This witness loads a specified development Muffin library and uses its normal
Clutter seat factory, so stable and development implementations cannot be mixed.
"""
import argparse
import ctypes
import gc
import hashlib
import json
import os
from pathlib import Path
import weakref

import gi

gi.require_version("Clutter", "0")
from gi.repository import Clutter  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--muffin-library", required=True, type=Path)
parser.add_argument("--case", choices=("evdev", "scroll", "dispose"), required=True)
args = parser.parse_args()
if not os.environ.get("DISPLAY") or os.environ.get("LINDM_ISOLATED_X11_TEST") != "1":
    parser.error("use xvfb-run with LINDM_ISOLATED_X11_TEST=1, not a user display")
library = args.muffin_library.resolve(strict=True)
muffin = ctypes.CDLL(str(library), mode=ctypes.RTLD_GLOBAL)
glib = ctypes.CDLL("libglib-2.0.so.0")
clutter = ctypes.CDLL("libmuffin-clutter-0.so.0")
xlib = ctypes.CDLL("libX11.so.6")

# These public initialization functions are deliberately skipped by Meta's GIR.
muffin.meta_get_option_context.restype = ctypes.c_void_p
context = muffin.meta_get_option_context()
argv = (ctypes.c_char_p * 3)(b"test-muffin-x11-input", b"--x11", None)
argv_pointer = ctypes.cast(argv, ctypes.POINTER(ctypes.c_char_p))
argc = ctypes.c_int(2)
error = ctypes.c_void_p()
glib.g_option_context_parse.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.POINTER(ctypes.c_char_p)), ctypes.POINTER(ctypes.c_void_p)]
glib.g_option_context_parse.restype = ctypes.c_int
assert glib.g_option_context_parse(context, ctypes.byref(argc), ctypes.byref(argv_pointer), ctypes.byref(error))
glib.g_option_context_free.argtypes = [ctypes.c_void_p]
glib.g_option_context_free(context)
muffin.meta_init.argtypes = []
muffin.meta_init.restype = None
muffin.meta_init()
seat = Clutter.get_default_backend().get_default_seat()
clutter.clutter_x11_get_default_display.restype = ctypes.c_void_p
display = clutter.clutter_x11_get_default_display()
assert display
xlib.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
xlib.XQueryKeymap.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
xlib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
xlib.XDefaultRootWindow.restype = ctypes.c_ulong
xlib.XQueryPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_uint)]
xlib.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
xlib.XKeysymToKeycode.restype = ctypes.c_ubyte
root = xlib.XDefaultRootWindow(display)


def keys():
    xlib.XSync(display, False)
    result = ctypes.create_string_buffer(32)
    xlib.XQueryKeymap(display, result)
    return result.raw


def buttons():
    xlib.XSync(display, False)
    window, child = ctypes.c_ulong(), ctypes.c_ulong()
    x, y, rx, ry = (ctypes.c_int() for _ in range(4))
    mask = ctypes.c_uint()
    assert xlib.XQueryPointer(display, root, ctypes.byref(window), ctypes.byref(child), ctypes.byref(rx), ctypes.byref(ry), ctypes.byref(x), ctypes.byref(y), ctypes.byref(mask))
    return mask.value & (0x1f << 8)


keyboard = seat.create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE)
pointer = seat.create_virtual_device(Clutter.InputDeviceType.POINTER_DEVICE)
result = {"case": args.case, "muffin": str(library), "sha256": hashlib.sha256(library.read_bytes()).hexdigest()}
if args.case == "evdev":
    keyboard.notify_key(0, 42, Clutter.KeyState.PRESSED)
    state = keys()
    result["x11_shift_keycode_50_pressed"] = bool(state[50 // 8] & (1 << (50 % 8)))
    keyboard.notify_key(0, 42, Clutter.KeyState.RELEASED)
    passed = result["x11_shift_keycode_50_pressed"]
elif args.case == "scroll":
    class Event(ctypes.Union):
        _fields_ = [("type", ctypes.c_int), ("storage", ctypes.c_long * 24)]
    xlib.XSelectInput.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_long]
    xlib.XPending.argtypes = [ctypes.c_void_p]
    xlib.XNextEvent.argtypes = [ctypes.c_void_p, ctypes.POINTER(Event)]
    xlib.XSelectInput(display, root, (1 << 2) | (1 << 3))
    xlib.XSync(display, False)
    event = Event()
    while xlib.XPending(display):
        xlib.XNextEvent(display, ctypes.byref(event))
    pointer.notify_scroll_continuous(0, 0, 12, Clutter.ScrollSource.FINGER, Clutter.ScrollFinishFlags.NONE)
    xlib.XSync(display, False)
    presses = 0
    while xlib.XPending(display):
        xlib.XNextEvent(display, ctypes.byref(event))
        presses += event.type == 4  # ButtonPress
    result["wheel_presses"] = presses
    passed = presses == 1
else:
    before_keys, before_buttons = keys(), buttons()
    symbol = 0x0101F4A1
    assert xlib.XKeysymToKeycode(display, symbol) == 0
    keyboard.notify_key(0, 42, Clutter.KeyState.PRESSED)
    keyboard.notify_keyval(0, symbol, Clutter.KeyState.PRESSED)
    pointer.notify_button(0, 1, Clutter.ButtonState.PRESSED)
    assert keys() != before_keys and buttons() & (1 << 8)
    reserved = xlib.XKeysymToKeycode(display, symbol)
    assert reserved
    keyboard_weak, pointer_weak = weakref.ref(keyboard), weakref.ref(pointer)
    keyboard = pointer = None
    gc.collect()
    assert keyboard_weak() is None and pointer_weak() is None
    result.update(keys_released=keys() == before_keys, buttons_released=buttons() == before_buttons)
    # Normal release returns a code to Muffin's bounded five-code pool; it does
    # not immediately erase the cached XKB mapping. Exhaust the unused slots
    # with held symbols and require that a subsequent device can reuse it.
    probe = seat.create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE)
    probe_codes = []
    for probe_symbol in range(symbol + 1, symbol + 6):
        assert xlib.XKeysymToKeycode(display, probe_symbol) == 0
        probe.notify_keyval(0, probe_symbol, Clutter.KeyState.PRESSED)
        xlib.XSync(display, False)
        probe_codes.append(xlib.XKeysymToKeycode(display, probe_symbol))
    result.update(reserved_keycode=reserved, reservation_probe_codes=probe_codes,
                  reserved_code_reused=reserved in probe_codes)
    probe = None
    gc.collect()
    passed = all(result[name] for name in ("keys_released", "buttons_released", "reserved_code_reused"))
result["passed"] = passed
print(json.dumps(result), flush=True)
raise SystemExit(0 if passed else 1)
