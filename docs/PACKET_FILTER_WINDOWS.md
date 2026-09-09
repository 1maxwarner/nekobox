# Windows Packet Filter mode

NekoBox can use the optional `Packet Filter` mode to redirect TCP and UDP
traffic from physical adapters to the local sing-box mixed inbound. This is a
WireSock-style interception layer: profile routing, VLESS Reality/gRPC,
WireGuard and AmneziaWG remain handled by the selected core profile.

The mode uses the external ProxiFyre/Windows Packet Filter runtime. The runtime
is intentionally not committed to this repository. Obtain a compatible
`ProxiFyre.exe` release together with its companion files and install the
Windows Packet Filter (NDISAPI) driver for the same architecture.

For a packaged build, configure CMake with:

```text
-DPACKET_FILTER_RUNTIME_DIR=C:/path/to/unpacked/proxifyre-x64
```

The same path can be supplied through the `PACKET_FILTER_RUNTIME_DIR`
environment variable in the Windows packaging job.

The directory must contain `ProxiFyre.exe`, `socksify.dll`, and all DLL/config
files shipped by that release. The build copies it beside `nekobox.exe` under
`packetfilter/`; at runtime NekoBox stages a writable per-user copy before
writing its SOCKS endpoint configuration. Include the matching signed
`Windows.Packet.Filter*.msi` in that directory to let NekoBox install the
driver through the normal UAC prompt on first use.

Install the matching signed NDISAPI package before enabling the mode when the
MSI is not bundled. The driver must expose the `NDISRD` device; NekoBox checks
this device and refuses to start if it is absent. The current official packages
are published at
<https://github.com/wiresock/ndisapi/releases>.

Review the ProxiFyre (AGPL-3.0) and driver licensing terms before redistributing
the runtime with a NekoBox build.

The filter excludes NekoBox and the core process to prevent loops. If the
helper or driver cannot start, NekoBox stops the profile and disables the mode
instead of leaving traffic in an ambiguous state. Packet Filter, TUN and
System Proxy are mutually exclusive interception modes.
