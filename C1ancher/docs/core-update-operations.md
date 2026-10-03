# Core update operations

This runbook covers the independent C1 core trust chain. It does not use the application repository key, layout, sequence, or release format.

## Local lifecycle repairs (2026-09-06; not deployed)

The local implementation now uses a fixed `/etc/c1updater/recovery-verifier`, installed only by trusted enrollment or maintenance. It verifies signed generation contents before executing the verified updater inode. Normal four-component core releases cannot replace this helper or `/etc/app_daemon`.

Existing enrolled devices require an explicitly authorized `install-core-enrollment.ps1 -Action Maintenance` operation before relying on the new bootstrap contract. Maintenance checks the bundle's raw public-key hash against the existing device key before executing bundle code; local validation also requires the PEM and raw public keys to be identical. Initial enrollment still requires obtaining the bundle through a trusted provisioning process. Maintenance retains the existing key, generation pointers, transaction state, and pending-boot counter.

Bootstrap/updater capability versions are now `1.1.0`, independent of the displayed core release version. The protected `bootstrap.version` record binds the capability version to the installed bootstrap SHA-256. Signed releases requiring only the legacy `1.0.0` contract remain eligible for safe rollback; new minimum-version requirements are enforced before activation and boot.

Lock contention returns a retriable status rather than a corruption signal. The bootstrap stays the parent, tries the alternate updater after bounded ordinary failures, and handles statuses 71 (fatal), 72 (slot transition), and 75 (transient) consistently. Pending candidates may start on at most three distinct kernel boot IDs; the fourth boot rolls back before starting that candidate. Confirmation requires both the digest-bound ready marker and continuous launcher-observed UI heartbeat evidence across the 30-second observation window.

Local regressions are available through `make BUILD_DIR=build/lifecycle-fixes lifecycle-test`. Runtime logging uses the fixed helper to cap each file at 64 KiB with four rotated files; write failures disable persistence while continuing to drain output and preserve child exit handling. The Windows public-key validation test is `powershell -NoProfile -File tests/test_enrollment_host.ps1`. These tests use isolated paths and fixtures; they do not constitute physical power-loss, reboot, or maintenance acceptance. This change does not authorize deployment, stable promotion, or production signing.

## Security boundaries

- Keep the Ed25519 private key outside every repository and build output. Back it up offline before first production use.
- Install only the matching raw 32-byte public key on devices at `/etc/c1updater/core.ed25519.pub`.
- Install only the matching PEM public key on the server at `/srv/c1core/trust/core.ed25519.pem`.
- Never put either public key in a normal core release. An enrollment bundle may carry the public key because enrollment establishes the trust root.
- Never copy the server address file into a build directory or log its content. Publication reads it only after local release validation.
- Do not modify `/usr/data/c1/disable-auto-suspend` during enrollment, update, rollback, or recovery.

## Production gates

Do not enroll a production device or publish a production channel until all gates are complete:

1. The intended source tree is committed and `build-core-release.ps1` succeeds without `-AllowDirtySource`.
2. The production private key is stored and backed up outside the repositories.
3. The fixed server and device public keys have been compared byte-for-byte with the key derived from the production private key.
4. The root-owned core activation helper and maintenance migration have passed their isolated Linux/root tests; the existing Caddy proxy configuration is unchanged or separately validated.
5. HTTP GET/HEAD/Range succeeds for stable and the legacy canary URL, with byte-identical signed core responses. Write methods are rejected (403 at a rejecting proxy or 405 with `Allow: GET, HEAD` at the Go service).
6. The representative device has a fresh `/etc/app_daemon`, `/usr/data`, and `/storage` backup.
7. The fault matrix below has passed on an enrolled development device.

A failed gate stops rollout. Do not bypass source cleanliness with `-AllowDirtySource` in production.

## Build a deterministic core release

Run `scripts/build-core-release.ps1` from the `C1ancher` repository with an explicit sequence, security epoch, source date epoch, and external key path. Keep the generated PEM/raw public-key outputs outside the release directory.

The release must contain exactly:

- `manifest.v1`
- `manifest.v1.sig`
- `artifacts/C1ancher`
- `artifacts/c1pkg`
- `artifacts/C1ancher-launcher`
- `artifacts/c1updater`

Validate with `scripts/validate-core-release.sh` using the fixed PEM public key. Record sequence, version, source revision, source date epoch, manifest SHA-256, and release ID in the change record. Do not record credentials or private-key paths.

## Server architecture and single-channel maintenance

**Deployed and independently verified on 2026-09-26.**
The Go service and root publication helper now use one authoritative stable
channel; `channels/canary -> stable` is a permanent compatibility alias.
Immediately after this migration, both URLs served the existing signed
sequence 28 / version 2.9.10 byte-for-byte. This deployment did not publish a
new core version. Loopback and public-IP GET/HEAD/Range, write rejection,
application read endpoints and the running binary hash were verified.
An independent client also verified the Ed25519 signatures and all six core
files from both public URLs. Trust roots, validators, Caddy/configuration,
immutable releases and the application catalog were unchanged.

The production Caddy routes `/c1/core/v1/*` to the Go `c1repo` service on
`127.0.0.1:8091`; core files are not served by a separate Caddy file-server rule.
The service reads `/srv/c1core` and has no core signing key. It strictly accepts
only the manifest, detached signature and four component paths, with existing
GET/HEAD, Range, download queue, bandwidth and path-containment controls.

当前单通道状态：

- `channels/stable` is the sole authoritative release pointer.
- `channels/canary` is a fixed relative symlink to `stable`, never a second
  release pointer. It remains a dangling alias while stable is paused.
- The Go service also maps every accepted `canary/` request to `stable/` before
  opening a file. There is no redirect, cached duplicate or retired-version
  fallback, even if an old canary path remains on disk.
- An atomic change to stable therefore affects both URL spellings on every
  future publication. Separate requests can still straddle a publication;
  device signature/hash checks continue to reject mixed release contents.
- Caddy routing, both core trust-key representations, the installed release
  validator and `c1verify` signature verifier remain unchanged.

Repository releases, channels, disabled markers and trust remain root-controlled;
only staging may be publisher-writable. Install the reviewed helper from the
local-only `C1ancher-server/scripts/activate-core-release.sh` at
`/usr/local/libexec/c1core/activate-core-release`, root-owned mode 0755. The helper
requires the migration alias, fixed trust paths, root-owned non-writable trust
boundaries, and the existing `.publish.lock` for every operation. It fails closed
on an unmigrated independent canary pointer.

The private deployment/rollback runbook is
`C1ancher-server/docs/stable-channel-maintenance.md`. It describes a pinned SSH
read-only inspection, local Linux binary build, explicit approved baseline,
exclusive staging, lock-protected activation and safe binary rollback. Server
source and publisher tooling remain local-only; deploy only the required server
binary and root activation helper, never a public source attachment.

## Publish, compatibility promote, pause, and disable

Prepare exactly the six signed release files in the canonical staging path
`/srv/c1core/staging/<release-id>-<32-lowercase-hex>.upload`. Invoke the installed
helper with the legacy syntax `publish <stage> <release-id>`. Publication now
atomically activates **stable directly**, which also serves all legacy canary
URLs. It never writes an independent canary version. No new signing or rebuilding
is involved in the channel migration itself.

Existing command forms are handled as follows:

- `promote <release-id> stable` succeeds without changing anything only when
  that exact, verified, enabled release is already active on stable. A retired
  canary, different release, paused channel or other destination is rejected.
- `pause stable` removes only the stable pointer. Both URL spellings become
  unavailable; the `canary -> stable` alias is retained.
- `pause canary` is a compatibility spelling of `pause stable` and explicitly
  warns that it pauses both URLs. It cannot secretly leave stable serving.
- `disable <release-id>` durably creates the immutable disabled marker and
  withdraws stable if it selects that release. Both URL spellings are then
  unavailable. Disabling an unrelated historical release leaves stable intact.

The helper preserves full signature, artifact hash/size/ABI checks, protected
intake copying, immutable byte comparison on retries, file/directory sync,
atomic rename and the exclusive publication lock. Sequence and security-epoch
floors are checked against **all retained verified releases**, including when a
channel is paused or disabled; same-sequence identity conflicts and disabled
sequences are rejected before importing a conflicting release. Keep this
history: deleting high-water releases would remove this additional server-side
floor and requires a separate retention design. Device-side anti-rollback is
unchanged.

Resume a paused channel through a validated `publish` of the latest eligible
signed release, not through `promote`. Disabled releases cannot be resumed.
Retire historical one-off scripts that derive and execute a modified copy of the
old helper: the installed single-channel helper is the only supported publication
entry point. Such old derivation scripts should fail their source checks, not be
adapted to recreate an independent canary.

## Enroll devices

Build an enrollment bundle with `scripts/build-core-enrollment.ps1`. Supply the signed core release, the same external core private key, and the exact approved SHA-256 values for `/etc/app_daemon` baselines.

### ADB or on-site enrollment

Use `scripts/install-core-enrollment.ps1 -Action Install -BundleDirectory <bundle> -Reboot`.

The installer:

1. requires exactly one root ADB device and a read-only root mount;
2. validates the bundle locally before executing any bundle code;
3. pushes every file separately and verifies device-side SHA-256;
4. revalidates the signed bootstrap and core manifests on-device;
5. retains the original startup script and legacy compatibility binaries in the historical recovery directories (not a backup of factory learning software);
6. installs root-owned updater A/B slots, the fixed raw public key and independent recovery verifier;
7. durably saves a temporary root-protected, hash-bound copy of the approved startup script, then atomically installs the real bootstrap and its capability record;
8. restores the root mount to read-only, then runs the unchanged `prepare-local` compatibility and signature checks;
9. uses `bootstrap-activate` to establish and confirm the first generation, verifies its full contents, and only then changes compatibility links;
10. durably commits the enrollment marker, removes the temporary startup authorization and script, and restores the root mount to read-only before reporting success.

The temporary startup path is authorized only for incomplete first enrollment;
a completed enrollment never returns to vendor startup. It does not copy
`/usr/bin/d261` or delete historical backups. A pending first activation can be
resumed by the authenticated enrollment flow; ordinary GUI preflight continues
to reject incomplete non-factory enrollment pending trusted recovery. Physical
power-loss and cold-boot behavior still require device acceptance.

Use `-Action Verify` for a non-destructive check. Use `-Action Uninstall` to restore the original daemon and compatibility binaries. Signed generations are retained for forensic recovery.

`install-default-app.ps1` accepts `-CoreEnrollmentBundle` to chain default-app installation and enrollment before one optional reboot.

### Application-repository enrollment

An enrollment bundle is also a complete application payload. Add it to an application catalog as:

- ID: `c1-core-bootstrap`
- version: `1.0.0`
- entry: `enroll.sh`
- payload directory: the immutable enrollment bundle

Build and sign with the existing application repository builder. The application signature authorizes delivery; the embedded core signature authorizes the exact bootstrap, updater, key, baseline set, and initial core release. Remove the package from the catalog after the enrolled coverage target is reached.

Unknown root-daemon hashes must fail closed and enter the manual recovery queue.

## Fault matrix

Run host tests before every release. They cover strict parsing, signature/key failures, links, path rejection, size overflow, rollback, same-sequence conflicts, transaction retry, first-generation activation, updater slot self-test, crash policy, request isolation, and interrupted state generations.

Run these destructive tests only on a backed-up development device with physical recovery access:

| Fault | Injection point | Required result |
|---|---|---|
| Network interruption | each manifest/signature/artifact download | current confirmed generation remains bootable; retry is bounded |
| Bad signature/key | before artifact download | no prepared generation and no pointer change |
| Artifact size/hash mismatch | staging verification | staging is rejected and current remains unchanged |
| `/storage` unavailable | before and during download | update fails without touching `/usr/data` pointers |
| `/usr/data` full | candidate copy | no partial committed generation; current remains unchanged |
| Read-only root | updater slot/bootstrap enrollment | enrollment aborts and restores root read-only |
| Power loss | after each file write, fsync, directory fsync, generation rename, state link rename, and core pointer rename | reboot selects either the old confirmed generation or the complete new generation, never mixed components |
| Candidate launcher crash | before ready and before confirmation | supervisor restores previous and records failed identity |
| Repeated C1ancher crash | five short runs | crash storm is detected and rollback/retained supervision follows policy |
| Corrupt inactive updater slot | before slot selection | current slot continues; failed self-test cannot replace a slot |
| Fatal active updater | after alternate slot is installed | root bootstrap switches once to the other valid slot |
| Both updater slots invalid | completed enrollment | fixed verifier tries signed previous/current generations; no vendor fallback |
| Confirmation timeout | candidate stays alive without valid health marker | candidate is not confirmed and previous is restored |
| Concurrent publish/promote | hold publication lock | exactly one operation succeeds; the other fails closed |
| Disabled release | publish/promote after disable | operation is rejected and no channel points to it |
| Repeated enrollment | after confirmed generation but before marker, and after marker | operation is idempotent and backups are unchanged |
| Enrollment uninstall | after a cold boot | original daemon/process chain is restored; auto-suspend marker is unchanged |

For every device test, capture only release ID, manifest digest, phase, monotonic timestamps, process counts, pointer targets, mount state, hashes, and result codes. Do not capture URLs containing parameters, keys, passwords, or the server address file.

## Stable-only rollout

1. Validate the signed release offline and on a physically recoverable development device; reboot twice and confirm health before public activation.
2. Use explicit device enrollment/update batches for observation. The old canary URL is a stable alias, not a hidden test cohort.
3. Publish the approved signed release directly to stable through the installed helper.
4. Confirm the stable and canary URLs return the exact same manifest, signature and all four artifacts; check health-confirmation time, boot count, rollback count and updater-slot status.
5. Pause stable immediately if confirmation latency or rollback rate exceeds the release threshold. The old `pause canary` spelling has the same global effect.
6. Resume only with an eligible validated publication after investigation; never rebuild or resign merely to move a channel.
7. Disable a release on evidence of a security or boot-safety defect. Retain its immutable bytes and marker for audit and anti-rollback checks.

## Disaster recovery

1. Stop automatic rollout by pausing stable; this also stops every legacy canary URL.
2. Preserve device state, core generation directories, state generations, enrollment log, and hashes before changing anything.
3. If the candidate is pending, run the known-good updater `rollback` command with the fixed state/core/key paths.
4. If the active updater is fatal, select the other verified slot in `/usr/data/c1/update/updater-slot` using a same-directory temporary file and rename.
5. If both slots fail, restore `/etc/app_daemon.c1-original` from the root copy or either writable-filesystem recovery copy, verifying SHA-256 before same-directory replacement.
6. Remount root read-only and verify the expected PID chain after restart.
7. Never delete current, previous, pending, disabled-for-investigation, or sole factory-recovery bytes.

Historical release cleanup is intentionally manual. Before deleting any server or device generation, resolve every channel/current/previous/pending pointer and prove that at least one confirmed generation plus the factory recovery path remains valid.