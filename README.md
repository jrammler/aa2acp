# AA2ACP

AA2ACP is a C++20 Raspberry Pi bridge that receives Android Auto from an
Android phone over USB and presents it to a car as a wireless CarPlay phone.
It is aimed at owners of CarPlay-only head units who want to use an Android
phone with them.

```
Android phone ── Android Auto ──> AA2ACP ── CarPlay ──> car head unit
```

The bridge owns Bluetooth pairing, the CarPlay Wi-Fi handover, persistent
AirPlay pairing, and the media/control translation.

> **Work in progress:** AA2ACP does not yet connect to a real car head unit.
> Bluetooth discovery and pairing with real cars are still in progress. The
> wired Android Auto → wireless CarPlay path has been verified end to end with
> [LIVI](https://github.com/f-io/LIVI). Audio, physical controls, wireless
> Android Auto, production authentication, and appliance packaging are also
> still in progress.

## Current state

The following functionality has been verified with
[LIVI](https://github.com/f-io/LIVI):

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
[TESTING.md](TESTING.md) for the end-to-end runbook and diagnostics.

## Installation

Releases are published on [GitHub Releases](../../releases). Each release
provides two artifacts:

- `aa2acp.deb` — an arm64 Debian package with the binary, systemd units, udev
  rule, and maintainer scripts. This is what most users want.
- `aa2acp` — the raw arm64 binary, mainly useful for developers running it
  from a source tree (see Development).

Install the package:

```bash
sudo apt install ./aa2acp.deb
```

The package's post-install script creates the `aa2acp` system user and group,
enables the `aa2acp.service` systemd unit automatically, and restarts the
service on package upgrades. The service runs as the `aa2acp` system account
with its state in `/var/lib/aa2acp`.

## Usage

AA2ACP serves its management UI on a WPA2-protected Wi-Fi hotspot that the
daemon itself configures through NetworkManager shared mode (DHCP included).
Connect a phone or laptop to the hotspot and open `http://10.42.0.1:8080`
(or the hotspot gateway address if it was configured differently).

- **First start:** the bridge picks the first usable Wi-Fi adapter and starts
  a hotspot named `AA2ACP-<MAC suffix>-1` with the default password
  `changeme`. The UI permits no other action until you replace that password.
- **Changing the password:** the UI asks for confirmation and, by default,
  increments the SSID's final counter (e.g. `-1` → `-2`) so devices reconnect
  to the new network without forgetting the old one. Adapter, SSID, and
  password are persisted and can all be changed later in the UI.

Configuration, the AirPlay pairing identity, and 30 launch-rotated timestamped
log files live in `$XDG_STATE_HOME/aa2acp`: `/var/lib/aa2acp` for the deb
install, `~/.local/state/aa2acp` when running as your own user from a source
build.

Open the UI, scan if necessary, select the CarPlay head unit's Bluetooth
address, select the Wi-Fi interface that should join its CarPlay network, save,
then plug in the Android phone. See [TESTING.md](TESTING.md) for the full
runbook and expected log output.

With wired Android Auto and one Wi-Fi adapter, the adapter hosts the hotspot
while idle; during an active projection session the bridge joins the car's
wireless-CarPlay network instead, so the UI is unavailable for that interval.
Wireless Android Auto (planned) requires a second radio so its Wi-Fi path
stays independent of the CarPlay Wi-Fi client connection.

### Wired Android Auto USB access

Android phones use manufacturer-specific USB IDs before AA2ACP switches them
to Android Open Accessory Protocol mode, so the deb's udev rule grants the
`aa2acp` group access to USB devices rather than maintaining a phone-ID
allowlist; the service runs under a dedicated `aa2acp` system account that is
the group's only member.

When building from source, you run the binary as your own user and need the
same access yourself: install the checked-in udev rule and add your user to
the `aa2acp` group (you know the drill: `groupadd --system`, `usermod -aG`,
re-login), then copy `udev/70-aa2acp-android-auto.rules` into
`/etc/udev/rules.d/` and reload udev.

## Development

Two supported build paths exist. Both build the same pinned dependency set
from `deps.lock`.

### Plain Linux / Raspberry Pi OS

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

`flake.nix` provides a development shell with all dependencies prebuilt, plus
package outputs:

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

### Formatting

clang-format **21** is canonical. CI enforces it via a pinned pip package, and
the Nix dev shell provides it through `llvmPackages_21.clang-tools`. Older
versions format this codebase differently; before committing C++ changes, run:

```bash
find include src tests -type f \( -name "*.cpp" -o -name "*.hpp" \) \
  -print0 | xargs -0 clang-format -i
```

## Releases

Releases are cut by pushing a `v*` tag. CI builds the artifacts on an arm64
runner, packages the deb with CPack (including the systemd units, udev rule,
and postinst), and attaches `aa2acp.deb` and the raw `aa2acp` binary to the
GitHub Release.

## License

GPL-3.0-only. See [LICENSE](LICENSE).
