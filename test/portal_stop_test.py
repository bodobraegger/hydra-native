#!/usr/bin/env python3
"""Checks that build/hydra-native-capture closes an open picker and ends at once when it is stopped while the
picker waits for the user: by SIGTERM (a new evaluation or the end of hydra-native) and by the end of the program
that reads its frames. A mock portal on a private D-Bus bus stands for xdg-desktop-portal, so no dialog opens."""
import os
import pathlib
import signal
import subprocess
import sys
import threading
import time

import warnings

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

# register_object is deprecated in newer PyGObject; it still works and keeps the mock short
warnings.filterwarnings("ignore", category=DeprecationWarning)

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(os.environ.get("HYDRA_NATIVE_BUILD", ROOT / "build"))
CAPTURE = BUILD / "hydra-native-capture"
PORTAL_PATH = "/org/freedesktop/portal/desktop"
MAX_STOP_SECONDS = 2.0
WAIT_SECONDS = 10.0

SCREENCAST_XML = """<node><interface name="org.freedesktop.portal.ScreenCast">
<method name="CreateSession"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
<method name="SelectSources"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
<method name="Start"><arg type="o" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
</interface></node>"""
REQUEST_XML = """<node><interface name="org.freedesktop.portal.Request"><method name="Close"/></interface></node>"""


class MockPortal:
    """CreateSession answers at once; SelectSources never answers, like a picker that stays open."""

    def __init__(self, connection):
        self.connection = connection
        self.picker_open = threading.Event()
        self.closed = threading.Event()
        self.request_info = Gio.DBusNodeInfo.new_for_xml(REQUEST_XML).interfaces[0]
        info = Gio.DBusNodeInfo.new_for_xml(SCREENCAST_XML).interfaces[0]
        connection.register_object(PORTAL_PATH, info, self.screencast_call, None, None)

    def request_path(self, sender, options):
        token = options["handle_token"]
        return f"{PORTAL_PATH}/request/{sender[1:].replace('.', '_')}/{token}"

    def screencast_call(self, connection, sender, path, interface, method, parameters, invocation):
        arguments = parameters.unpack()
        request = self.request_path(sender, arguments[-1])
        connection.register_object(request, self.request_info, self.request_call, None, None)
        invocation.return_value(GLib.Variant("(o)", (request,)))
        if method == "CreateSession":
            results = {"session_handle": GLib.Variant("s", f"{PORTAL_PATH}/session/mock")}
            connection.emit_signal(sender, request, "org.freedesktop.portal.Request", "Response",
                                   GLib.Variant("(ua{sv})", (0, results)))
        elif method == "SelectSources":
            self.picker_open.set()

    def request_call(self, connection, sender, path, interface, method, parameters, invocation):
        self.closed.set()
        invocation.return_value(None)


def run_case(name, stop, address):
    connection = Gio.DBusConnection.new_for_address_sync(
        address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
    portal = MockPortal(connection)
    owned = threading.Event()
    Gio.bus_own_name_on_connection(connection, "org.freedesktop.portal.Desktop", Gio.BusNameOwnerFlags.NONE,
                                   lambda *_: owned.set(), None)
    if not owned.wait(WAIT_SECONDS):
        return f"{name}: the mock portal did not get its name"
    environment = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address)
    helper = subprocess.Popen([str(CAPTURE), "picker", "64", "36"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              env=environment)
    if not portal.picker_open.wait(WAIT_SECONDS):
        helper.kill()
        return f"{name}: the helper never opened the picker: {helper.communicate()[1].decode()}"
    start = time.monotonic()
    stop(helper)
    try:
        helper.wait(MAX_STOP_SECONDS)
    except subprocess.TimeoutExpired:
        helper.kill()
        return f"{name}: the helper did not end within {MAX_STOP_SECONDS} s"
    seconds = time.monotonic() - start
    connection.close_sync(None)
    if not portal.closed.is_set():
        return f"{name}: the picker request was not closed"
    print(f"portal_stop_test: {name}: ended after {seconds:.3f} s, picker closed")
    return None


def main():
    daemon = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                              stdout=subprocess.PIPE, text=True)
    address = daemon.stdout.readline().strip()
    loop = GLib.MainLoop()
    threading.Thread(target=loop.run, daemon=True).start()
    try:
        errors = [error for error in (
            run_case("SIGTERM", lambda helper: helper.send_signal(signal.SIGTERM), address),
            run_case("reader closed", lambda helper: helper.stdout.close(), address),
        ) if error]
    finally:
        loop.quit()
        daemon.terminate()
        daemon.wait()
    if errors:
        sys.exit("portal_stop_test: " + "; ".join(errors))
    print("portal_stop_test: ok")


if __name__ == "__main__":
    main()
