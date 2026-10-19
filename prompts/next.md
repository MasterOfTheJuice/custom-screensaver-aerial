<!-- Summary (round 6): webOS 4 polish: correct place-name label for the video the helper is playing, plus housekeeping (debug override file, helper log, GStreamer noise, multi-SPS/PPS, reliable test key sending). Run autonomously; no approval wait. -->

You are working on the Aerial custom screensaver for LG webOS TVs. NON-INTERACTIVE run (--always-approve): nobody answers questions. Write a short plan, then implement, deploy, verify, and print the final report.

## State (read rtsp-evidence/round3/, round4/ and round5/results.md first; they are authoritative)
- Committed baseline: commit 2b5f744 on main. Work on top of it. Do NOT commit or push; leave changes uncommitted.
- webOS 4 path (isWebOS4 in assets/screensaver-main.qml) launches the card helper org.aabytt.webos.custom-screensaver-aerial.acb (native/, build native/build.sh, SDK /home/motj/sdk/arm-webos-linux-gnueabi_sdk-buildroot). The helper loads videos.json, shuffles, streams 1080p H.264 over verified TLS (bundled cacert.pem incl. Apple Root CA, dlopen'd system libcurl), demuxes MP4 in-tree, plays on the ACB MAIN sink, prefetches the next title (gap < 100 ms), skips bad URLs, and on EXIT/BACK relaunches returnTo via libluna-service2 LSRegister("").
- CAMERA-CONFIRMED and must stay working: Aerial over YouTube, playlist advance, bad-URL skip, EXIT returns to YouTube, remote works afterwards.

## Tasks (in priority order)
1. Place-name label. Today the QML shows the name for asset index 0 while the helper plays a shuffled, different URL. Make the label match what is on screen. Note round 3 found the screensaver QML process does NOT survive the helper's card launch, so QML cannot be the one drawing the label during playback; verify this first on the TV. Then pick the smallest correct option, e.g.: (a) the helper draws the label itself (SDL2 is already linked; an overlay on the UI plane above the ACB video, small text bottom-left, fades in/out like the original, using the same name/locale fields from videos.json that the QML uses, respecting the QML's show-name setting); or (b) if the label cannot be drawn without breaking the video, hide it on webOS 4 and document why. Do not regress video visibility. Camera must show the label text and it must match the playing title (log the title).
2. Debug override file: the jailed helper cannot unlink a root-owned /tmp file. Move the override to a location the helper owns (e.g. its app data dir or a path under /tmp it creates itself), or only honour it when the helper is built with a debug flag. A leftover file must never change normal playback. Document how tests enable it.
3. Helper log: cap its size (e.g. rotate at ~1 MB) and remove/truncate it on clean exit, or write only with a debug flag. No unbounded growth in /tmp.
4. SPS/PPS: emit each SPS and PPS as its own Annex-B NAL (start code each) instead of one glued blob.
5. GStreamer-CRITICAL lines at startup/unload: find the cause; fix if cheap and safe, otherwise document that they are harmless.
6. Clean up leftover files from earlier rounds in TV /tmp (aerial-acb, aerial-acb.new, smp-acb.ipk, aerial-ca.pem, test scripts) once you have confirmed nothing uses them.

## Test key sending (use for every test)
Pointer socket /tmp/netinput.pointer.sock: 32-bit big-endian length + "type:button\nname:<KEY>\n\n". KEEP THE SOCKET OPEN ~1.5 s after writing (round 5 showed an immediate close can drop the key). Put this in a small reusable script (e.g. tools/tv-key.sh, run on the TV over SSH) and use it everywhere.

## Environment
- SSH ONLY: ssh -F ~/.ssh/ssh_config tv ; scp -F ~/.ssh/ssh_config. Do not edit ~/.ssh.
- luna-send needs a TTY: ssh -F ~/.ssh/ssh_config tv "script -q -c \"luna-send -n 1 luna://... '{}'\" /dev/null"
- Camera: rtsp://192.168.11.179:554/live/ch00_0 (ffmpeg -rtsp_transport tcp). TV crop crop=900:500:700:350 (verify by eye). Host clock may differ from TV; use the camera overlay or date -u on the TV.
- Reinstall the helper via the dev install path that refreshes appinfo.

## Constraints
- Minimal changes, webOS 4 only and gated; webOS 5+ unchanged; screensaver window type stays SCREENSAVER; helper stays a card with the returnTo relaunch; TLS verification stays on.
- Do not edit prompts/next.md.
- NEVER claim something is on screen without a camera frame showing it.

## Verification (save under ./rtsp-evidence/round6/, write results.md)
1. YouTube in front, before.jpg confirms it.
2. turnOnScreenSaver; frames at ~15 s and +5 s show moving Aerial AND the label; zoom/crop the label area into its own image and read it; it must match the logged title. Repeat across one title change (use the debug cap via the new override mechanism) and show the label changed to the new title.
3. Show that a leftover override file in the old /tmp location has no effect on normal playback.
4. EXIT (with tools/tv-key.sh): camera shows YouTube; DOWN works; no helper process; helper log removed or bounded; /tmp clean.
5. Regression: playlist advance and bad-URL skip still work (brief re-run).

## Final report (print at end)
Per task: what changed and why, camera evidence (label crops), numbers, TV log findings (GStreamer cause), git diff --stat, remaining issues, recommended next step (expected: real-world triggers — natural idle timeout, over a paused YouTube video, Netflix, HDMI with a live source — and packaging the helper into the main IPK).
