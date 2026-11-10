#!/usr/bin/env python3
"""Write raw RGBA frames of a window or a monitor that the user picks to stdout.

Like getDisplayMedia() in the browser (initScreen() of the web editor), this uses the ScreenCast API of
xdg-desktop-portal, so GNOME shows its picker dialog. The order of the calls:
CreateSession, SelectSources (monitor or window), Start, OpenPipeWireRemote; then pipewiresrc reads the stream
through the PipeWire file descriptor of the portal.

usage: portal_screencast.py WIDTH HEIGHT [--dry-run]
--dry-run answers the portal calls with a fake bus and prints the calls instead of opening the dialog.
"""
import itertools
import os
import subprocess
import sys

from gi.repository import Gio, GLib

PORTAL_BUS_NAME = "org.freedesktop.portal.Desktop"
PORTAL_PATH = "/org/freedesktop/portal/desktop"
SCREENCAST_INTERFACE = "org.freedesktop.portal.ScreenCast"
REQUEST_INTERFACE = "org.freedesktop.portal.Request"
SOURCE_TYPES_MONITOR_AND_WINDOW = 1 | 2
CURSOR_MODE_EMBEDDED = 2
RESPONSE_SUCCESS = 0

tokens = itertools.count(1)


def pipeline_command(fd, node, width, height):
    return ["gst-launch-1.0", "-q", "pipewiresrc", f"fd={fd}", f"path={node}", "always-copy=true", "!",
            "videoconvert", "!", "videoscale", "!", f"video/x-raw,format=RGBA,width={width},height={height}", "!",
            "fdsink", "fd=1", "sync=false"]


class PortalBus:
    """The session bus; each portal method answers through the Response signal of a Request object."""

    def __init__(self):
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION)
        self.sender = self.bus.get_unique_name()[1:].replace(".", "_")

    def request(self, method, arguments, token):
        path = f"{PORTAL_PATH}/request/{self.sender}/{token}"
        loop, result = GLib.MainLoop(), {}

        def on_response(connection, sender, object_path, interface, signal_name, parameters):
            result["response"], result["results"] = parameters.unpack()
            loop.quit()

        # Subscribe before the call, so that a fast response does not get lost.
        subscription = self.bus.signal_subscribe(PORTAL_BUS_NAME, REQUEST_INTERFACE, "Response", path, None,
                                                 Gio.DBusSignalFlags.NONE, on_response)
        self.bus.call_sync(PORTAL_BUS_NAME, PORTAL_PATH, SCREENCAST_INTERFACE, method, arguments,
                           GLib.VariantType("(o)"), Gio.DBusCallFlags.NONE, -1, None)
        loop.run()
        self.bus.signal_unsubscribe(subscription)
        return result["response"], result["results"]

    def open_pipewire_remote(self, session):
        reply, fds = self.bus.call_with_unix_fd_list_sync(
            PORTAL_BUS_NAME, PORTAL_PATH, SCREENCAST_INTERFACE, "OpenPipeWireRemote",
            GLib.Variant("(oa{sv})", (session, {})), GLib.VariantType("(h)"), Gio.DBusCallFlags.NONE, -1, None, None)
        return fds.get(reply.unpack()[0])


class DryRunBus:
    """Answers like the portal and prints each call, for checks without the dialog."""

    def request(self, method, arguments, token):
        print(f"call {method} {arguments.print_(False)}")
        if method == "CreateSession":
            return RESPONSE_SUCCESS, {"session_handle": "/org/freedesktop/portal/desktop/session/dry/run"}
        if method == "Start":
            return RESPONSE_SUCCESS, {"streams": [(42, {"source_type": 2})]}
        return RESPONSE_SUCCESS, {}

    def open_pipewire_remote(self, session):
        print(f"call OpenPipeWireRemote {session}")
        return os.open(os.devnull, os.O_RDONLY)


def main():
    width, height = int(sys.argv[1]), int(sys.argv[2])
    dry_run = "--dry-run" in sys.argv
    bus = DryRunBus() if dry_run else PortalBus()

    def token():
        return f"hydra_native_{os.getpid()}_{next(tokens)}"

    request_token = token()
    response, results = bus.request("CreateSession", GLib.Variant("(a{sv})", ({
        "handle_token": GLib.Variant("s", request_token),
        "session_handle_token": GLib.Variant("s", token()),
    },)), request_token)
    if response != RESPONSE_SUCCESS:
        sys.exit("screencast: the portal refused the session")
    session = results["session_handle"]
    request_token = token()
    response, _ = bus.request("SelectSources", GLib.Variant("(oa{sv})", (session, {
        "handle_token": GLib.Variant("s", request_token),
        "types": GLib.Variant("u", SOURCE_TYPES_MONITOR_AND_WINDOW),
        "multiple": GLib.Variant("b", False),
        "cursor_mode": GLib.Variant("u", CURSOR_MODE_EMBEDDED),
    })), request_token)
    if response != RESPONSE_SUCCESS:
        sys.exit("screencast: no source selected")
    request_token = token()
    response, results = bus.request("Start", GLib.Variant("(osa{sv})", (session, "", {
        "handle_token": GLib.Variant("s", request_token),
    })), request_token)
    if response != RESPONSE_SUCCESS or not results.get("streams"):
        sys.exit("screencast: the picker was closed")
    node = results["streams"][0][0]
    fd = bus.open_pipewire_remote(session)
    command = pipeline_command(fd, node, width, height)
    if dry_run:
        print("run " + " ".join(command))
        return
    # hydra-native stops the whole process group, gst-launch-1.0 included.
    subprocess.run(command, pass_fds=(fd,))


if __name__ == "__main__":
    main()
