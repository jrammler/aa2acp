# Architecture and roadmap

The bridge runs on a Raspberry Pi-class Linux computer. A phone connects by
wired Android Auto today (wireless Android Auto is planned); the bridge joins
the car's wireless CarPlay network and acts as the CarPlay phone.

```
Android phone ── Android Auto ──> AA2ACP ── CarPlay ──> car head unit
```

The bridge owns Bluetooth pairing, the CarPlay Wi-Fi handover, persistent
AirPlay pairing, and the media/control translation. A separate Wi-Fi radio is
required for wireless Android Auto so it remains independent from the CarPlay
Wi-Fi client connection.

## Current implementation

- Android Auto: AOAP, TLS/control, H.264 video service, and wired USB hotplug.
- CarPlay: Bluetooth iAP2/CSM, native BlueZ pairing, Wi-Fi handover, encrypted
  AirPlay pairing/control/video, and persistent identity.
- Runtime: local management UI, XDG state, rotating timestamped logs, and
  bounded child-worker shutdown on Android Auto disconnect.
- Video: live H.264 forwarding is verified; SPS metadata is normalized without
  re-encoding for decoder interoperability.

## Next milestones

1. Prepare CarPlay as Android Auto connects and read its display profile before
   replying to Android Auto service discovery. Cache the profile per head unit,
   invalidate it if the configured device changes, and use it for Android Auto
   video; keep 1280×720 as the bounded-timeout fallback.
2. Forward Android Auto media, guidance, and system audio to CarPlay; return
   microphone audio to Android Auto.
3. Translate CarPlay physical controls to Android Auto input. Prioritize
   directional, select, back, home, and media controls for non-touch cars.
4. Soak-test connect, unplug, replug, shutdown, reconnect, and persisted state.
5. Add wireless Android Auto using a dedicated second Wi-Fi radio.
6. Package the daemon as a minimal Raspberry Pi image with ignition/power
   handling and persistent state.

The development MFi implementation and LIVI test setup are external test
infrastructure; they are not part of this repository or a production
certification story.
