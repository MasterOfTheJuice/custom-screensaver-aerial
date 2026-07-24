# CONVENTIONS.md

Working conventions for agents contributing to this repository. Read `CLAUDE.md` first for architecture; read `SPEC.md` for the webOS 4.4 effort.

## Repository ground rules

- **Two halves, one contract.** The Enyo settings app (`frontend/`) and the QML screensaver (`assets/screensaver-main.qml`) share only the JSON files in `assets/`. Never introduce another coupling channel.
- `assets/*.qml` and everything else under `assets/` ships **verbatim** in the .ipk — no build step touches it. Don't add imports or QML language features newer than what webOS 5-era Qt (QtQuick 2.4 / QtMultimedia 5.6) supports.
- The app id `org.aabytt.webos.custom-screensaver-aerial` is hardcoded as an absolute path in `frontend/views/MainPanel.js` (`basePath`) and `assets/screensaver-main.qml` (`basePath`). Any new file that needs the install path must reuse those existing constants, and an id change must update both.
- Never hand-edit `version` in `appinfo.json`; only `npm version <bump>` (runs `tools/sync-version.js`).
- Don't commit `dist/`, `*.ipk`, or `.enyocache`. Don't touch `lib/enyo` (git submodule) or `package-lock.json` unless the task is explicitly about dependencies.

## Code style

### QML (`assets/*.qml`)
- 4-space indent; ` : ` spacing in property bindings matches the existing file — keep it consistent within a file.
- One self-contained file per screensaver variant; no `qmldir`, no separate JS files (nothing else gets deployed to the mount-bind target).
- State lives in root-level `property var` declarations on the `WebOSWindow`; imperative logic in plain `function`s at the bottom half of the file.
- Polling/recovery logic belongs in the existing 1-second `Timer` (`refreshOSD`) — extend `checkError`/`checkStatus` rather than adding new timers.
- Luna calls from QML use the existing `Service { }` pattern (`WebOSServices 1.0`).
- New user-facing behavior must respect existing settings keys (`osdOpacity`, `debug`, `playLowerQuality`, `sourceType`, `localeLang`); read them from the loaded `settings` object, never hardcode.

### Enyo/JavaScript (`frontend/`)
- Legacy Enyo 2.7 idiom: `kind()` declarations, CommonJS `require`, `this.$.componentName` access, `onchange`/`ontap` string handlers. Match it; no ES6+, no arrow functions, no `let`/`const` (target engines are old TV Chromium builds).
- All privileged operations go through the existing `exec()` helper (shell string to `luna://org.webosbrew.hbchannel.service/exec`). Keep shell strings simple and single-quoted-JSON-safe — `settingsSave()` embeds `JSON.stringify` output inside single quotes; don't introduce values that can contain `'`.
- UI additions go in `views/MainPanel.js` alongside existing controls. Result/feedback text goes to the shared `result` BodyText via existing handlers.

### Settings contract (critical, easy to break)
- `settings.json` stores value+index pairs (`sourceType`/`sourceTypeIndex`, `localeLang`/`localeLangIndex`). Both must be written together, and picker option **order** in `MainPanel.js` must match the indexes. Never reorder existing picker entries; append only.
- Adding a locale = new `assets/locales/<lang>.json` **plus** a picker entry in `MainPanel.js` in the matching position.
- New settings keys: add a sane default so a stale `settings.json` from a previous version doesn't break the QML (guard reads: `settings.newKey === undefined` → default).

### Shell (`assets/*.sh`)
- POSIX-ish `sh` with `set -e -o pipefail`, as in `apply.sh`. These run as **root at TV boot** via `/var/lib/webosbrew/init.d/` — fail loudly and early (`[-]`/`[+]`/`[~]` prefixed messages to stderr), never leave the system half-configured, and check preconditions (target file exists, not already mounted) before acting.

## webOS-4.4-specific conventions (SPEC.md work)

- **webOS 5+ stays byte-identical.** Preferred: separate `assets/screensaver-webos4.qml`, selected by version check in `apply.sh`. Only fold changes into `screensaver-main.qml` if the diff is a couple of property values.
- One experiment = one minimal diff. Record every experiment (pass or fail) in `RESEARCH-webos4.md` with firmware version, exact change, and observed behavior — negative results are a deliverable (upstream issue #30 explicitly asks for them).
- webOS 4 defaults: `sourceType` of `url-1080-SDR` or `url-4K-SDR`, `playLowerQuality: true`; do not offer Dolby Vision expectations on webOS 4 paths.
- Anything that changes screensaver lifecycle (window type, launcher apps) must prove keypress-dismissal still works before being considered a pass.

## Verification (no tests, no linters — on-device is the only truth)

Build/deploy loop:

```sh
npm run build && npm run package && npm run deploy   # to a connected TV (ares device setup assumed)
npm run launch                                        # opens the settings app
```

Screensaver iteration loop (faster than reinstalling, once the app is installed):

1. Copy the QML to the TV (redeploy, or `ares-shell`/ssh copy into the installed app's `assets/`).
2. If already mount-bound: `umount /usr/palm/applications/com.webos.app.screensaver/qml/main.qml`, then re-run `apply.sh`.
3. Trigger: `luna-send -n 1 'luna://com.webos.service.tvpower/power/turnOnScreenSaver' '{}'` (same command as the app's "Test run" button).
4. Set `"debug": true` in `settings.json` to get the on-screen diagnostic overlay (media status, playback state, stall countdown, error strings).

Before declaring any change done: build succeeds (`npm run build -- --production`, what CI runs), package succeeds, and the relevant on-device checks from SPEC.md acceptance criteria pass or are explicitly listed as not-run (no TV available).

## Git

- Commit messages: short imperative subject, matching existing history (`Update videos.json`, `Merge pull request …`). Body only when the change needs explanation (webOS 4 experiments: cite the experiment number/result from `RESEARCH-webos4.md`).
- Branch per feature (`webos4-compat`-style naming). PRs target `main`; CI must pass (build + package on Node 14).
- Never commit TV-specific artifacts (dumped firmware files, device logs) — summarize them in `RESEARCH-webos4.md` instead.
