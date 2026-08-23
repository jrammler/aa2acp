# AA2ACP

AA2ACP is a C++20 Raspberry Pi bridge that receives Android Auto from an
Android phone and presents it to a car as a wireless CarPlay phone.

> **Work in progress:** this is an experimental development project, not a
> finished consumer product. It has a verified video path, but audio, physical
> controls, wireless Android Auto, production authentication, and appliance
> packaging are still in progress. Do not rely on it while driving.

## Current state

The wired Android Auto → wireless CarPlay video path has been verified end to
end with [LIVI](https://github.com/f-io/LIVI):

- AOAP USB and Android Auto TLS/control session;
- Bluetooth iAP2, pairing, MFi test authentication, and CarPlay Wi-Fi handover;
- encrypted AirPlay control and H.264 video forwarding;
- lossless H.264 SPS metadata normalization for decoder compatibility;
- head-unit-bound capabilities caching, including a 30-second cold-start
  negotiation window before Android Auto video setup;
- persisted configuration/pairing state, rotating logs, and clean disconnect
  handling.

Media and guidance audio forwarding are verified with LIVI; system-audio
forwarding is implemented but not yet exercised with a phone-side trigger.
Call audio and physical-control forwarding are not implemented yet. See
[TODO.md](TODO.md) for the active task list and
[ARCHITECTURE.md](ARCHITECTURE.md) for the system design. The verified
end-to-end runbook and external test-rig assumptions are in
[TESTING.md](TESTING.md). The planned real-car management-network behavior is
in [DEPLOYMENT.md](DEPLOYMENT.md).

## Build and test

### Plain Linux / Raspberry Pi OS (no Nix)

Install the build dependencies (Debian/Ubuntu package names; adapt for other
distros):

```bash
sudo apt-get install -y \
  cmake g++ pkg-config git \
  libssl-dev libboost1.83-all-dev libabsl-dev protobuf-compiler libprotobuf-dev \
  libusb-1.0-0-dev libdbus-1-dev libbluetooth-dev \
  libavcodec-dev libavutil-dev
```

Build the pinned external dependencies (`deps.lock`, bundled into
`.deps/install`), then the bridge itself:

```bash
./scripts/build-deps.sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="$PWD/.deps/install"
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The dependency step is idempotent: it rebuilds only when `deps.lock` or a
patch changes.

### Nix

For Nix users: `flake.nix` provides a development shell with all dependencies
prebuilt, plus package outputs. See `flake.nix` for details.

```bash
nix develop --command bash -c '
  cmake -S . -B build
  cmake --build build -j2
  ctest --test-dir build --output-on-failure
'
```

Run the integrated daemon with:

```bash
nix run .#aa2acp
```

`aa2acp` runs the Nix check phase before its first build. For quicker Pi
iterations, use `nix run .#aa2acp-unchecked`. CMake tests are enabled by
default and can be skipped with `-DBUILD_TESTING=OFF`; standalone iAP2
diagnostic tools are opt-in with `-DAA2ACP_BUILD_DIAGNOSTICS=ON`.

The management UI listens on all IPv4 interfaces at port 8080. From a device
connected to its management hotspot, open `http://10.42.0.1:8080` (or the
hotspot gateway address if it was configured differently). Its configuration,
AirPlay pairing identity, and 30 launch-rotated timestamped log files live in
`$XDG_STATE_HOME/aa2acp` (normally `~/.local/state/aa2acp`).

For wired Android Auto, install the checked-in udev rule once and ensure the
running user belongs to the dedicated `aa2acp` group:

```bash
sudo groupadd --system aa2acp
sudo usermod -aG aa2acp "$USER"
sudo install -Dm644 udev/70-aa2acp-android-auto.rules \
  /etc/udev/rules.d/70-aa2acp-android-auto.rules
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=usb
```

Android phones use manufacturer-specific USB IDs before AA2ACP switches them
to Android Open Accessory Protocol mode, so the rule grants the trusted
`aa2acp` group access to USB devices rather than maintaining a phone-ID
allowlist. Use a dedicated service account with that group in a production
image.

## License

GPL-3.0-only. See [LICENSE](LICENSE).
