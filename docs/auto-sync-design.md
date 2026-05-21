# Auto Sync Design Sketch

## Context

The current sleep path is real deep sleep / power cutoff. `HalPowerManager::startDeepSleep()` notes the MCU is completely powered off on battery, including RTC context. That means an activity cannot wake itself every N minutes from sleep, fetch, then return to sleep unless the hardware/firmware gets a new timed wake mechanism.

For now, auto-sync can run while the firmware is awake, or in an optional soft-sleep mode that keeps the main loop alive.
Deep sleep remains the default and cannot perform scheduled sync.

## Upstream Prior Art

There have already been several attempts around automated online updates or sync. The important pattern is that maintainers are cautious about hidden background network work, WiFi cost, heap/memory risk, and concurrent UI/network operations.

Relevant PRs/issues:

- [PR #408: automated online sleep screen updates](https://github.com/crosspoint-reader/crosspoint-reader/pull/408)
  - Closed, unmerged.
  - Added a Calendar Mode that wakes on a timer, connects WiFi, downloads an image from a server, updates the sleep screen, then sleeps again.
  - This is the closest prior attempt to generic scheduled remote fetching.
  - Maintainer feedback said automated online updates do not fit the current `SCOPE.md` guidelines and should go through a Discussion first.
  - Hardware findings in the PR are important:
    - Deep sleep timer wake works on USB power in tests, but not on battery.
    - Deep sleep GPIO wake on battery behaves like a full power-on reset.
    - RTC data is wiped on battery wake, confirming full power loss/reset behavior.
    - Light sleep timer wake was reported as not working in those tests.
  - Result: true "deep sleep, timed wake, fetch, resume sleep" is not a viable battery feature today.

- [PR #1571: KOReader auto-sync background progress tracking](https://github.com/crosspoint-reader/crosspoint-reader/pull/1571)
  - Closed, unmerged.
  - Tried automatic KOReader progress sync after page-turn thresholds and timer intervals.
  - Included status bar indicators and a local sync state file.
  - Closed because running sync automatically in the background can be expensive and risky, especially with WiFi and UI/runtime interaction.
  - Maintainer feedback preferred a more explicit user-demand approach.

- [PR #2026: auto KOSync on book open/close](https://github.com/crosspoint-reader/crosspoint-reader/pull/2026)
  - Closed, unmerged.
  - Tried automatic KOReader sync on book open/close with cooldowns, battery checks, WiFi timeout, and a debug log.
  - Closed in favor of a design that does not run sync concurrently with other UI operations.
  - Relevant lesson: even bounded automatic sync was still considered too implicit and risky.

- [PR #1090: Push Progress & Sleep](https://github.com/crosspoint-reader/crosspoint-reader/pull/1090) and [Issue #914: Dedicated options to push/pull sync](https://github.com/crosspoint-reader/crosspoint-reader/issues/914)
  - Still the preferred direction for KOReader-style sync.
  - Adds an explicit reader-menu action to push progress and then sleep.
  - The useful lesson for auto-sync is the shape: user-controlled, foreground-safe, and not concurrent with unrelated UI work.

Design consequences for this fork:

- Treat v1 as an experimental fork feature, not upstream-ready.
- Keep it disabled by default.
- Prefer manual sync and explicit soft-sleep scheduling over hidden always-running background sync.
- Do not claim support for scheduled wake from deep sleep.
- Avoid automatic fetches during normal foreground activity.
- If this is ever proposed upstream, open a Discussion before a PR.

## Proposed Shape

Add this as two pieces, not only as an Activity.

### `AutoSyncService`

- Long-lived background coordinator called from `main loop()`.
- Loads jobs from a hardcoded SD file, for example `/.crosspoint/auto-sync.json`.
- Tracks runtime state: idle, waiting for network, connecting, downloading, success/error.
- Owns the “only one auto-sync at a time” rule.
- Exposes status for UI and status-bar/header overlay.
- Persists job status to SD, for example `/.crosspoint/auto-sync-state.json`.
- Logs append-only text to `/.crosspoint/auto-sync.log`.

### `AutoSyncActivity`

- Normal UI activity for inspecting and manually triggering jobs.
- Shows job list: destination path, interval, last attempt, last result.
- Actions:
  - Fetch selected job now.
  - Fetch all now.
  - Reload job config.
  - Open/view log.
- It does not own the scheduler; it talks to `AutoSyncService`.

### Auto-Sync Settings

Auto-sync should have its own settings section in the settings activity.

V1 settings:

- Enable/disable scheduled auto-sync.
- Enable/disable soft sleep for scheduled sync, default off, visible only when auto-sync is enabled.
- Configure log rotation cap.
- Other scheduler/runtime tuning that is not an immediate manual action.

The activity UI is for job status, manual triggers, config reload, and log viewing. Long-lived behavior belongs in settings.

By default, the feature is disabled and the Auto Sync UI is reachable only from Settings. When the user enables auto-sync,
Home should show an Auto Sync entry that opens the job/status/manual-trigger/log UI.

## Job File Format

Prefer JSON over CSV-like tuples because the repo already uses ArduinoJson and this will need validation.

Example:

```json
{
  "version": 1,
  "jobs": [
    {
      "url": "https://example.com/feed/book.epub",
      "path": "/books/book.epub",
      "intervalMinutes": 360
    }
  ]
}
```

Later-compatible fields could be `enabled`, `name`, `etag`, `ifModifiedSince`, `maxBytes`.

V1 does not support auth for data sources.

## Scheduling Semantics

Given current power behavior:

- While the device is awake in normal foreground use, `AutoSyncService` may update due/overdue state but should not automatically start downloads.
- Manual actions from `AutoSyncActivity` can fetch one job or all jobs.
- Scheduled fetches run only while the device is in firmware soft sleep / idle sleep.
- If the user manually wakes the device and jobs are overdue, show status in the Auto Sync UI or Home entry rather than silently starting network work.
- If auto-sleep is about to happen and jobs are due, v1 should still enter soft sleep first and let the soft-sleep scheduler decide. Avoid a pre-deep-sleep network window unless later testing proves it is safe and useful.
- If the device is in deep sleep, it cannot wake itself on interval with the current sleep implementation.
- If soft sleep is enabled, the device shows a sleep-like screen but keeps the MCU and main loop alive at low activity. Scheduled sync can run from this state, then return to the soft-sleep screen.
- Entering deep sleep immediately stops any ongoing sync. Auto-sync does not postpone deep sleep.

Deep sleep vs soft sleep must be explicit in UX/docs. Otherwise users will expect a cron-like sleeper with deep-sleep battery life, which the current hardware/firmware cannot provide.

Use the term "firmware soft sleep" or "idle sleep" for this feature. Do not describe it as ESP light sleep unless the implementation actually uses and validates ESP light sleep, because prior tests in PR #408 reported light sleep timer wake problems.

Soft sleep behavior:

- Default off.
- Only available when auto-sync is enabled.
- WiFi remains off except during sync windows.
- Main loop runs with longer delays and low CPU frequency where possible.
- Foreground activity is effectively stalled behind a sleep-looking screen until user wake/input.
- Battery drain is higher than deep sleep and should be described as a deliberate tradeoff.
- If soft sleep is enabled, normal power-button sleep should enter soft sleep instead of deep sleep so scheduled sync can work.
- Later refinement: reserve a longer/alternate power-button gesture to force true deep sleep even when soft sleep is the default sleep mode.

## Network Ownership

Do not let each feature poke raw `WiFi` independently. Right now several activities directly do `WiFi.mode`, `WifiSelectionActivity`, `HttpDownloader`, then `silentRestart()` on exit because WiFi/TLS fragments heap.

For this feature, introduce a small shared network coordinator before adding auto-sync:

`NetworkSession` / `NetworkManager`

- Provides “network busy” state.
- Allows foreground network activities to claim exclusive ownership.
- Allows background auto-sync to claim only when no foreground network owner exists.
- Connects using existing `WifiCredentialStore`, preferably last connected SSID first.
- Uses existing `HttpDownloader`.
- Tears WiFi down after background sync.
- Does not silent-restart after every background fetch unless memory testing proves it is required.

Without this, auto-sync will race OPDS, OTA, WebServer, font downloads, KOReader sync, etc.

## Respecting Current UI State

Rules to implement:

- If WebServer/OPDS/OTA/font download/KOReader sync is active: auto-sync waits.
- If any user input happened recently: auto-sync waits.
- If a foreground activity is doing blocking work: auto-sync waits.
- If a scheduled soft-sleep sync is running and the user wakes the device or opens a network feature: foreground wins, auto-sync cancels or defers.
- Auto-sync never calls `goHome`, never replaces current activity, never calls deep sleep by itself unless it was invoked as part of a pre-sleep hook.
- V1 should not run scheduled downloads during unrelated foreground activity. This avoids the concurrency concerns that closed earlier auto-sync PRs.

## Status Overlay

The current status bar is reader-focused, not truly system-wide. Avoid forcing a refresh icon onto every screen in v1 because e-ink refreshes and framebuffer ownership are sensitive.

Better v1:

- Add a small global “background sync active” flag.
- Draw a sync glyph in places that already draw header/status UI:
  - reader status bar
  - UI headers via theme header drawing
  - AutoSyncActivity itself
- Use a simple static icon, not animation. E-ink animation is not worth it.

Later:

- Generalize into a `SystemOverlayState` consumed by themes.

## Download Safety

For each job:

- Validate URL is `http://` or `https://`.
- Validate destination is absolute SD path and reject paths under `/.crosspoint/auto-sync*` except logs/state/config.
- Download to temporary path first, for example `/books/book.epub.part`.
- Only replace final path when the remote file changed and the download completed successfully.
- Remove `.part` on failure/cancel.
- Record bytes downloaded, HTTP error, file error, timestamp.
- Use HTTP validators where available: ETag and/or Last-Modified. If the server does not provide validators, compare downloaded temp file to the existing file before replacing, if feasible.

## Log Viewer

Simplest good UX:

- `AutoSyncActivity` has “View log”.
- Implement a read-only text viewer activity that tails the last N KB/lines of `/.crosspoint/auto-sync.log`.
- Controls: up/down page, back.
- Do not load the whole log into RAM.
- Rotate/truncate log at a small cap, for example 64 KB or 128 KB.

## Open Questions

Resolved:

1. JSON is acceptable for the SD job file.
2. V1 supports anonymous HTTP(S) sources only.
3. Downloads should overwrite only when changed.
4. Auto-sync gets its own UI for status/manual triggers/log view.
5. Auto-sync gets its own settings section for scheduled behavior, soft sleep, log rotation, and related adjustments.
6. Deep sleep kills ongoing sync immediately; auto-sync does not postpone deep sleep.
7. By default, the Auto Sync UI lives only in Settings.
8. When auto-sync is enabled, Home shows an Auto Sync entry.
9. If soft sleep is enabled, normal sleep enters soft sleep; force deep sleep can be a later gesture/refinement.

## Recommended V1

Use a JSON job file, anonymous HTTP(S), overwrite only when changed, manual “fetch all/one”, persistent status/log,
Settings-only entry while disabled, Home entry after enablement, optional firmware soft sleep for scheduled sync,
and no timed wake from deep sleep.

The service can exist while awake, but in v1 it should coordinate state and expose status rather than launching hidden network work during normal foreground use. Scheduled network work belongs to soft sleep only.

That gives a working foundation without fighting the device’s power model.

## Implementation Plan

Implement this in small commits.

### Commit 1: Shared Network Stack

Goal: make new network use go through one common path before Auto Sync exists.

Scope:

- Add a small `NetworkManager` / `NetworkSession` abstraction.
- Use existing `WifiCredentialStore`.
- Connect to known networks, preferably last-connected SSID first.
- Track ownership/busy state so only one network operation owns WiFi.
- Let foreground/manual callers claim network explicitly.
- Let background/scheduled callers fail or defer when network is busy.
- Keep existing `HttpDownloader`; do not reinvent HTTP.
- Do not migrate every existing network feature yet. First commit can add the shared system and wire enough of it for Auto Sync.

Important v1 behavior:

- Manual Auto Sync should not run concurrently with any existing network activity.
- If the network is busy, the Auto Sync UI should show "network busy" and let the user retry.
- This keeps the first implementation debuggable and matches the lessons from prior rejected auto-sync PRs.

### Commit 2: Minimal Manual Auto Sync

Goal: prove the useful path: read jobs, show jobs, fetch manually.

Scope:

- Add `AutoSyncActivity`.
- Use the hardcoded jobs file `/.crosspoint/auto-sync.json`.
- Parse JSON jobs:
  - `url`
  - `path`
  - `intervalMinutes`, parsed/displayed but not scheduled yet
- UI lists jobs from the file.
- Actions:
  - Fetch selected job now.
  - Fetch all jobs now.
  - Reload jobs file.
- Use the shared network stack from commit 1.
- Use existing `HttpDownloader`.
- Download to a temporary path, then replace destination on success.
- Append simple log lines to `/.crosspoint/auto-sync.log`.

Explicitly out of scope for this commit:

- Settings screen.
- Sleep behavior.
- Scheduler.
- Status overlay.
- Built-in log viewer.
- Log deletion button.

The log can be inspected as a normal SD-card file for now. A later UI should include a button to delete/drop Auto Sync logs.

### Later Commits

- Settings section:
  - Enable/disable feature.
  - Soft sleep toggle.
  - Log rotation length.
- Home entry only when enabled.
- Firmware soft-sleep scheduler.
- Status/header sync glyph.
- Better status persistence.
- Log viewer.
- Button to delete/drop all Auto Sync logs.
- HTTP validators: ETag / Last-Modified / unchanged detection.
