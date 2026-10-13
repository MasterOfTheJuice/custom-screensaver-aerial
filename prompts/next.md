<!-- Summary (round 5): webOS 4 ACB helper must play the REAL Aerial playlist (streamed from videos.json URLs) instead of one local 50 s clip. Keep over-app display and exit-return working. Run autonomously; no approval wait. -->

You are working on the Aerial custom screensaver for LG webOS TVs. NON-INTERACTIVE run (--always-approve): nobody answers questions. Write a short plan, then implement, deploy, verify, and print the final report.

## State (read rtsp-evidence/round3/results.md and rtsp-evidence/round4/results.md first; they are authoritative)
- webOS 4 path (isWebOS4 in assets/screensaver-main.qml) launches the native card app org.aabytt.webos.custom-screensaver-aerial.acb (source native/, build native/build.sh, SDK /home/motj/sdk/arm-webos-linux-gnueabi_sdk-buildroot). It plays H.264 Annex-B on the ACB MAIN sink (MSE player type, ES pushed with setMediaVideoData).
- CAMERA-CONFIRMED and must stay working: Aerial over YouTube; EXIT and BACK relaunch returnTo (YouTube) via libluna-service2 LSRegister("") + LSCallOneReply applicationManager/launch; no helper left running.
- GAP: the helper loops ONE pre-converted 50 s local clip. The TV has no ffmpeg. curl to sylvan.apple.com fails TLS verification ("unable to get local issuer certificate"); curl -k works.

## Goal
On webOS 4 the helper plays the real Aerial playlist: videos chosen the same way the QML chooses them (assets/videos.json or wherever the QML loads its list, honoring the user's settings where the QML does, preferring url-1080-H264 then url-1080-SDR), streamed from the network, advancing to the next video when one ends, continuously, without a black gap longer than ~2 s if feasible. webOS 5+ unchanged.

## Design guidance (choose and justify; smallest robust option wins)
1. Getting the list/URL to the helper: QML already picks a URL; pass the playlist (or a path to the json and settings) in the applicationManager/launch params, read via the helper's launch parameters (argv JSON on webOS native apps). Keep QML changes minimal and gated.
2. Fetching: link libcurl (or the SDK's TLS lib) statically/dynamically in the helper. Do proper TLS verification: ship a CA bundle in the helper app dir (e.g. Mozilla cacert.pem from curl.se) and set CURLOPT_CAINFO. Do NOT disable verification. If the TV's clock or TLS version blocks it, document exactly why and only then fall back, clearly flagged.
3. Demuxing: the files are MP4/MOV with H.264 (AVC). Convert avcC/length-prefixed samples to Annex-B (prepend SPS/PPS on keyframes), with correct PTS. Options: a small MP4 parser (e.g. minimp4 single-header, MIT), or static libavformat from the SDK if available. Stream with HTTP range reads or progressive download into a ring/temporary file under /tmp (watch RAM/disk; the TV has limited space; clean up). Handle moov-at-end files via range requests. Audio is not required (screensaver), but keep the silent-audio trick from round 3 if ACB needs an audio stream.
4. Transition: on end-of-stream or error, move to the next URL. Reuse the ACB instance if it supports a new segment/flush; otherwise unload/reload quickly. Never leave the screen black on a single bad URL; skip it and log.
5. Keep the local clip as an offline fallback if no network.

## Environment
- SSH ONLY: ssh -F ~/.ssh/ssh_config tv ; scp -F ~/.ssh/ssh_config. Do not edit ~/.ssh.
- luna-send needs a TTY over SSH: ssh -F ~/.ssh/ssh_config tv "script -q -c \"luna-send -n 1 luna://... '{}'\" /dev/null"
- Camera: rtsp://192.168.11.179:554/live/ch00_0 (ffmpeg -rtsp_transport tcp). TV crop: crop=900:500:700:350 (verify by eye).
- Remote keys: pointer socket /tmp/netinput.pointer.sock, 32-bit big-endian length + "type:button\nname:EXIT\n\n" (see round 4).
- Reinstall the helper with the dev install path that updates appinfo (round 4 notes that editing appinfo on disk is not picked up).

## Constraints
- Minimal changes; webOS 4 only, gated; webOS 5+ path unchanged; screensaver window type stays SCREENSAVER; helper stays a card with the returnTo relaunch.
- Do NOT git commit or push. Do not edit prompts/next.md.
- NEVER claim something is on screen without a camera frame showing it.

## Verification (save under ./rtsp-evidence/round5/, write results.md)
1. YouTube in front; before.jpg confirms YouTube.
2. turnOnScreenSaver. Grab frames at ~15 s and +5 s: moving Aerial (numbers + your description). Log which URL is playing; confirm it is NOT the local clip (different scene than the round 3/4 underwater whale clip, or prove from logs that bytes came from the network).
3. Prove playlist advance: either let a video end (they are several minutes; acceptable) or add a debug option to cap each video to ~45 s for the test only (off by default), then grab frames across the transition and show the scene changed to the next video; report the gap length from logs/frames.
4. Robustness: one run with a bad URL inserted first (debug only) to show it is skipped.
5. EXIT: camera shows YouTube again; DOWN key works; no helper or temp files left (check /tmp usage).
6. Check memory/disk use on the TV during playback (free, df /tmp).

## Final report (print at end)
Design chosen and why; libraries added and licenses; TLS result; each attempt with camera results and numbers; transition gap; resource use; git diff --stat; remaining issues and recommended next step.
