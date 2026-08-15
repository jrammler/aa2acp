# Remaining work

## Video and display

- [x] Prepare CarPlay as Android Auto connects, then discover and cache its
  head-unit capabilities before replying to Android Auto service discovery. The cache
  is bound to the configured head unit and is invalidated when that device
  changes.
- [x] Configure Android Auto video from that profile, with 1280×720 as a
  bounded-timeout fallback.
- [x] Validate cached reconnect and a cold head-unit-capabilities negotiation
  against the target head unit at 1920×1080.

## Audio and controls

- [x] Forward Android Auto media and guidance audio to CarPlay. System-audio
  forwarding is implemented but still needs a phone-side test trigger.
- [ ] Bridge call audio: forward Android Auto telephony audio to CarPlay and
  return microphone audio from CarPlay to Android Auto.
- [ ] Translate CarPlay physical controls to Android Auto input: directional,
  select, back, home, and media controls. Support non-touch head units first.
- [ ] Audit every AASDK service channel against CarPlay capabilities, documenting
  the viable mappings and advertising only the end-to-end supported intersection.

## Reliability and deployment

- [ ] Add a dedicated management Wi-Fi hotspot on a second radio, using
  NetworkManager shared mode for DHCP. Bind the management UI to that network;
  provide SSID and WPA2 password settings in the UI while keeping the
  car-facing Wi-Fi radio dedicated to wireless CarPlay. On first boot, choose
  a deterministic usable Wi-Fi adapter, start the hotspot, and persist that
  management-adapter choice. With one adapter, stop the hotspot while an
  active wired-Android-Auto session uses it for CarPlay; require a second
  adapter only before enabling wireless Android Auto.
- [ ] Soak-test first pairing, existing pairing, unplug, replug, daemon
  shutdown, restart, and retained state.
- [ ] Add wireless Android Auto with a dedicated second Wi-Fi radio.
- [ ] Package the daemon as a minimal Raspberry Pi image with ignition/power
  handling and persistent state.

## Boundaries

The bridge currently has verified wired Android Auto to wireless CarPlay H.264
video with LIVI. The development MFi implementation and LIVI test setup are
external test infrastructure, not part of this repository or a production
certification story.
