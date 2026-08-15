# Architecture and roadmap

The bridge runs on a Raspberry Pi-class Linux computer. A phone connects by
wired Android Auto today (wireless Android Auto is planned); the bridge joins
the car's wireless CarPlay network and acts as the CarPlay phone.

```
Android phone ── Android Auto ──> ACP-AA Bridge ── CarPlay ──> car head unit
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
  re-encoding for decoder interoperability observed with the test head unit.

## Next milestones

1. Discover and cache the CarPlay display profile, then use it when configuring
   Android Auto video. Keep 1280×720 as the conservative fallback.
2. Forward Android Auto media, guidance, and system audio to CarPlay; return
   microphone audio to Android Auto.
3. Translate CarPlay physical controls to Android Auto input. Prioritize
   directional, select, back, home, and media controls for non-touch cars.
4. Soak-test connect, unplug, replug, shutdown, reconnect, and persisted state.
5. Add wireless Android Auto using a dedicated second Wi-Fi radio.
6. Package the daemon in a minimal appliance image with ignition/power handling
   and persistent state.

The development MFi implementation and test head unit test setup are external test
infrastructure; they are not part of this repository or a production
certification story.
