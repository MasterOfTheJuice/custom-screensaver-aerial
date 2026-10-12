# AGENTS.md

## Goal
Add working video display for the Aerial screensaver on rooted LG webOS 4 TVs.
Existing support is webOS 5+. On webOS 4 the video plane does not appear (blank screen) due to layering / video-surface differences.

## Key technical difference
- **webOS 5+**: Exported window (`wl_webos_exported` / `SDL_webOSCreateExportedWindow` + `SDL_webOSSetExportedWindow` with src/dst rects). Video lives on its own layer under the UI.
- **webOS 4**: ACB (`libAcbAPI`). Typical sequence: `AcbAPI_create` → `initialize` → `setMediaId` → `setSinkType` → `setState` → `setDisplayWindow`.

Reference:
- webOS Homebrew media guide (video placement section)
- webosbrew/samples (media/smp/acb path, verified on webOS 4 hardware)

## How the screensaver works
- Bind-mounts a custom `main.qml` over the system screensaver QML
  (`/usr/palm/applications/com.webos.app.screensaver/qml/main.qml`).
- Base approach comes from webosbrew/custom-screensaver.

## Constraints
- Keep webOS 5+ behavior working.
- Prefer the smallest change that makes the video visible on webOS 4.
- Do not break the existing bind-mount / autostart mechanism.

## Testing / acceptance
1. Build and package the IPK.
2. Deploy over SSH to the TV and launch the screensaver.
3. Capture a frame from the RTSP IP cam aimed at the TV.
4. The frame must show actual aerial video content (not a blank / black screen).

## Tools available to the agent
- SSH access to the TV (keys already configured).
- RTSP stream from the IP cam (use ffmpeg to grab frames).
- Local build tooling for the IPK.

## Preferred workflow
- Start in plan mode for non-trivial changes.
- Use the deploy + RTSP check skill after every meaningful change.
- Document any webOS version detection logic you add.
