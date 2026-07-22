# PlatformIO extra_script: pause the air-monitor-bridge systemd user service
# around serial uploads. The bridge holds the USB console open and its
# heartbeat lines would corrupt esptool's bootloader handshake, so it must let
# go of the port while flashing. OTA envs (espota) are untouched — stopping the
# bridge there would just flip the station into power-saving mid-upload.
#
# Everything is best-effort (check=False): on a machine without the bridge
# unit / systemd, uploads behave exactly as before. If an upload dies between
# the pre- and post-action, the dead-man timer restarts the bridge within
# ~3 minutes (start on an already-running unit is a no-op, so the timer is
# harmless after a successful upload too).
Import("env")

import os
import subprocess


def _host_run(*cmd):
    # The pio upload button may run inside the VS Code flatpak; systemctl must
    # reach the host's user session from there.
    if os.path.exists("/.flatpak-info"):
        cmd = ("flatpak-spawn", "--host") + cmd
    subprocess.run(cmd, check=False)


def _pause_bridge(source, target, env):
    _host_run("systemctl", "--user", "stop", "air-monitor-bridge")
    _host_run("systemd-run", "--user", "--collect", "--on-active=180",
              "systemctl", "--user", "start", "air-monitor-bridge")


def _resume_bridge(source, target, env):
    _host_run("systemctl", "--user", "start", "air-monitor-bridge")


if env.subst("$UPLOAD_PROTOCOL") != "espota":
    for t in ("upload", "uploadfs"):
        env.AddPreAction(t, _pause_bridge)
        env.AddPostAction(t, _resume_bridge)
