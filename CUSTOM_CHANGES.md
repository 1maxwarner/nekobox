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

HAPP-like headers are disabled by default. Enabling them uses a synthetic
HWID, but a stable synthetic identifier can still link requests until a new
profile is generated.

## Reliability and diagnostics

- Windows suspend/resume notifications trigger a delayed core/TUN/profile
  recreation after network adapters settle.
- Thrift core calls use finite connect, receive, and send timeouts.
- Core restart waits are bounded and the restart mutex is released on
  rate-limit exits.
- Runtime logs are written to `settings/nekobox.log` and rotated at 4 MiB.
- A leading Unicode country flag in a profile name is displayed as the
  corresponding English country name.

## Core

The upstream source currently pins `github.com/qr243vbi/sing-box`
`v1.13.14-mod3`, which contains the XHTTP implementation used by this build.
