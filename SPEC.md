# SPEC: webOS 4.4 support for custom-screensaver-aerial

## Objective

Make the Aerial screensaver work on a **rooted LG TV running webOS 4.4 with the Homebrew Channel already installed**, without regressing webOS 5–23 support.

## Feasibility summary (research verdict)

**Feasible, with one hard blocker to solve experimentally.** Everything in the app's delivery chain is already proven to work on webOS 4.x by field reports in the upstream issue tracker:

| Component | Status on webOS 4.x | Evidence |
|---|---|---|
| Homebrew Channel `exec` luna service | works (given root, per prerequisite) | settings app functions in reports |
| `apply.sh` mount-bind over stock screensaver QML | **works** — custom QML is launched by the system | [#29](https://github.com/aabytt/custom-screensaver-aerial/issues/29), [#34](https://github.com/aabytt/custom-screensaver-aerial/issues/34) |
| QML runtime, all imports (`QtMultimedia 5.6`, `Eos.Items`, `WebOSServices`, `iLib`) | **loads without error** (OSD text renders, debug overlay runs) | [#29](https://github.com/aabytt/custom-screensaver-aerial/issues/29), [#34](https://github.com/aabytt/custom-screensaver-aerial/issues/34) |
| Screensaver trigger/dismiss lifecycle (`com.webos.service.tvpower`) | works — stock screensaver activates normally | [#29](https://github.com/aabytt/custom-screensaver-aerial/issues/29) |
| Media pipeline (video decode + playback) | works — debug OSD reports playback running/timecode advancing | [#34](https://github.com/aabytt/custom-screensaver-aerial/issues/34) |
| **Video visible on screen** | **BROKEN** — black screen with OSD on top | [#17](https://github.com/aabytt/custom-screensaver-aerial/issues/17), [#29](https://github.com/aabytt/custom-screensaver-aerial/issues/29), [#30](https://github.com/aabytt/custom-screensaver-aerial/issues/30), [#34](https://github.com/aabytt/custom-screensaver-aerial/issues/34) |

Maintainer's diagnosis ([#30](https://github.com/aabytt/custom-screensaver-aerial/issues/30)): on webOS, hardware-decoded video is rendered on a **video plane below the graphics plane**; QML apps reveal it with the `PunchThrough` element (`Eos.Items`). On webOS 4 the punch-through is not honored for the screensaver window, so the video plays invisibly "at the bottom layer". The maintainer's suggested direction: try a different window/app type. No fork or branch has solved this yet (checked all forks; `alexastall/webos4-compat` branch contains zero commits ahead of main).

Mechanism detail (from [webosose/qml-webos-framework](https://github.com/webosose/qml-webos-framework) `src/Eos/Items/src/punchthrough.cpp`): `PunchThrough` does two things — (1) renders a transparent rect with GL blending disabled (writes alpha=0 into the framebuffer), and (2) registers the rect with the compositor via a platform-native hook `setWindowPunchThroughRectFunc`. On webOS 4 TV firmware either the hook is absent or the compositor ignores punch-through for `_WEBOS_WINDOW_TYPE_SCREENSAVER` surfaces. This is a compositor/window-type policy problem, **not** a hardware or codec limitation — which is why a workaround is credible.

## Constraints

- Target device: rooted webOS 4.4 TV, Homebrew Channel installed. Root method out of scope (prerequisite).
- webOS 5–23 behavior must not change. Any webOS 4 path must be version-gated.
- The stock screensaver replacement mechanism (mount-bind of `assets/screensaver-main.qml`, autostart via `/var/lib/webosbrew/init.d/`) stays; it is proven working on webOS 4.
- Dolby Vision source types are irrelevant on most webOS 4.4 panels; default webOS 4 settings to `url-1080-SDR` or `url-4K-SDR` with `playLowerQuality: true`.
- No CI-testable outcome exists: **all verification is manual, on-device**. Each phase below defines its own on-device pass/fail check.

## Requirements

- **R1** Video visibly plays fullscreen during screensaver on webOS 4.4.
- **R2** System lifecycle preserved: screensaver starts on idle, dismisses within ~1s of any remote keypress, and foreground app resumes intact.
- **R3** OSD (name, POI, clock/date, opacity, locale) works as on webOS 5+.
- **R4** Stall/error recovery (existing 1-second timer logic) still cycles videos; never leaves a static frame or static OSD on an OLED panel for a long period.
- **R5** webOS 5–23 users see zero behavior change.
- **R6** Settings app requires no webOS-4-specific UI beyond (optionally) exposing anything the chosen fix needs.

## Plan — experiment ladder

Work is phased; each phase is cheap to test because iteration is only: edit QML → redeploy (`npm run deploy` or copy file over ssh) → re-run `apply.sh` (umount first if already bound) → trigger with `luna-send -n 1 'luna://com.webos.service.tvpower/power/turnOnScreenSaver' '{}'`. Stop at the first phase that passes R1+R2.

### Phase 0 — on-device recon (do first, informs everything)

On the webOS 4.4 device (via Homebrew Channel exec or ssh):

1. Dump the stock screensaver app: `/usr/palm/applications/com.webos.app.screensaver/` (its `main.qml` shows which imports/window type/luna calls LG itself uses on this firmware — the ground truth for what works here).
2. Inventory QML modules: `find / -name "libeos*" -o -path "*qml/Eos*" 2>/dev/null`; check whether `Eos/Items/qmldir` exports `PunchThrough` and what plugin version ships.
3. Check compositor: `ls /usr/lib/qt5/plugins/platforms/`, note the wayland/eglfs platform plugin; grep binaries for `setWindowPunchThroughRectFunc`.
4. List available window types and luna services: `ls-monitor` / `luna-send -n 1 luna://com.webos.service.bus/signal/registerServerStatus`; confirm `com.webos.media` (uMediaServer) API surface (`luna-send -n 1 luna://com.webos.media/getPipelineState '{}'` etc.).
5. Enable `debug: true` in settings and capture the debug OSD values (media status, playback state, buffer) plus `/var/log/`/`pmloglib` output from a screensaver run.

Deliverable: a `RESEARCH-webos4.md` capturing all of the above.

### Phase A — window/compositor experiments (cheapest, maintainer-endorsed)

Try in order, each as a minimal diff to `screensaver-main.qml`:

1. `windowType: "_WEBOS_WINDOW_TYPE_CARD"` (also try `_WEBOS_WINDOW_TYPE_OVERLAY`, `_WEBOS_WINDOW_TYPE_POPUP`, `_WEBOS_WINDOW_TYPE_NONE`, and whatever the Phase 0 stock QML uses).
2. `color: "transparent"` on the `WebOSWindow` (with and without `PunchThrough`).
3. `PunchThrough` geometry/z variants: explicit `setRegion`, full-screen rect at `z: -1` as a **sibling** of `Video` (pattern from [webosbrew/sample-media-qmlvideo](https://github.com/webosbrew/sample-media-qmlvideo)).
4. Height hack check: the current `height: parent.height - 1` exists to avoid auto-disable; verify it isn't what breaks webOS 4 fullscreen video association (try full height on webOS 4).

Pass check: video visible (R1). Then verify R2 — if a non-screensaver window type displays video but the system no longer dismisses it on keypress, add explicit dismissal: handle `Keys.onPressed`/webOS key events in QML and call `luna://com.webos.service.tvpower/power/turnOffScreenSaver` (verify exact method on-device in Phase 0) and/or `Qt.quit()`.

### Phase B — direct media-pipeline routing (if A fails)

Bypass Qt's video item entirely: keep the QML window for OSD only, and drive playback through luna:

1. `luna://com.webos.media/load` with the video URL and `com.webos.app.screensaver` appId, then `play`, `setDisplayWindow` (fullscreen dst rect) / `switchToFullscreen` — mirrors what native players do and controls the video plane explicitly.
2. If the media service on webOS 4 exposes z-order/plane controls (`com.webos.service.avoutput`, `videooutput`, `vsm` — discover in Phase 0), use them to raise the video plane or lower the screensaver surface.

Pass check: same as Phase A. The existing QML `Video`-based logic stays for webOS 5+; webOS 4 branch swaps `Video`/`PunchThrough` for a `Service`-driven pipeline while reusing playlist/OSD/recovery logic.

### Phase C — hybrid launcher (fallback, guaranteed-rendering path)

If no window/pipeline trick renders video inside the screensaver app:

1. Keep the mount-bound QML as a **thin trigger stub**: on `Component.onCompleted` it calls `luna://com.webos.applicationManager/launch` for a dedicated fullscreen player app and renders nothing itself.
2. The player is a plain **web app** (video via `<video>` element) — web apps demonstrably display streamed video on webOS 4 (every streaming app does). It reuses `videos.json`/`settings.json`/locales from the shared assets directory.
3. Lifecycle contract: stub QML forwards dismissal — when the system kills the screensaver (any keypress), stub's destruction handler (or the player subscribing to `com.webos.surfacemanager/getForegroundAppInfo`) closes the player app so the user lands back on their previous app.

Known risk: Apple's files are HEVC/H.264 in a `.mov` container; the web engine's pipeline may reject `.mov` even though the QML/gstreamer path accepts it. Mitigation: test `url-1080-H264` first; if containers are the blocker, this phase fails fast and the answer is documented.

### Version gating (whichever phase wins)

- Detect webOS version at runtime: read `/var/run/nyx/os_info.json` (shell, via apply.sh) or `luna://com.webos.service.tv.systemproperty/getSystemInfo` (QML `Service`).
- Preferred shape: `apply.sh` picks which QML to bind — `screensaver-main.qml` (webOS 5+, unchanged) vs `screensaver-webos4.qml` — keeping the webOS 5+ file byte-identical (R5). In-file branching is acceptable only if the diff is trivial (e.g. window type + color only).

## Acceptance criteria

1. On webOS 4.4: idle-triggered screensaver shows moving aerial video with OSD for ≥30 min across ≥5 different videos (exercises stall recovery and playlist rotation).
2. Any remote keypress returns to the prior app within ~1s; repeated 10×, no stuck black screen, no orphaned player app.
3. Autostart symlink + reboot: screensaver works with no manual steps.
4. On a webOS 5+ device (or by file diff if no device): `screensaver-main.qml` behavior unchanged.
5. Settings app loads, saves settings, and "Test run" works on webOS 4.4.
6. `RESEARCH-webos4.md` documents Phase 0 findings and which experiments passed/failed (valuable upstream even on failure — see issue #30's explicit request for this).

## Out of scope

- Rooting the TV / installing Homebrew Channel (stated prerequisite).
- webOS 3.x support (maintainer confirms different app structure needed; same boat as 3.4 — [#33](https://github.com/aabytt/custom-screensaver-aerial/issues/33)).
- webOS 9 (24+) Flutter port (tracked upstream in [#4](https://github.com/aabytt/custom-screensaver-aerial/issues/4)).
- Dolby Vision on webOS 4 panels that lack it (fallback logic already exists).

## Primary sources

- Fork issues: [#30 webOS 4 explainer](https://github.com/aabytt/custom-screensaver-aerial/issues/30), [#17](https://github.com/aabytt/custom-screensaver-aerial/issues/17), [#29](https://github.com/aabytt/custom-screensaver-aerial/issues/29), [#34](https://github.com/aabytt/custom-screensaver-aerial/issues/34), [#33](https://github.com/aabytt/custom-screensaver-aerial/issues/33)
- [webosbrew/sample-media-qmlvideo](https://github.com/webosbrew/sample-media-qmlvideo) — canonical PunchThrough + Video pattern
- [webosose/qml-webos-framework](https://github.com/webosose/qml-webos-framework) — `PunchThrough` implementation (`src/Eos/Items/src/punchthrough.cpp`)
- [openlgtv webOS hacking notes](https://gist.github.com/Informatic/1983f2e501444cf1cbd182e50820d6c1) — screensaver QML path, surface manager notes
- openlgtv Discord thread referenced in #30 (unreachable from CLI; worth reading during Phase 0)
