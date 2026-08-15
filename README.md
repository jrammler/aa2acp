# Pi bridge daemon

This is the production-oriented C++20 implementation of the phone-side
CarPlay emulator. It is being ported incrementally from `../iphone-emulator/`.
See `../ARCHITECTURE.md` for the system plan and milestone sequence.

## Current milestone

`iap2-tcp` establishes the phone-side iAP2 link layer over TCP. It repeatedly
sends the iAP2 marker while detecting the accessory, negotiates LSP with
SYN/ACK packets, and exits successfully after reaching `NORMAL`. Its
`--bootstrap` mode performs the minimal CSM identification and software-MFi
test flow; it has been verified against test head unit from both the development machine
and the development device, including OpenSSL ECDSA-SHA-256 signature verification.

The `--carplay` probe is also verified on 2026-08-15. test head unit sent its five
subscription requests (`0x5000`, `0x5200`, `0xae00`, `0x4157`, `0x4154`) and a
wireless `CarPlayStartSession` naming AirPlay port 7000, over both the TCP test
path and real RFCOMM from the development device. It does not yet join test head unit's AP or
start AirPlay.

`airplay-pair-setup-probe` is the first AirPlay C++ milestone. It speaks the
RTSP Pair-Setup M1–M4 exchange: it validates test head unit's returned SRP salt/public
key, computes the M3 client proof, and cryptographically verifies M4. It was
verified against test head unit on 2026-08-15 (16-byte salt, 384-byte SRP public key).
The encrypted M5/M6 identity exchange is also verified: the client encrypts
and signs its Ed25519 identity, then decrypts and verifies test head unit's accessory
identity. Pair-Verify and the complete encrypted RTSP control plane are also
live-verified against test head unit: `/info`, session `SETUP`, screen `SETUP` (which
returns a live screen-data TCP port), and `RECORD`. The encrypted H.264 media
plane is also live-verified: it derives the screen stream key, sends avcC
configuration and ChaCha20-Poly1305 protected AVCC frames, and test head unit decodes the
test video on its display.

Build and run tests from the repository root:

```bash
nix develop --command bash -c '
  cmake -S pi-bridge -B pi-bridge/build
  cmake --build pi-bridge/build
  ctest --test-dir pi-bridge/build --output-on-failure
'
```

With test head unit's TCP test handler running on the Pi:

```bash
./pi-bridge/build/iap2-tcp --host [redacted-host] --port 12346
# Full first CSM milestone:
./pi-bridge/build/iap2-tcp --host [redacted-host] --port 12346 --bootstrap --timeout 30
```

With the development machine joined to test head unit's AP, the complete C++ AirPlay
test (including video) is:

```bash
./pi-bridge/build/airplay-pair-setup-probe --host 10.10.0.1 \
  --video iphone-emulator/test_frames.h264
```

## Bluetooth bootstrap test

`iap2-bt` opens test head unit's iAP2 RFCOMM channel (3) after the Pi is paired with the
test head unit adapter. Pairing is intentionally left to BlueZ tooling for this first
transport milestone; the production daemon will own an agent and pairing flow.
`--bootstrap` runs the same CSM identification and software-MFi validation flow
as TCP once the RFCOMM link reaches `NORMAL`.

`--carplay` extends bootstrap through `CarPlayAvailability` and waits for
`CarPlayStartSession`. It deliberately stops there: joining the AP and AirPlay
are later milestones.

`--wifi-config` additionally requests and parses the accessory Wi-Fi
configuration, without changing the local network. `--join-wifi` is the
explicit development-host test mode: it uses `nmcli` to join the supplied AP
on `wlp15s0` (override with `--wifi-interface`), then sends
`WirelessCarPlayUpdate(status=1)`. Do not use that backend as the production
network manager. `--leave-wifi` disconnects an interface while retaining its
saved NetworkManager profile, matching the intended end-of-session behaviour.

Pass `--pair` to make the client register a temporary BlueZ
`NoInputNoOutput` agent, scan, pair, and trust test head unit before opening RFCOMM.
This is the intended Just-Works path for an unpaired device; it is opt-in so
ordinary runs do not touch persistent BlueZ pairing state.

This path was acceptance-tested on 2026-08-15 from a bond removed on **both**
the development device and test head unit: discovery → pair → trust → RFCOMM → iAP2 `NORMAL`
completed without `bluetoothctl` pairing commands.

```bash
bluetoothctl
# In bluetoothctl: agent NoInputNoOutput; default-agent;
# scan on; pair [redacted-device-address]; trust [redacted-device-address]
./pi-bridge/build/iap2-bt --mac [redacted-device-address] --bootstrap --timeout 30
# Pair/trust first when the device is new or its bond was removed:
./pi-bridge/build/iap2-bt --mac [redacted-device-address] --pair --bootstrap --timeout 60
# CarPlay control-session probe:
./pi-bridge/build/iap2-bt --mac [redacted-device-address] --carplay --timeout 30
```
