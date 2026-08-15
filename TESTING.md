# End-to-end test handoff

This document describes the verified development path. It deliberately records
the external test-rig assumptions without vendoring head-unit software or test
emulators into this repository.

## Verified state

AA2ACP has been tested end to end with a wired Android Auto phone and a test head unit
development head unit:

1. An Android phone connects over USB and starts Android Auto.
2. AA2ACP receives the phone's H.264 video stream through AOAP/AASDK.
3. AA2ACP pairs or reuses its BlueZ bond with the configured head unit,
   completes iAP2/CSM and the CarPlay Wi-Fi handover, then establishes AirPlay.
4. The Android Auto H.264 stream is visible on the head unit.
5. Unplugging the Android phone stops the CarPlay session and returns the head
   unit to its normal screen.

Audio and input/control forwarding are not implemented. The active work is in
[TODO.md](TODO.md).

## Local prerequisites

- Linux with BlueZ, NetworkManager, a Bluetooth adapter, and a Wi-Fi interface
  that can join the head unit's CarPlay Wi-Fi network.
- The Android phone must be connected to a data-capable USB port. The daemon
  needs persistent USB access; install `udev/70-aa2acp-android-auto.rules` and
  run it as a user in `plugdev` as described in the README.
- A CarPlay-capable head unit that accepts this development implementation.
  The current test head unit test setup uses development-only software MFi
  authentication; it is external to this repository and is not a production
  certification configuration.

## Runbook

Build and start the daemon:

```bash
nix develop --command bash -c '
  cmake -S . -B build
  cmake --build build -j2
  ./build/aa2acp-bridge-daemon
'
```

Open `http://127.0.0.1:8080`, scan if necessary, select the CarPlay head
unit's Bluetooth address, select the Wi-Fi interface that should join its
CarPlay network, and save. The selected device is pinned through the persistent
AirPlay pairing record; a later connection validates that same head unit.

Then plug in the Android phone. The normal successful sequence includes:

```text
Android Auto video stream started
starting CarPlay for Android Auto video
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

## Useful diagnostics

- Run `bluetoothctl` to inspect or remove a stale BlueZ bond. AA2ACP reuses an
  existing valid bond and otherwise performs Just Works pairing.
- Use the management UI's Bluetooth scan. It scans LE and BR/EDR sequentially;
  some head units take time to become visible.
- To capture the Android Auto elementary H.264 stream during a run, set
  `AA2ACP_DUMP_H264=/tmp/android-auto.h264` before launching the daemon. This
  is for diagnosis only; it can produce a large file.
- The current bridge does not yet negotiate the head-unit display profile, so
  Android Auto uses the conservative 1280x720 path. See the first TODO item.

## Regression cases

Before considering a transport or lifecycle change complete, test:

- fresh Bluetooth pairing and an already-paired head unit;
- a fresh NetworkManager connection and an existing saved Wi-Fi profile;
- phone connect, unplug, replug, and daemon `SIGTERM`/`SIGINT`;
- head-unit reconnect after the previous AirPlay pairing record is retained.
