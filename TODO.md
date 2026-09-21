# Remaining work

## Video and display

- [ ] Add a management-UI display preflight that establishes the encrypted
  screen stream and sends a non-flashing color test card with optional,
  low-motion AA2ACP text (and a static reduce-motion mode), without requiring
  an Android Auto phone; report each transport and rendering milestone.
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

- [x] Add persisted management-hotspot settings: Wi-Fi adapter, SSID, and
  WPA2 password; expose them in the management UI.
- [x] Bootstrap the management hotspot on first start by selecting a
  deterministic usable Wi-Fi adapter and requiring replacement of its default
  password before other configuration is available.
- [x] Manage the hotspot lifecycle through NetworkManager shared mode (DHCP)
  and bind the management UI to its network.
- [x] Run the bridge as a restartable systemd service on the Raspberry Pi
  image, with declarative USB permissions for arbitrary Android phones.
- [ ] Soak-test first pairing, existing pairing, unplug, replug, daemon
  shutdown, restart, and retained state.
- [ ] Support the CarPlayAvailability/CarPlayStartSession wireless handoff
  variant alongside the direct Wi-Fi-configuration handoff, selected from the
  head unit's advertised iAP2 control-message lists.
- [ ] Discover the direct Wi-Fi handoff AirPlay endpoint using mDNS instead of
  assuming the DHCP gateway and a fixed port.
- [ ] Provide a management-UI preflight diagnostic that enables structured
  protocol logging, locally redacts identifiers, credentials, addresses, and
  pairing material, then exports an issue-ready support bundle for
  head-unit-specific interoperability reports.
- [ ] Add sanitized head-unit protocol fixtures under `tests/fixtures/` that
  assert parsing and normalized outgoing messages. Keep raw car captures
  outside the repository, and verify committed fixtures contain no credentials,
  pairing keys, MAC/IP addresses, or other identifying data.
- [ ] Inject transport, clock, Bluetooth, Wi-Fi, and AirPlay dependencies so
  pairing, reconnect, handoff, timeout, and failure-path state machines can be
  tested deterministically without hardware.
- [ ] Define anonymized capability profiles for each verified head unit,
  covering iAP2 messages, Wi-Fi handoff, AirPlay capabilities, and negotiated
  formats, and run the regression suite against every profile in CI.
- [ ] Provision and validate the Apple/MFi trust chain for software-MFi
  authentication. The current major-2 verifier intentionally accepts the
  signer certificate carried by the accessory; do not enable chain checking
  until a redistributable/pinned trust anchor is available.
- [ ] Confirm the exact major-2 RSA signature encoding on sanitized head-unit
  captures; the current verifier accepts the observed raw-challenge form and
  retains a legacy DigestInfo compatibility path.
- [ ] Support MFi authentication protocol major 3, including its 32-byte
  challenge and SHA-256 signature-verification path while retaining the
  verified 20-byte SHA-1 major-2 path.
- [ ] Add wireless Android Auto with a dedicated second Wi-Fi radio.
- [ ] Package the daemon as a minimal Raspberry Pi image with ignition/power
  handling and persistent state.

## Boundaries

The bridge is experimental and not a production certification story. Real
head units vary in their iAP2 and AirPlay behavior; compatibility work is
tracked above.
