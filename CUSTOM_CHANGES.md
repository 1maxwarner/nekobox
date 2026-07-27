# Custom NekoBox changes

This private build follows `qr243vbi/nekobox` and keeps the public repository
configured as the `upstream` Git remote.

## Subscription compatibility

- Optional HAPP-like request headers with a generated synthetic device profile.
- Existing `happ://crypt*` decryption remains supported.
- JSON subscriptions accept:
  - sing-box and V2RayN full configurations;
  - individual sing-box or V2RayN outbound objects;
  - arrays of links or outbound objects;
  - `nodes`, `configs`, and `items` wrapper arrays;
  - Clash JSON;
  - SIP008 feeds with or without a `version` field.
- XHTTP imports preserve the `extra` object and accept the
  `x_padding_bytes` share-link compatibility parameter.
- Legacy `splithttp`/`split-http` VLESS links are normalized to XHTTP before
  the outbound is used by proxy or Windows TUN/VPN mode.
- Subscription names are decoded from `Profile-Title` and compatible title
  headers, MIME encoded words, percent/RFC 5987 values, and
  `Content-Disposition` filenames.

HAPP-like headers are disabled by default. Enabling them uses a synthetic
HWID, but a stable synthetic identifier can still link requests until a new
profile is generated.

## Reliability and diagnostics

- Windows suspend/resume notifications trigger a delayed core/TUN/profile
  recreation after network adapters settle.
- A remote QUIC `StreamError` with code `0` is normalized as a closed
  connection and retried once after invalidating the DNS transport.
- Three closed DNS streams within 30 seconds reset the DNS pool and network
  outbounds, including the active Hysteria session, before retrying the
  original query through a fresh session.
- Windows resume and default-interface changes force QUIC-based outbounds to
  discard their current sessions.
- Thrift core calls use finite connect, receive, and send timeouts.
- Core restart waits are bounded and the restart mutex is released on
  rate-limit exits.
- Runtime logs are written to `settings/nekobox.log` and rotated at 4 MiB.
- A leading Unicode country flag in a profile name is displayed as the
  corresponding English country name.

## Routing profiles

- Each routing profile has a persistent enabled state and numeric priority.
- Lower priority numbers are shown first and are used first as fallback when
  the currently selected profile is disabled.
- Profiles can be enabled or disabled with one click from the routing profile
  list or with the adjacent toggle button. At least one profile remains
  enabled.
- Enabled state and priority are preserved by route profile import/export.
- A detailed Russian guide with examples is available in
  `docs/ROUTING_RU.md`.

## Interface

- The Inter V variable font is embedded into the executable and used as the
  default interface font.
- An explicitly selected font in the application settings still takes
  precedence, and log views retain their fixed-width font.

## Core

The core is pinned to the private
`fabbiodev/nekobox-sing-box-core@91655874690d`, based on
`qr243vbi/sing-box v1.13.14-mod3`. It contains the XHTTP implementation and
the DNS/QUIC recovery changes used by this build.
