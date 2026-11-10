#!/usr/bin/env python3
"""Write raw RGBA frames of a monitor, a screen area or a window to stdout.

Uses the GNOME Mutter ScreenCast D-Bus API directly, so no portal dialog appears.
The session lives as long as this process keeps its D-Bus connection.

usage: screencast.py monitor INDEX WIDTH HEIGHT
       screencast.py area X Y W H WIDTH HEIGHT
       screencast.py window DELAY_SECONDS WIDTH HEIGHT   (records the window focused after the delay)
"""
import signal
import subprocess
import sys
import time

from gi.repository import Gio, GLib, GLibUnix

SCREENCAST_BUS_NAME = "org.gnome.Mutter.ScreenCast"
SESSION_INTERFACE = "org.gnome.Mutter.ScreenCast.Session"
STREAM_INTERFACE = "org.gnome.Mutter.ScreenCast.Stream"
CURSOR_MODE_EMBEDDED = 1

bus = Gio.bus_get_sync(Gio.BusType.SESSION)


def call(name, path, interface, method, arguments, return_type):
    return bus.call_sync(name, path, interface, method, arguments,
                         GLib.VariantType(return_type) if return_type else None,
                         Gio.DBusCallFlags.NONE, -1, None)


def monitor_connector(index):
    state = call("org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
                 "org.gnome.Mutter.DisplayConfig", "GetCurrentState", None, None)
    monitors = state.unpack()[1]
    if not 0 <= index < len(monitors):
        sys.exit(f"screencast: no monitor {index}, there are {len(monitors)}")
    return monitors[index][0][0]


def main():
    mode, arguments = sys.argv[1], [int(value) for value in sys.argv[2:]]
    width, height = arguments[-2:]
    session = call(SCREENCAST_BUS_NAME, "/org/gnome/Mutter/ScreenCast", "org.gnome.Mutter.ScreenCast",
                   "CreateSession", GLib.Variant("(a{sv})", ({},)), "(o)")[0]
    properties = {"cursor-mode": GLib.Variant("u", CURSOR_MODE_EMBEDDED)}
    if mode == "monitor":
        stream = call(SCREENCAST_BUS_NAME, session, SESSION_INTERFACE, "RecordMonitor",
                      GLib.Variant("(sa{sv})", (monitor_connector(arguments[0]), properties)), "(o)")[0]
    elif mode == "area":
        x, y, area_width, area_height = arguments[:4]
        stream = call(SCREENCAST_BUS_NAME, session, SESSION_INTERFACE, "RecordArea",
                      GLib.Variant("(iiiia{sv})", (x, y, area_width, area_height, properties)), "(o)")[0]
    elif mode == "window":
        time.sleep(arguments[0])
        stream = call(SCREENCAST_BUS_NAME, session, SESSION_INTERFACE, "RecordWindow",
                      GLib.Variant("(a{sv})", (properties,)), "(o)")[0]
    else:
        sys.exit(__doc__)

    loop = GLib.MainLoop()
    pipeline = []

    def on_stream_added(connection, sender, path, interface, signal_name, parameters):
        node = parameters.unpack()[0]
        print(f"screencast: pipewire node {node}", file=sys.stderr)
        caps = f"video/x-raw,format=RGBA,width={width},height={height}"
        pipeline.append(subprocess.Popen([
            "gst-launch-1.0", "-q", "pipewiresrc", f"path={node}", "always-copy=true", "!",
            "videoconvert", "!", "videoscale", "!", caps, "!", "fdsink", "fd=1", "sync=false"]))
        GLib.child_watch_add(GLib.PRIORITY_DEFAULT, pipeline[0].pid, lambda *_: loop.quit())

    bus.signal_subscribe(SCREENCAST_BUS_NAME, STREAM_INTERFACE, "PipeWireStreamAdded", stream, None,
                         Gio.DBusSignalFlags.NONE, on_stream_added)
    bus.signal_subscribe(SCREENCAST_BUS_NAME, SESSION_INTERFACE, "Closed", session, None,
                         Gio.DBusSignalFlags.NONE, lambda *_: loop.quit())
    for signal_number in (signal.SIGTERM, signal.SIGINT):
        GLibUnix.signal_add(GLib.PRIORITY_DEFAULT, signal_number, lambda: loop.quit() or False)
    call(SCREENCAST_BUS_NAME, session, SESSION_INTERFACE, "Start", None, None)
    loop.run()
    for process in pipeline:
        process.terminate()
    try:
        call(SCREENCAST_BUS_NAME, session, SESSION_INTERFACE, "Stop", None, None)
    except GLib.Error:
        pass


if __name__ == "__main__":
    main()
