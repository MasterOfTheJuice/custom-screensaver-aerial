<!-- Summary (round 2): webOS 4 video fix for Aerial. Stage A is QML-only and gated on version (based on the alexastall fork); stage B is a native ACB helper fallback. Run autonomously; no approval wait. -->

You are working on the Aerial custom screensaver for LG webOS TVs. This is a NON-INTERACTIVE run (--always-approve). Nobody will answer questions or approve a plan. Do NOT stop after planning: write a short plan in your output, then immediately implement, deploy, and verify. Finish with a written report (see end).

## Attempt log
- Round 1: the agent started in plan mode, read the repo, and exited without changing, deploying, or testing anything (it waited for an approval that never comes). No RTSP result.
- Pre-check by the orchestrator: `ssh root@192.168.11.55` with default settings fails (publickey). The working SSH config is NOT in the default location: use `ssh -F ~/.ssh/ssh_config tv` and `scp -F ~/.ssh/ssh_config <file> tv:<path>` (Host tv -> 192.168.11.55, user root, key already configured). The RTSP camera is reachable: ffmpeg grabbed a frame from rtsp://192.168.11.179:554/live/ch00_0.
- Repo notes: AGENTS.md and deploy-and-check.sh exist in the repo; deploy-and-check.sh uses plain `ssh root@...`, so pass the -F config (or export GIT/SSH options) rather than relying on its defaults. Do not edit ~/.ssh.

## Goal
Make Aerial video actually show on screen on webOS 4, and keep webOS 5+ behaving exactly as it does today.

## Environment (fill in)
- TV: webOS 4.x, rooted, Homebrew Channel, 192.168.11.55. SSH: ssh -F ~/.ssh/ssh_config tv
- IP camera pointed at the TV: rtsp://192.168.11.179:554/live/ch00_0
- Repo: you are in it (~/Repositories/custom-screensaver-aerial, a copy of aabytt upstream). Add the remote https://github.com/alexastall/custom-screensaver-aerial (its webOS 4 attempt is 9 commits ahead of upstream main)

## Background (verified research)
- The screensaver is the QML file assets/screensaver-main.qml. assets/apply.sh bind-mounts it over /usr/palm/applications/com.webos.app.screensaver/qml/main.qml. Autostart is a symlink at /var/lib/webosbrew/init.d/50-custom-screensaver-aerial. The app is installed at /media/developer/apps/usr/palm/applications/org.aabytt.webos.custom-screensaver-aerial/.
- Upstream has a WebOSWindow (windowType _WEBOS_WINDOW_TYPE_SCREENSAVER, color "black", appId com.webos.app.screensaver), a QtMultimedia Video set to parent.height - 1 (on purpose, so the screensaver doesn't auto-exit), PunchThrough{z:-1} inside the Video, and a black opacityBox that fades in and out.
- Root cause on webOS 4: PunchThrough isn't supported (log: "setWindowPunchThroughRectFunc is not defined"). The umedia backend draws to the hardware video plane, so the opaque black window hides the video. Symptom: a black screen while the timecode keeps running.
- The alexastall fork, tested on webOS 4.10 (K5LP), uses a transparent WebOSWindow, a full-height Video with opacity 0, no opaque fade overlay, PunchThrough kept as a best effort, and window type kept as SCREENSAVER. It also has a 1080p H.264 default (4K HDR fails with uMediaClient error 204), a QML resource-load race fix, a hard-coded appId because globalVars is undefined on webOS 4, a keyCatcher Item replacing WebOSWindow focus (which crashed qml-runner), video stopped in Component.onDestruction, and an apply.sh that always remounts. Its own commits flip between opacity 0 and 1, and it never confirmed visible video. Treat it as a candidate, not a fix.
- Rejected approaches: the CARD window type leaves power and media stuck. Launching through applicationManager leaves the Home UI visible and the remote stuck.
- An active HDMI input, Live TV or Netflix can take over the video plane. Test from the Home screen.

## Constraints
- Smallest change possible. Every webOS 4 behaviour goes behind a runtime version check. The webOS 5+ code path (PunchThrough, black window, fade overlay) stays byte-for-byte unchanged in behaviour.
- Version detection: try luna://com.webos.service.tv.systemproperty/getSystemInfo {"keys":["sdkVersion"]} or /var/run/nyx/os_info.json. Check first on the TV over SSH which one works, because neither has been verified.
- Don't change the window type away from SCREENSAVER.
- Do NOT git commit or push. Leave changes uncommitted in the working tree so they can be reviewed. Do not edit prompts/next.md.

## Plan, stage A (QML only)
1. Diff upstream against alexastall: git diff origin/main alexastall/main. Bring over only the webOS 4 pieces and gate them on the version.
2. Prefer a 1080p H.264 source on webOS 4.
3. Deploy: copy the changed files to the app's assets directory on the TV, run assets/apply.sh, and check the mount with: findmnt /usr/palm/applications/com.webos.app.screensaver/qml/main.qml
4. Launch: luna-send -n 1 'luna://com.webos.service.tvpower/power/turnOnScreenSaver' '{}'
5. Check logs: grep -iE 'PunchThrough|setWindowPunchThroughRect|uMediaClient|qml' /var/log/messages (or use journalctl/PmLogDump if present)
6. Verify with RTSP (see below). If the frame is blank, try the variations one at a time (Video opacity 0 vs 1, window color transparent vs no overlay) and record each result.

## Stage B (only if stage A stays blank after the variations; propose it, don't start without approval)
A native webOS 4 helper modelled on https://github.com/webosbrew/samples/tree/main/media/smp/acb (acb_plane.c, main.c): AcbAPI_create, then AcbAPI_initialize(id, PLAYER_TYPE_MSE, appId, cb), then AcbAPI_setMediaId before Load(), then AcbAPI_setDisplayWindow fullscreen after Load. Only after LOADCOMPLETED: AcbAPI_setSinkType(SINK_TYPE_MAIN), then setState(APPSTATE_FOREGROUND, PLAYSTATE_LOADED), then PLAYING. On exit, UNLOADED, then finalize and destroy. setMediaVideoData takes three arguments, despite what the header says. Build with the buildroot-nc4 toolchain (target media-smp-acb-webos4). The sample plays raw Annex-B H.264, so MP4 demuxing is needed. If ACB leaks and the screen goes black, run "restart AcbService". The QML launches the helper and stays transparent. Nothing changes on webOS 5+, where libAcbAPI isn't present.

## Verification (required for every attempt)
Save every frame under ./rtsp-evidence/round2/ in the repo (create it; e.g. before.jpg, a1-after1.jpg, a1-after2.jpg, per attempt) and write ./rtsp-evidence/round2/results.md with YAVG and SSIM/PSNR per attempt.
1. Grab a baseline frame before launching: ffmpeg -rtsp_transport tcp -i "rtsp://192.168.11.179:554/live/ch00_0" -frames:v 1 -y before.jpg
2. Launch the screensaver, wait about 15 s, then grab after1.jpg, wait 5 s more and grab after2.jpg.
3. Prove there's real video, not a black screen or a static overlay: the TV area should have a mean luma clearly above black, for example ffmpeg -i after1.jpg -vf signalstats -f null - (look at YAVG), and after1 and after2 should differ (moving video), for example with ffmpeg's ssim/psnr filter or an ImageMagick compare. Look at the images yourself as well.
4. Exit the screensaver (press a remote key or send the luna call) and confirm the TV returns to normal and the remote works.
5. Regression check: if a webOS 5+ TV is available, run the same check there. If not, show from the diff that the 5+ path hasn't changed.

## Acceptance criteria
- Video visibly appears on the webOS 4 TV, not a blank or black screen.
- The RTSP frames prove non-blank, changing video content (attach the luma and difference numbers plus the images).
- webOS 5+ behaviour is unchanged.
- The change is minimal and gated on version.

## Final report (print at the end of your output)
webOS version detected and how; files changed (git diff --stat); each attempt with its variation and RTSP numbers; whether video was visible; errors from TV logs; your recommendation for the next step (another stage A variation, or stage B). Report the final diff, the attempt log (what was tried and the RTSP result for each), and any remaining issues.
