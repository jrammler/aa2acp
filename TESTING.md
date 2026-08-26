# Runbook and diagnostics

How to verify an AA2ACP setup end to end, and what to reach for when it
misbehaves. Installation is covered in the [README](README.md); this document
assumes a running daemon (deb service or a locally built binary).

## Verified state

AA2ACP has been tested end to end with a wired Android Auto phone and
[LIVI](https://github.com/f-io/LIVI), a CarPlay-capable head unit for
development:

1. An Android phone connects over USB and starts Android Auto.
2. AA2ACP receives the phone's H.264 video stream through AOAP/AASDK.
3. AA2ACP pairs or reuses its BlueZ bond with the configured head unit,
   completes iAP2/CSM and the CarPlay Wi-Fi handover, then establishes AirPlay.
4. The Android Auto H.264 stream is visible on the head unit.
5. Unplugging the Android phone stops the CarPlay session and returns the head
   unit to its normal screen.

Media and guidance-audio forwarding are verified; system audio awaits a
phone-side test trigger. Call audio and input/control forwarding are not
implemented. The active work is in [TODO.md](TODO.md).

LIVI's development-only authentication configuration is external to this
repository and is not a production certification setup.

## Runbook

Open the management UI (see the README), scan if necessary, select the CarPlay
head unit's Bluetooth address, select the Wi-Fi interface that should join its
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

CarPlay runs in a persistent worker process created before the daemon starts
its threads. The daemon sends start and stop commands over a local control
socket; Android Auto media continues to use its dedicated Unix sockets.

If the first AirPlay connection after Wi-Fi handover fails, the bridge waits
one second and retries the AirPlay phase once without repeating Bluetooth
pairing or the Wi-Fi handover.

## Diagnostics

- **Logs first.** Configuration, the AirPlay pairing identity, and up to 30
  launch-rotated logs live in `$XDG_STATE_HOME/aa2acp` (`/var/lib/aa2acp` for
  the deb install, `~/.local/state/aa2acp` when running as your own user).
  Inspect `logs/` when a failed session is no longer visible in the terminal.
- `bluetoothctl` inspects or removes a stale BlueZ bond. AA2ACP reuses an
  existing valid bond and otherwise performs Just Works pairing.
- The management UI's Bluetooth scan covers Bluetooth Classic (BR/EDR), which
  is the transport used for CarPlay pairing and SDP; some head units take time
  to become visible.
- Set `AA2ACP_DUMP_H264=/tmp/android-auto.h264` before launching the daemon to
  capture the raw Android Auto H.264 elementary stream. Diagnosis only; the
  file grows quickly.
- The bridge caches CarPlay's head-unit capabilities (main display, supported
  direct-PCM audio routes) in `$XDG_STATE_HOME/aa2acp/head-unit-capabilities`
  and uses a supported cached resolution for Android Auto. A first connection,
  unavailable cache, or unsupported profile waits up to 30 seconds for CarPlay
  discovery before falling back to 1280x720.
- `journalctl -u aa2acp.service` for the deb-installed service; run the binary
  in a terminal without `--no-file-log` for file logging during local runs.
- **Log level** defaults to `info` and can be raised to `debug` via the
  `AA2ACP_LOG_LEVEL` environment variable (`debug`, `info`, `warning`,
  `error`). For the deb-installed service, use a systemd drop-in:

  ```bash
  sudo systemctl edit aa2acp.service
  # add:
  #   [Service]
  #   Environment=AA2ACP_LOG_LEVEL=debug
  sudo systemctl restart aa2acp.service
  ```

## Regression cases

Before considering a transport or lifecycle change complete, test:

- fresh Bluetooth pairing and an already-paired head unit;
- a fresh NetworkManager connection and an existing saved Wi-Fi profile;
- phone connect, unplug, replug, and daemon `SIGTERM`/`SIGINT`;
- head-unit reconnect with the previous AirPlay pairing record retained;
- cached vs cold head-unit capability negotiation.
