# Pi bridge daemon

This is the production-oriented C++20 implementation of the phone-side
CarPlay emulator. It is being ported incrementally from `../iphone-emulator/`.
See `../ARCHITECTURE.md` for the system plan and milestone sequence.

## Current milestone

### Wired Android Auto receiver

`android-auto-usb` is the wired Android Auto transport diagnostic. It uses
AASDK and libusb hotplug monitoring to find an Android phone, put a normal USB
device into Android Open Accessory Protocol (AOAP) mode, retain the resulting
transport, and report phone disconnects. It completes the Android Auto control
TLS handshake and advertises a non-touch head-unit baseline: H.264 video,
media/guidance/system PCM audio, microphone, driving-status sensor, and
physical-button input. The receiver handles channel open/configuration and
flow control for those services.

This was verified with the development Android phone on 2026-08-15: Android
Auto accepted the head unit, displayed its welcome flow, opened the H.264 video
channel, selected 1280×720@30 Baseline H.264, and started the video stream.
`bridge-daemon --run` is now the first live video bridge path: once Android
Auto starts H.264 projection, it starts an authenticated wireless CarPlay
session, joins test head unit's AP, and forwards the phone's Annex-B H.264 access units
through the established encrypted AirPlay data stream. A bounded local queue
keeps USB processing independent from CarPlay setup; it retains SPS/PPS so a
late CarPlay connection still receives the required video configuration.
Audio and head-unit input forwarding are not implemented yet.
`--run-carplay` preserves the old direct-CarPlay diagnostic mode.

The process needs read/write access to the phone's USB device node. Install the
provided udev rule once (and ensure the service/developer user belongs to
`plugdev`):

```bash
sudo install -Dm644 pi-bridge/udev/70-acp-aa-bridge-android-auto.rules \
  /etc/udev/rules.d/70-acp-aa-bridge-android-auto.rules
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=18d1
```

The checked-in development rule covers this phone's initial `18d1:4ee1` gadget
and later AOAP `18d1:2d00`/`18d1:2d01` device, so it survives the mandatory
re-enumeration. Add similarly specific IDs for another phone; a broader rule
is an appliance-image security decision, not a developer-machine default.

```bash
nix develop --command ./pi-bridge/build/android-auto-usb
# Live wired Android Auto → CarPlay video bridge; stop with SIGTERM/SIGINT:
nix develop --command ./pi-bridge/build/bridge-daemon --run
```

The daemon uses the configured head-unit MAC, Wi-Fi interface, and persistent
AirPlay pairing record. It asks BlueZ to pair if needed, then preserves the
BlueZ/NetworkManager records for fast reconnect while disconnecting test head unit's AP
after the CarPlay session ends.

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
test head unit adapter. Its optional `--pair` mode owns the first-pairing flow through a
temporary BlueZ `NoInputNoOutput` agent.
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

test head unit is intermittently visible to this host only through an explicit BlueZ LE
scan. `--pair` therefore selects the LE discovery filter before scanning. This
is an observed test head unit/BlueZ test-environment behaviour, not a claim that every
wireless-CarPlay head unit is LE-only; future hardware support needs discovery
fallbacks for Classic/BREDR-only accessories.

The first-pairing path was acceptance-tested on 2026-08-15 from a bond removed
on **both** the development host and test head unit: LE discovery → pair → trust →
RFCOMM → iAP2 `NORMAL` → Wi-Fi join → AirPlay Pair Setup/Pair Verify →
encrypted RTSP `RECORD` completed without `bluetoothctl` pairing commands.

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

## Integrated bridge and state matrix

`--bridge` combines iAP2, the received Wi-Fi configuration, the
`WirelessCarPlayUpdate`, and the AirPlay client. `--pairing-store` persists the
AirPlay controller identity and accessory public key (mode `0600`), so later
sessions run Pair Verify only. BlueZ owns the Bluetooth bond and NetworkManager
owns the saved AP profile; the bridge deliberately does not duplicate either.

```bash
nix develop --command ./pi-bridge/build/iap2-bt \
  --mac [redacted-device-address] --pair --bridge \
  --wifi-interface wlp15s0 \
  --pairing-store ~/.local/state/acp-aa-bridge/airplay-pairing.bin \
  --timeout 60
```

The following state combinations were exercised against test head unit on 2026-08-15:

| Bluetooth bond | NetworkManager `test head unit` profile | AirPlay record | Result |
| --- | --- | --- | --- |
| Present | Present | Absent | iAP2 and Wi-Fi reconnect, then Pair Setup + Pair Verify succeed. |
| Present | Present | Present | iAP2 and Wi-Fi reconnect, then Pair Verify-only AirPlay session succeeds. |
| Present | Absent | Present | iAP2 supplies fresh credentials, NetworkManager recreates the profile, then Pair Verify-only succeeds. |
| Absent on both devices | Absent | Absent | Native LE discovery, Just-Works pair/trust, Wi-Fi join, Pair Setup, Pair Verify, and encrypted `RECORD` all succeed. |

After a normal session, call `--leave-wifi` to disconnect from the car AP while
preserving its NetworkManager profile for a faster later handover. The future
daemon will do this automatically before restoring its idle management AP.
