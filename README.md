# AA2ACP

AA2ACP is a C++20 Raspberry Pi bridge that receives Android Auto from an
Android phone and presents it to a car as a wireless CarPlay phone.

> **Work in progress:** this is an experimental development project, not a
> finished consumer product. It has a verified video path, but audio, physical
> controls, wireless Android Auto, production authentication, and appliance
> packaging are still in progress. Do not rely on it while driving.

## Current state

The wired Android Auto → wireless CarPlay video path has been verified end to
end with a development CarPlay head unit:

- AOAP USB and Android Auto TLS/control session;
- Bluetooth iAP2, pairing, MFi test authentication, and CarPlay Wi-Fi handover;
- encrypted AirPlay control and H.264 video forwarding;
- lossless H.264 SPS metadata normalization for decoder compatibility;
- persisted configuration/pairing state, rotating logs, and clean disconnect
  handling.

Audio and physical-control forwarding are not implemented yet. See
[TODO.md](TODO.md) for the active task list and
[ARCHITECTURE.md](ARCHITECTURE.md) for the system design. The verified
end-to-end runbook and external test-rig assumptions are in
[TESTING.md](TESTING.md).

## Build and test

All project commands run through the Nix development shell:

```bash
nix develop --command bash -c '
  cmake -S . -B build
  cmake --build build -j2
  ctest --test-dir build --output-on-failure
'
```

Run the integrated daemon with:

```bash
nix develop --command ./build/aa2acp-bridge-daemon
```

The management UI listens on `http://127.0.0.1:8080`. Its configuration,
AirPlay pairing identity, and 30 launch-rotated timestamped log files live in
`$XDG_STATE_HOME/aa2acp` (normally `~/.local/state/aa2acp`).

For wired Android Auto, install the checked-in udev rule once and ensure the
running user belongs to `plugdev`:

```bash
sudo install -Dm644 udev/70-aa2acp-android-auto.rules \
  /etc/udev/rules.d/70-aa2acp-android-auto.rules
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=18d1
```

## License

GPL-3.0-only. See [LICENSE](LICENSE).
