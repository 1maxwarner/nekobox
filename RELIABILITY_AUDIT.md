# Reliability and security audit

Audit date: 2026-07-27

## Scope and confidence

This audit covers the Windows desktop client, profile/subscription import,
routing-profile selection, the Go core wrapper, the private sing-box fork, and
the GitHub release pipeline. It combines source review, the previous
installation's Windows diagnostics, focused Go tests, a clean local Release
build, package inspection, and an isolated GUI smoke run.

The current build is materially more reliable than the previous installation,
but it is not yet a high-assurance VPN client. The main confidence gap is the
absence of automated GUI/configuration tests and a real suspend/resume
integration test.

## Previous installation and original failure mode

- GUI source lineage: NekoRay/NekoBox 4.x.
- Core: sing-box `1.9.7-neko-1`.
- The persistent `neko.log` file was empty, and no NekoBox crash dump was
  present.
- Windows Error Reporting contained a `RADAR_PRE_LEAK_64` event for
  `nekobox_core.exe` on 2026-07-14. This is a resource-growth warning, not a
  conclusive application crash.
- Windows event history showed frequent suspend/resume cycles.
- The repeated `remote StreamError(code=0)` messages were graceful
  remote-side QUIC stream cancellations that the old stack treated as terminal
  DNS errors and then kept retrying through stale session state.

## Correctness and recovery work completed

- Remote `quic.StreamError{ErrorCode: 0, Remote: true}` is normalized as a
  closed connection in both the generic DNS client and DoQ transport.
- A failed DNS exchange resets its transport and is retried once on a fresh
  session.
- Three matching failures inside 30 seconds reset the network outbound pool,
  including Hysteria/QUIC state, with a five-second reset cooldown.
- Windows power and interface notifications reset network state; the GUI also
  performs a delayed, deduplicated full core/TUN/profile recreation after
  resume.
- Core RPC calls have finite connect/send/receive deadlines.
- Core shutdown/restart waits are bounded, and failed initial launches are not
  misclassified as crashes.
- Runtime logs are persisted with rotation.
- VLESS XHTTP, legacy SplitHTTP aliases, HAPP links/headers, JSON subscription
  variants, decoded response titles, leading-flag country names, and routing
  profile enable/priority state are supported.
- The core is based on sing-box `v1.13.14`, which is the latest stable upstream
  release as of the audit date. Updating beyond it would currently mean moving
  to an upstream pre-release or development commit.

## High-priority remaining findings

### 1. Release supply chain is not reproducible or adequately pinned

The workflow uses mutable action refs such as `@main`, `@master`, and `@latest`.
Windows and Linux packaging also download the latest Cronet and continuous
AppImage tooling at build time. A rebuild of the same Git commit can therefore
produce different bytes, and compromise of any mutable dependency can become
code execution in a VPN application that is commonly run elevated.

Recommended work:

1. Pin every GitHub Action to a reviewed full commit SHA.
2. Pin Cronet, rule-set, Qt, linuxdeploy, and AppImage assets by immutable
   version and verify SHA-256 before use.
3. Generate an SBOM and provenance attestation, and publish checksums signed by
   a dedicated release key.
4. Make the release job consume only artifacts produced by the same workflow
   run and exact commit.

### 2. CMake can hide failed Go preparation and reuse a stale core

`run_go_command` does not collect or validate `execute_process`'s result.
Failures in Thrift generation, `go mod tidy`, or vendoring can be printed and
then ignored. The core custom command also depends only on its output/wrapper,
not on the Go sources, `go.mod`, or `go.sum`; an incremental build can therefore
leave an old `nekobox_core` in an otherwise new package.

Recommended work:

- make every preparation command fatal on a non-zero exit status;
- model Go source/module files as build dependencies or always rebuild the
  small Go target in release jobs;
- build in a clean directory in CI and verify the embedded module replacement
  with `go version -m`;
- fail packaging if GUI, core, updater, version, or expected resources do not
  all match.

### 3. GUI concurrency has potential use-after-free and data-race paths

Several dialogs launch detached work with lambdas capturing raw `this`.
For example, a routing-profile download continues to access the dialog and its
`chain` after the dialog can be closed. Core state and the profile manager are
also shared between the UI thread, the core thread, and detached workers using
ordinary booleans/containers plus partial manual locking.

This is a plausible explanation for intermittent crashes that leave no
actionable dump.

Recommended work:

- use `QPointer`/weak ownership in every asynchronous UI callback;
- give each background operation cancellation tied to QObject destruction;
- restrict profile/database mutation to one owner thread or protect all access
  consistently;
- replace manual `lock()/unlock()` with RAII guards;
- enable ThreadSanitizer on a Linux test target and Application Verifier/page
  heap for Windows test builds.

### 4. There is no automated regression gate for the GUI feature surface

No CMake/CTest or Qt Test target covers profile import, title decoding, XHTTP
round-trips, flag display, routing priority, database migration, process
recovery, or GUI startup. The focused Go DNS/QUIC tests are useful, but they do
not protect the C++ integration where most fork-specific behavior lives.

Recommended minimum suite:

- table-driven import corpus for URI, Base64, SIP008, Clash, sing-box, HAPP,
  malformed/oversized JSON, and response-header encodings;
- round-trip tests for VLESS XHTTP fields and `extra`;
- model tests for enabled/disabled routing profiles and equal priorities;
- fake-core tests for RPC timeouts, failed launch, rapid exit, and restart;
- Windows VM test that suspends/resumes with an active TUN profile and verifies
  DNS recovery without reboot.

### 5. Secrets are stored and exposed more broadly than necessary

Profiles, UUIDs, proxy passwords, and the inbound password are stored in the
local LevelDB/INI data model without encryption at rest. The optional QuickJS
updater environment exposes the complete process environment and complete
application configuration to replaceable resource scripts. Debug builds can
also log complete bean JSON, which contains credentials.

The UI password is an access-control check, not encryption of the profile
database.

Recommended work:

- protect sensitive values with Windows DPAPI/Credential Manager;
- expose an explicit allow-list, not the whole environment/config, to updater
  scripts;
- redact URI userinfo, UUIDs, passwords, tokens, and subscription bodies in all
  logging paths;
- clearly label settings that send stable device identifiers.

## Medium-priority findings

### 6. Resume recovery is deliberately broad and timing-based

The GUI waits a fixed eight seconds and restarts the whole core/TUN/profile.
The core also reacts to interface changes. This is robust compared with the old
behavior but can cause duplicate churn, drop all active connections, and still
be too early on a slow adapter/VPN driver.

Improve it with a recovery state machine: wait for a usable interface and
default route, invalidate the generation of all old sessions once, rebuild,
then run a bounded DNS/HTTP health check before declaring recovery complete.

### 7. DNS/QUIC healing has a large blast radius and little observability

The current three-errors-in-30-seconds threshold and one retry are fixed.
Crossing the threshold closes the shared outbound pool, interrupting unrelated
connections. There are no GUI counters for normalized closes, retries, pool
resets, or post-resume recovery duration.

Add per-outbound circuit breakers, jittered backoff, generation IDs that forbid
retry on stale sessions, and structured recovery metrics in the diagnostics
view.

### 8. A timed-out core kill can still fall through to `Start()`

`CoreProcess::Restart()` logs when the old process does not exit within three
seconds but then clears state and invokes `Start()` anyway. If the old process
or child remains alive, the new instance can fail to bind RPC/TUN resources and
the recovery attempt becomes ambiguous.

After timeout, verify the process tree is gone or abort the restart with a
specific actionable error. Do not start another core against occupied
resources.

### 9. Subscription downloads have no explicit response-size limit

The HTTP layer has a timeout but reads the response into memory without a
documented cap. A malicious or broken subscription endpoint can consume large
amounts of RAM before JSON/Base64 parsing begins.

Use separate configurable limits for headers, subscription bodies, route
profiles, and update packages; reject excessive `Content-Length` early and cap
streamed bytes.

### 10. Routing priority is fallback selection, not rule composition

Lower numeric values are considered first. If the currently selected route is
disabled or absent, the first enabled profile becomes active. Rules from
multiple enabled profiles are not merged. The UI should state this directly;
otherwise “priority” can be misunderstood as rule precedence across profiles.

Equal priorities are deterministic only because the database ID is used as the
tie-breaker.

### 11. Import compatibility is intentionally heuristic

The JSON importer recognizes several common Clash, sing-box, Hysteria, and
legacy shapes, but the parser is large and uses heuristic field aliases and
`goto`-based fallback paths. Unsupported vendor schemas can be partly imported
without a per-entry explanation. XHTTP `extra` remains user-editable JSON and
needs schema validation to prevent silent loss of unknown/invalid fields.

Return a structured import report: accepted, normalized, skipped, and rejected
entries with redacted reasons and source locations.

### 12. Stable HAPP device identity is a privacy trade-off

HAPP compatibility derives a stable 16-hex identifier from machine identity
material and sends device/OS/model headers when HAPP mode is enabled. This may
be required by a provider but allows subscription requests to be linked across
time.

Make the scope and persistence visible, support per-subscription random IDs,
and provide a one-click identity rotation with a warning about provider
activation limits.

## Maintainability and UX findings

- `mainwindow.cpp` is roughly 4,700 lines, `ConfigBuilder.cpp` roughly 1,600,
  and `GroupUpdater.cpp` roughly 1,500. They mix UI, persistence, networking,
  parsing, and process lifecycle responsibilities.
- Routing uses magic outbound IDs (`-1`, `-2`, `-3`, `-4`) in several places.
  A typed enum/value object would prevent invalid combinations.
- The fork has a large Go dependency surface and multiple third-party forks.
  Automated dependency review, license inventory, and vulnerability scanning
  are missing.
- Country names derived from leading flags are currently English (`en_CC`);
  localized territory names should follow the UI locale.
- The first-flag conversion is intentionally strict: only two leading regional
  indicator code points count as a country flag. This avoids misclassifying
  ordinary emoji but does not recognize subdivision flags.
- Recovery actions are visible only in text logs. A compact status such as
  “interface changed → sessions reset → DNS retry succeeded” would make support
  substantially easier.
- The custom core identifies itself as upstream `v1.13.14`; adding a fork
  revision/build commit to diagnostics would make bug reports reproducible.

## Recommended delivery order

1. Add the import/config/process test harness and make clean builds mandatory.
2. Eliminate mutable release dependencies and publish checksums/provenance.
3. Fix async QObject lifetimes and centralize mutable application state.
4. Replace fixed resume timers with generation-based, health-checked recovery.
5. Encrypt local secrets and reduce updater/log exposure.
6. Split the three largest source files into parser, service, model, and UI
   layers.

## Validation completed for this release

- `go test ./dns`
- `go test -tags with_quic ./dns/transport/quic`
- clean local Windows Release build of GUI, private core, and updater
- `go version -m` verification of private core revision
  `2782f5f906fc8e1424d2a5a9e3e07c8e7ec0a2a2`
- portable archive content and SHA-256 inspection
- isolated eight-second GUI startup smoke run with a separate app-data folder
- private-core Actions access via a dedicated read-only deploy key

Still required after installation: a real Windows suspend/resume cycle with an
active profile, followed by verification of `[Power]` and DNS/session recovery
markers in the persistent log.
