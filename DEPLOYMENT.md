# Deployment design

## Management hotspot

AA2ACP uses a WPA2-protected management Wi-Fi hotspot for its web interface.
NetworkManager shared mode supplies DHCP; the phone receives an address
automatically and reaches the UI at the hotspot gateway address (normally
`10.42.0.1`). The daemon configures this at runtime; systemd is only an
optional boot/restart wrapper.

On first start, AA2ACP selects a deterministic usable Wi-Fi adapter, starts
the hotspot, generates initial credentials, and persists the selected adapter,
SSID, and password. The management UI lets the owner change all three.

With wired Android Auto and one Wi-Fi adapter, that adapter hosts the hotspot
while idle and leaves it to join the car's wireless-CarPlay network during an
active projection session. The UI is unavailable for that interval. Wireless
Android Auto requires a second radio so its Wi-Fi path stays independent of
the CarPlay Wi-Fi client connection.
