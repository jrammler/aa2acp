# End-to-end test handoff

This document describes the verified development path. It deliberately records
the external test-rig assumptions without vendoring head-unit software or test
emulators into this repository.

## Verified state

AA2ACP has been tested end to end with a wired Android Auto phone and
[LIVI](https://github.com/f-io/LIVI):

1. An Android phone connects over USB and starts Android Auto.
2. AA2ACP receives the phone's H.264 video stream through AOAP/AASDK.
3. AA2ACP pairs or reuses its BlueZ bond with the configured head unit,
   completes iAP2/CSM and the CarPlay Wi-Fi handover, then establishes AirPlay.
4. The Android Auto H.264 stream is visible on the head unit.
5. Unplugging the Android phone stops the CarPlay session and returns the head
   unit to its normal screen.

Media and guidance-audio forwarding are verified; system audio awaits a
phone-side test trigger. Call audio and input/control forwarding are not
implemented. The active work is in
[TODO.md](TODO.md).

## Prerequisites

- Linux with BlueZ, NetworkManager, a Bluetooth adapter, and a Wi-Fi interface
  that can join the head unit's CarPlay Wi-Fi network.
- The Android phone must be connected to a data-capable USB port. The daemon
  needs persistent USB access; the deb package installs the udev rule and
  creates the group, or install `udev/70-aa2acp-android-auto.rules` from a
  source build and run it as a user in `aa2acp`, as described in the README.
- A LIVI development setup, or another CarPlay-capable head unit that accepts
  this development implementation. LIVI's development-only authentication
  configuration is external to this repository and is not a production
  certification configuration.

## Runbook

Build and start the checked package:

```bash
nix run .#aa2acp
```

For faster Pi iterations that skip the Nix check phase, use
`nix run .#aa2acp-unchecked`.

Open `http://127.0.0.1:8080`, scan if necessary, select the CarPlay head
unit's Bluetooth address, select the Wi-Fi interface that should join its
CarPlay network, and save. The selected device is pinned through the persistent
AirPlay pairing record; a later connection validates that same head unit. The
daemon does not start its Android Auto USB receiver until both settings are
saved; if a phone was already connected, unplug and reconnect it after setup.

Then plug in the Android phone. The normal successful sequence includes:

```text
Android Auto video stream started
 preparing CarPlay for Android Auto
CarPlay session started
... link established (NORMAL)
... Wi-Fi: joining SSID ...
... AirPlay: encrypted RECORD accepted
... forwarded Android Auto H.264 config and cached keyframe
```

The daemon keeps configuration, the AirPlay pairing identity, and up to 30
launch-rotated logs in `$XDG_STATE_HOME/aa2acp` (normally
`~/.local/state/aa2acp`). Inspect `logs/` first when a failed session is no
longer visible in the terminal.

CarPlay runs in a persistent worker process created before the daemon starts
its threads. The daemon sends start and stop commands over a local control
socket; Android Auto media continues to use its dedicated Unix sockets.

If the first AirPlay connection after Wi-Fi handover fails, the bridge waits
one second and retries the AirPlay phase once without repeating Bluetooth
pairing or the Wi-Fi handover.

## Useful diagnostics

- Run `bluetoothctl` to inspect or remove a stale BlueZ bond. AA2ACP reuses an
  existing valid bond and otherwise performs Just Works pairing.
- Use the management UI's Bluetooth scan. It scans LE and BR/EDR sequentially;
  some head units take time to become visible.
- To capture the Android Auto elementary H.264 stream during a run, set
  `AA2ACP_DUMP_H264=/tmp/android-auto.h264` before launching the daemon. This
  is for diagnosis only; it can produce a large file.
- The bridge caches CarPlay's head-unit capabilities (the main display and
  supported direct-PCM audio routes) in
  `$XDG_STATE_HOME/aa2acp/head-unit-capabilities` (normally
  `~/.local/state/aa2acp/head-unit-capabilities`) and uses a supported cached
  resolution for Android Auto. The first connection, unavailable cache, or an
  unsupported profile waits up to 30 seconds for CarPlay discovery before using
  the conservative 1280x720 fallback.

## Regression cases

Before considering a transport or lifecycle change complete, test:

- fresh Bluetooth pairing and an already-paired head unit;
- a fresh NetworkManager connection and an existing saved Wi-Fi profile;
- phone connect, unplug, replug, and daemon `SIGTERM`/`SIGINT`;
- head-unit reconnect after the previous AirPlay pairing record is retained.
