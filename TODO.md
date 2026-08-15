# Remaining work

## Video and display

- [ ] Discover and cache the CarPlay display profile during session setup.
- [ ] Configure Android Auto video from that profile; retain 1280×720 as a
  conservative fallback.
- [ ] Validate reconnect and display renegotiation against the target head unit.

## Audio and controls

- [ ] Forward Android Auto media, guidance, and system audio to CarPlay.
- [ ] Return microphone audio from CarPlay to Android Auto.
- [ ] Translate CarPlay physical controls to Android Auto input: directional,
  select, back, home, and media controls. Support non-touch head units first.

## Reliability and deployment

- [ ] Soak-test first pairing, existing pairing, unplug, replug, daemon
  shutdown, restart, and retained state.
- [ ] Add wireless Android Auto with a dedicated second Wi-Fi radio.
- [ ] Package the daemon in a minimal appliance image with ignition/power
  handling and persistent state.

## Boundaries

The bridge currently has verified wired Android Auto to wireless CarPlay H.264
video. Any development-only head-unit authentication setup is external test
infrastructure, not part of this repository or a production certification
story.
