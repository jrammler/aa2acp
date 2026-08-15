# Remaining work

## Video and display

- [ ] Prepare CarPlay as Android Auto connects, then discover and cache its
  display profile before replying to Android Auto service discovery. Bind the
  cache to the configured head unit and invalidate it when that device changes.
- [ ] Configure Android Auto video from that profile; retain 1280×720 as a
  bounded-timeout fallback.
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
- [ ] Package the daemon as a minimal Raspberry Pi image with ignition/power
  handling and persistent state.

## Boundaries

The bridge currently has verified wired Android Auto to wireless CarPlay H.264
video with LIVI. The development MFi implementation and LIVI test setup are
external test infrastructure, not part of this repository or a production
certification story.
