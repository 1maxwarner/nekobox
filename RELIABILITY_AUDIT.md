# Reliability audit

Audit date: 2026-07-27

## Previous installation

- GUI source lineage: NekoRay/NekoBox 4.x.
- Core: sing-box `1.9.7-neko-1`.
- The persistent `neko.log` file was empty, and no NekoBox crash dump was
  present.
- Windows Error Reporting contained a `RADAR_PRE_LEAK_64` event for
  `nekobox_core.exe` on 2026-07-14. This is a resource-growth warning, not a
  conclusive application crash.
- Windows event history showed frequent suspend/resume cycles.

## Code-level causes found

1. The previous GUI did not process `WM_POWERBROADCAST`, so a core process that
   remained alive with stale TUN handles, routes, sockets, or RPC state was not
   recreated after resume.
2. Core start/stop RPC calls had no finite transport deadline and could wait
   indefinitely after a network or power transition.
3. Core restart waited only 500 ms for termination.
4. The new upstream restart-rate-limit branch returned while retaining its
   restart mutex, preventing later recovery.
5. `download_timeout` was accidentally persisted through the
   `download_retries` field.
6. The subscription normalizer dereferenced a profile pointer before checking
   it for null.
7. Runtime logs were primarily held in the GUI and were not useful after a
   process exit.

## Applied mitigations

- Handle suspend and automatic/critical/manual resume notifications on Windows.
- Wait eight seconds after resume, then recreate the core, TUN state, and the
  previously active profile.
- Apply 15-second connect/send/receive deadlines to Thrift core calls.
- Use bounded three-second core shutdown waits and reset failed-start state
  before retrying.
- Release the restart mutex on every rate-limit path.
- Correct the timeout persistence mapping and the null-check order.
- Persist sanitized runtime logs with two-file rotation.

## Remaining validation

A real suspend/resume cycle is still the final integration test because Windows
adapter timing and driver behavior cannot be fully simulated by a unit test.
The persistent log records `[Power]` recovery markers for that verification.
