# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A webOS TV homebrew app (fork of webosbrew/custom-screensaver) that replaces the stock LG webOS screensaver with Apple TV aerial videos streamed from Apple's servers. Requires a rooted TV with the Homebrew Channel. Targets webOS 5–23.

## Commands

```sh
npm ci                        # install deps (lib/enyo is a git submodule — clone with --recurse-submodules)
npm run build                 # enyo pack → dist/ (CI uses: npm run build -- --production)
npm run package               # ares-package dist/ → .ipk (excludes enyo-ilib)
npm run deploy                # ares-install the .ipk onto a connected TV
npm run launch                # ares-launch the app on the TV
npm run clean                 # rm -rf dist/
npm run manifest              # generate Homebrew Channel manifest (needs the .ipk to exist first)
```

There are no tests or linters. CI (`.github/workflows/main.yml`) builds and packages on every push/PR to main; tagging `v*.*` triggers `release.yml`, which builds, packages, generates the manifest, and creates a GitHub release.

Versioning: `npm version <bump>` runs `tools/sync-version.js` to copy the new version into `appinfo.json` — never edit the version in `appinfo.json` by hand.

## Architecture

Two nearly independent halves that communicate only through JSON files in `assets/`:

**1. Settings app (`frontend/`)** — an Enyo 2.7 / Moonstone TV app (legacy framework: `kind()` declarations, CommonJS requires). `index.js` → `App.js` → `views/MainView.js` → `views/MainPanel.js`, which contains all UI and logic. It has no direct filesystem access: every privileged operation (reading/writing `settings.json`, symlinking autostart, test-launching the screensaver) is done by sending shell command strings to the Homebrew Channel root-exec service `luna://org.webosbrew.hbchannel.service/exec` via `enyo-webos/LunaService`.

**2. Screensaver (`assets/screensaver-main.qml`)** — a self-contained QtQuick QML file that is **not built or bundled by enyo**; it ships verbatim as an asset. `assets/apply.sh` activates it by `mount --bind`-ing it over the stock screensaver at `/usr/palm/applications/com.webos.app.screensaver/qml/main.qml` (lasts until reboot; "autostart" symlinks apply.sh into `/var/lib/webosbrew/init.d/`). At runtime it XHR-loads `settings.json`, `videos.json`, and a locale file from the installed app's assets directory, then plays random videos with OSD (video name, point-of-interest text, clock/date via iLib), quality fallback (Dolby Vision → SDR → H264), and stall/error recovery driven by a 1-second timer.

**Shared data contracts:**
- `assets/settings.json` — written by the app (via shell echo), read by the QML. Keys like `sourceType`/`sourceTypeIndex` and `localeLang`/`localeLangIndex` are stored redundantly (value for QML, index for the Enyo picker); both must stay in sync, and picker option order in `MainPanel.js` must match the stored indexes.
- `assets/videos.json` — Apple aerial playlist; each asset has per-quality URLs (`url-4K-HDR`, `url-1080-SDR`, etc.) and `pointsOfInterest` keyed by timecode in seconds.
- `assets/locales/*.json` — 40+ locale files mapping `localizedNameKey`/POI keys to display strings. Adding a language means adding the JSON file *and* an entry in the language picker in `MainPanel.js` (order matters).

The app id `org.aabytt.webos.custom-screensaver-aerial` is hardcoded as an absolute install path (`/media/developer/apps/usr/palm/applications/...`) in both `MainPanel.js` and `screensaver-main.qml` — changing the id means updating both.
