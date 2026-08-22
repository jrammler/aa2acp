# Deployment design

## Management hotspot

AA2ACP uses a WPA2-protected management Wi-Fi hotspot for its web interface.
NetworkManager shared mode supplies DHCP; the phone receives an address
automatically and reaches the UI at the hotspot gateway address (normally
`10.42.0.1`). The daemon configures this at runtime; systemd is only an
optional boot/restart wrapper.

On first start, AA2ACP selects the first usable Wi-Fi adapter and starts a
hotspot named `AA2ACP-<MAC suffix>-1`. Its initial WPA2 password is
`changeme`; the management UI permits no other action until the owner replaces
it. Password setup asks for confirmation and, by default, changes the SSID by
incrementing its final counter so a phone can connect to the new network
without forgetting the old one. The selected adapter, SSID, and password are
persisted, and the UI lets the owner change all three later. On a single-radio
installation, the bridge stops the hotspot only while joining the car's
CarPlay network and recreates it when that session ends.

With wired Android Auto and one Wi-Fi adapter, that adapter hosts the hotspot
while idle and leaves it to join the car's wireless-CarPlay network during an
active projection session. The UI is unavailable for that interval. Wireless
Android Auto requires a second radio so its Wi-Fi path stays independent of
the CarPlay Wi-Fi client connection.

## Raspberry Pi service

On the Raspberry Pi image, cloud-init installs and enables
`aa2acp.service`. It runs as `pi`, retains its state in
`/home/pi/.local/state/aa2acp`, and restarts after an unexpected exit. The
service starts the deployed executable at `/home/pi/.local/bin/aa2acp`; a
deployment updates that symlink to the copied Nix-store closure and restarts
the service.

The `aa2acp` group has USB-device access through the checked-in udev rule.
The service account is its only intended member. This broad access is needed
because Android phones have manufacturer-specific USB IDs before the bridge
probes and switches them to AOAP mode.
