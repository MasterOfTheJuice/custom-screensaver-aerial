/*
 * Aerial screensaver.
 *
 * Usage:
 *   mount --bind ./screensaver-main.qml /usr/palm/applications/com.webos.app.screensaver/qml/main.qml
 *
 * Test launch (no way to trigger on "No signal" screen)
 *   luna-send -n 1 'luna://com.webos.service.tvpower/power/turnOnScreenSaver' '{}'
 */
import QtQuick 2.4
import QtMultimedia 5.6
import Eos.Window 0.1
import Eos.Items 0.1
import WebOS.Global 1.0
import QtQuick.Window 2.2
import WebOSServices 1.0
import iLib 1.0 as I

WebOSWindow {
    id : window
    width : 1920
    height : 1080
    windowType : "_WEBOS_WINDOW_TYPE_SCREENSAVER"
    // webOS 4 has no punch-through, so an opaque window covers the video plane.
    color : isWebOS4 ? "transparent" : "black"
    appId : "com.webos.app.screensaver"
    visible : true
    property var poi
    property var poiIndex: 0
    property var settings
    property var playList
    property int randomIndex
    property int stalledCounter : 0
    property string sourceAlt
    property bool isWebOS4 : false
    property string versionLabel : "unknown"
    property string basePath : "file:///media/developer/apps/usr/palm/applications/org.aabytt.webos.custom-screensaver-aerial/assets/"
    // Companion native app. Jailer-only spawn never took the MAIN sink; SAM launch of this id did.
    property string acbAppId : "org.aabytt.webos.custom-screensaver-aerial.acb"
    property bool acbLaunched : false
    property bool acbQuerying : false
    property bool acbStopArmed : false
    property string acbReturnTo : ""
    property string acbStatus : "idle"
    Component.onCompleted : {
        // Version before any playback. Unknown stays on the webOS 5+ path.
        detectWebOSVersion()
        init()
        notificationsService.set('disable')
        if (!isWebOS4)
            playRandomVideo()
    }
    Component.onDestruction : {
        if (!isWebOS4)
            return
        stopAcbHelper()
        try {
            videoOutput.stop()
            videoOutput.source = ""
        } catch (e) {}
    }
    I.ILib {
        id : ilib
    }
    FontLoader {
        id : segoeUILight
        source : basePath + 'SegoeUI-Light.ttf'
    }
    Timer {
        id : refreshOSD
        interval : 1000
        running : true
        repeat : true
        onTriggered : {
            if (isWebOS4 && (!settings || !playList || !poi))
                return
            checkError()
            checkStatus()
            updateOSD()
        }
    }
    Video {
        id : videoOutput
        fillMode : VideoOutput.PreserveAspectCrop
        width : parent.width
        height : parent.height - 1 // non fullscreen to avoid screensaver automatic disabling
        // Opacity 1 on both paths. webOS 4 kept 0 in A1; the transparent window then showed the app underneath, not the plane.
        opacity : 1
        source : ""
        // webOS 4 draws with the ACB helper. Hide this item so an empty player does not cover that plane.
        visible : !isWebOS4
        autoPlay : true
        onStopped : {
            punchThroughArea.visible = false
            osd.visible = false
            fadeOutVideo.running = false
        }
        onPaused : {
            punchThroughArea.visible = false
            playRandomVideo()
            osd.visible = false
            fadeOutVideo.running = false
        }
        onPlaying : {
            punchThroughArea.visible = true
            if (!isWebOS4)
                fadeInVideo.running = true
            fadeInOsd.running = true
            osd.visible = true
        }
        PunchThrough {
            id : punchThroughArea
            visible : false
            x : 0
            y : 0
            z : -1
            width : parent.width
            height : parent.height
            Rectangle {
                id : opacityBox
                visible : !isWebOS4
                width : 1920
                height : 1080
                z : 1
                color : "black"
                OpacityAnimator {
                    id : fadeInVideo
                    target : opacityBox
                    from : 1
                    to : 0
                    duration : 3000
                    running : false
                }
                OpacityAnimator {
                    id : fadeOutVideo
                    target : opacityBox
                    from : 0
                    to : 1
                    duration : 5000
                    running : false
                }
            }
        }
    }
    Rectangle {
        id : osd
        opacity : 0
        visible : true
        color : "transparent"
        anchors.fill : parent
        anchors.margins : 65
        OpacityAnimator {
            id : fadeInOsd
            target : osd
            from : 0
            to : 1
            duration : 3000
            running : false
        }
        OpacityAnimator {
            id : fadeOutOsd
            target : osd
            from : 1
            to : 0
            duration : 5000
            running : false
        }
        Text {
            id : name
            opacity : isWebOS4 ? ((settings && settings.osdOpacity !== undefined) ? settings.osdOpacity / 100 : 0.6) : settings.osdOpacity / 100
            // webOS 4's QML process does not stay up once the helper card is in front.
            // The helper draws the place name for the URL it is actually playing.
            text : isWebOS4 ? "" : poi.strings[playList.assets[randomIndex].localizedNameKey]
            font.family : segoeUILight.name
            font.letterSpacing : -1
            fontSizeMode : Text.Fit
            font.pixelSize : 56
            y : parent.height * 0.9
            color : "white"
            style : Text.Raised
            styleColor : "black"
        }
        Text {
            id : poiOSD
            opacity : name.opacity
            text : isWebOS4 ? "" : poi.strings[playList.assets[randomIndex].pointsOfInterest[poiIndex]]
            font.family : name.font.family
            font.letterSpacing : name.font.letterSpacing
            fontSizeMode : name.fontSizeMode
            font.pixelSize : name.font.pixelSize - 16
            y : name.y + name.font.pixelSize + 10
            color : name.color
            style : name.style
            styleColor : name.styleColor
        }
        Text {
            id : timeOSD
            horizontalAlignment : Text.AlignRight
            anchors.right : parent.right
            opacity : name.opacity
            font.family : name.font.family
            font.letterSpacing : name.font.letterSpacing
            font.pixelSize : name.font.pixelSize + 23
            y : dateOSD.y - name.font.pixelSize - 40
            color : name.color
            style : name.style
            styleColor : name.styleColor
            fontSizeMode : name.fontSizeMode
        }
        Text {
            id : dateOSD
            horizontalAlignment : Text.AlignRight
            anchors.right : parent.right
            opacity : name.opacity
            font.family : name.font.family
            font.letterSpacing : name.font.letterSpacing
            font.pixelSize : name.font.pixelSize - 16
            y : name.y + name.font.pixelSize + 5
            color : name.color
            style : name.style
            styleColor : name.styleColor
            fontSizeMode : name.fontSizeMode
        }
    }
    Text {
        id : debug
        visible : isWebOS4 ? (settings && settings.debug) : settings.debug
        horizontalAlignment : Text.AlignRight
        anchors.right : parent.right
        anchors.margins : 25
        opacity : 0.7
        font.family : name.font.family
        font.pixelSize : name.font.pixelSize - 30
        color : name.color
        style : name.style
        styleColor : name.styleColor
        fontSizeMode : name.fontSizeMode
    }
    // webOS major from /var/run/nyx/os_info.json (webos_release). On this set that
    // file is authoritative. luna systemproperty returned an empty body over SSH.
    // major < 5 takes the webOS 4 path. If the file is unreadable, assets/platform.js
    // written by apply.sh supplies WEBOS_MAJOR. Unknown stays on the webOS 5+ path.
    function detectWebOSVersion() {
        isWebOS4 = false
        versionLabel = "unknown"
        if (readWebOSReleaseFile("file:///var/run/nyx/os_info.json", "nyx"))
            return
        try {
            Qt.include(basePath + "platform.js")
            if (typeof WEBOS_MAJOR === "number") {
                isWebOS4 = WEBOS_MAJOR < 5
                versionLabel = "platform.js:" + (typeof WEBOS_RELEASE === "string" ? WEBOS_RELEASE : WEBOS_MAJOR)
                return
            }
        } catch (e) {
            versionLabel = "platform.js-include:" + e
        }
        readPlatformJs(basePath + "platform.js")
    }
    function applyWebOSRelease(rel, source) {
        if (!rel)
            return false
        var major = parseInt(String(rel).split(".")[0], 10)
        if (isNaN(major))
            return false
        isWebOS4 = major < 5
        versionLabel = source + ":" + rel
        return true
    }
    function readWebOSReleaseFile(url, source) {
        try {
            var xhr = new XMLHttpRequest()
            xhr.open("GET", url, false)
            xhr.send(null)
            if (!xhr.responseText)
                return false
            var info = JSON.parse(xhr.responseText)
            return applyWebOSRelease(info.webos_release, source)
        } catch (e) {
            versionLabel = source + ":" + e
            return false
        }
    }
    function readPlatformJs(url) {
        try {
            var xhr = new XMLHttpRequest()
            xhr.open("GET", url, false)
            xhr.send(null)
            if (!xhr.responseText)
                return false
            var majorMatch = xhr.responseText.match(/WEBOS_MAJOR\s*=\s*(\d+)/)
            var relMatch = xhr.responseText.match(/WEBOS_RELEASE\s*=\s*"([^"]*)"/)
            if (!majorMatch)
                return false
            var major = parseInt(majorMatch[1], 10)
            isWebOS4 = major < 5
            versionLabel = "platform.js:" + (relMatch ? relMatch[1] : major)
            return true
        } catch (e) {
            versionLabel = "platform.js-xhr:" + e
            return false
        }
    }
    function init() {
        loadJSONData(basePath + 'settings.json', 'settings', loadResources)
    }
    function loadResources() {
        if (isWebOS4) {
            loadJSONData(basePath + 'videos.json', 'playList', function () {
                loadJSONData(basePath + 'locales/' + settings.localeLang + '.json', 'poi', playRandomVideo)
            })
            return
        }
        loadJSONData(basePath + 'videos.json', 'playList')
        loadJSONData(basePath + 'locales/' + settings.localeLang + '.json', 'poi', playRandomVideo)
    }
    function playWebOS4Video() {
        if (!playList || !playList.assets || !playList.assets.length || !settings)
            return
        stalledCounter = 0
        // webOS 4 does not start the QML Video item. The helper reads videos.json
        // (launch param "videos") and plays url-1080-H264, then url-1080-SDR.
        // sourceType and playLowerQuality are passed through; the H.264 pipeline
        // still prefers url-1080-H264 because the other 1080 entries are HEVC.
        randomIndex = 0
        sourceAlt = " - webOS 4 acb playlist"
        notificationsService.set('disable')
        launchAcbHelper()
    }
    function launchAcbHelper() {
        if (acbLaunched || acbQuerying)
            return
        acbQuerying = true
        acbStatus = "query-foreground"
        acbQueryTimeout.start()
        acbService.queryForeground()
    }
    function beginAcbPlayback() {
        if (acbLaunched)
            return
        acbQuerying = false
        acbLaunched = true
        acbStatus = "launching " + acbReturnTo
        acbService.launch()
        armAcbStop.start()
    }
    function stopAcbHelper() {
        // A SAM launch can dismiss this window at once. Closing the helper from that
        // destruction would kill the picture we just started, so the stop stays disarmed
        // until the window has been up long enough to be a real exit.
        if (!acbLaunched || !acbStopArmed)
            return
        acbLaunched = false
        acbStopArmed = false
        acbStatus = "closing"
        acbService.stop()
    }
    function playRandomVideo() {
        if (isWebOS4) {
            playWebOS4Video()
            return
        }
        stalledCounter = 0
        randomIndex = Math.floor(Math.random() * playList.assets.length)
        if (!playList.assets[randomIndex].viewed) {
            if (playList.assets[randomIndex][settings.sourceType]) {
                sourceAlt = ""
                videoOutput.source = playList.assets[randomIndex][settings.sourceType]
                    notificationsService.set('disable')
                    videoOutput.play()
            } else if(settings.sourceType == "url-4K-HDR" && settings.playLowerQuality){
                sourceAlt = " - n/a, trying url-4K-SDR"
                videoOutput.source = playList.assets[randomIndex]["url-4K-SDR"]
                    videoOutput.play()
            } else if(settings.sourceType == "url-1080-HDR" && settings.playLowerQuality){
                if(playList.assets[randomIndex]["url-1080-SDR"]){
                    sourceAlt = " - n/a, trying url-1080-SDR"
                    videoOutput.source = playList.assets[randomIndex]["url-1080-SDR"]
                    videoOutput.play()
                } else if(playList.assets[randomIndex]["url-1080-H264"]){
                    sourceAlt = " - n/a, trying url-1080-H264"
                    videoOutput.source = playList.assets[randomIndex]["url-1080-H264"]
                    videoOutput.play()
                } else playRandomVideo() 
            } else playRandomVideo() 
        } else 
            playRandomVideo()
    }

    function checkError() {
        if (isWebOS4)
            return
        if (videoOutput.error !== 0) {
            notificationsService.set('enable')
            punchThroughArea.visible = false
            playRandomVideo()
        }
    }

    function checkStatus() {
        if (isWebOS4) {
            if (settings && playList && playList.assets)
                debug.text = "webOS: " + versionLabel + " path=4 acb\n" + acbStatus
            return
        }
        if (videoOutput.position > 2000) {
            notificationsService.set('enable')
            playList.assets[randomIndex].viewed = true
        }
        if (Math.floor(videoOutput.position / 1000) == Math.floor(videoOutput.duration / 1000) - 5) {
            if (!isWebOS4)
                fadeOutVideo.running = true
            fadeOutOsd.running = true
        }
        if (videoOutput.status == MediaPlayer.EndOfMedia) 
            playRandomVideo()
        if (videoOutput.status === 1) 
            var status = 'NoMedia'
        else if (videoOutput.status === 2) {
            var status = 'Loading'
            stalledCounter ++
            if (stalledCounter > 25) {
                punchThroughArea.visible = false
                playRandomVideo()
            }
        }
        else if (videoOutput.status === 3) 
            var status = 'Loaded'
        else if (videoOutput.status === 4) 
            var status = 'Buffering'
        else if (videoOutput.status === 5) {
            var status = 'Stalled'
            stalledCounter ++
            if (stalledCounter > 25) {
                punchThroughArea.visible = false
                playRandomVideo()
            }
        }
        else if (videoOutput.status === 6) 
            var status = 'Buffered'
         else if (videoOutput.status === 7) 
            var status = 'EndOfMedia'
         else if (videoOutput.status === 8) 
            var status = 'InvalidMedia'
         else if (videoOutput.status === 0) 
            var status = 'UnknownStatus'
        
        if (videoOutput.playbackState === 1) 
            var playbackState = 'playing'
         else if (videoOutput.playbackState === 2) 
            var playbackState = 'paused'
         else if (videoOutput.playbackState === 0){ 
            var playbackState = 'stopped'
            stalledCounter ++
            if (stalledCounter > 15) {
                punchThroughArea.visible = false
                playRandomVideo()
            }
        }

        debug.text = "Video " + randomIndex + " of " + playList.assets.length +
        "\n webOS: " + versionLabel + (isWebOS4 ? " path=4" : " path=5+") +
        "\n Source Type: " + settings.sourceType + sourceAlt +
        "\n Try Other Source: " + settings.playLowerQuality +
        "\n Locale: " + settings.localeLang +
        "\n OSD opacity: " + settings.osdOpacity + "%" +
        "\n Timecode: " + Math.floor(videoOutput.position / 1000) + " / " + Math.floor(videoOutput.duration / 1000) +
        "\n Media Status: " + status +
        "\n Stalled Timeout: " + (25 - stalledCounter) +
        "\n Error: " + videoOutput.error + " " + videoOutput.errorString +
        "\n Playback State: " + playbackState +
        "\n Buffer Progress : " + (
        videoOutput.bufferProgress * 33.334).toFixed(0) + "%"
    }
    function updateOSD() {
        var DateFmt = ilib.require("DateFmt.js")
        var now = new Date()
        var time = new DateFmt({
            locale: settings.localeLang,
            timezone: "local",
            type: "time",
            length: "full"
        })
        var day = new DateFmt({
            locale: settings.localeLang,
            timezone: "local",
            type: "date",
            date: "dmw",
            length: "full"
        })
        timeOSD.text = time.format(now)
        if (poi.date) 
            dateOSD.text = poi.date.daysOfWeek[now.getDay()
            ] + ", " + now.getDate() + " " + poi.date.months[now.getMonth()
            ]
         else 
            dateOSD.text = day.format(now)
        
        if (playList.assets[randomIndex].pointsOfInterest[Math.floor(videoOutput.position / 1000)]) 
            poiIndex = Math.floor(videoOutput.position / 1000)        
    }

    Timer {
        id : armAcbStop
        interval : 4000
        repeat : false
        onTriggered : acbStopArmed = true
    }
    Timer {
        id : acbQueryTimeout
        interval : 1500
        repeat : false
        onTriggered : beginAcbPlayback()
    }
    Item {
        id : keyCatcher
        anchors.fill : parent
        focus : isWebOS4
        Keys.onPressed : {
            if (!isWebOS4)
                return
            acbStopArmed = true
            stopAcbHelper()
        }
    }
    Service {
        id : acbService
        appId : (typeof globalVars !== "undefined" && globalVars.appId) ? globalVars.appId : "com.webos.app.screensaver"
        onResponse : {
            acbStatus = method + " " + payload
            if (!acbQuerying || method.indexOf("getForegroundAppInfo") < 0)
                return
            try {
                var info = JSON.parse(payload)
                var id = info.appId || ""
                if (id && id !== acbAppId && id.indexOf("screensaver") < 0 && id.indexOf("hdmi") < 0)
                    acbReturnTo = id
            } catch (e) {}
            beginAcbPlayback()
        }
        function queryForeground() {
            call("luna://com.webos.applicationManager", "/getForegroundAppInfo", "{}")
        }
        function launch() {
            var params = {
                log : "/tmp/aerial-acb.log",
                videos : "/media/developer/apps/usr/palm/applications/org.aabytt.webos.custom-screensaver-aerial/assets/videos.json",
                sourceType : (settings && settings.sourceType) ? settings.sourceType : "",
                playLowerQuality : (settings && settings.playLowerQuality) ? "true" : "false",
                locale : (settings && settings.localeLang) ? settings.localeLang : "",
                osdOpacity : (settings && settings.osdOpacity !== undefined) ? String(settings.osdOpacity) : ""
            }
            if (acbReturnTo)
                params.returnTo = acbReturnTo
            call("luna://com.webos.applicationManager",
                "/launch",
                JSON.stringify({
                    id : acbAppId,
                    params : params
                }))
        }
        function stop() {
            call("luna://com.webos.applicationManager",
                "/closeByAppId",
                JSON.stringify({ id : acbAppId }))
        }
    }
    Service {
        id : notificationsService
        // Creation-time binding. webOS 5+ still uses globalVars.appId when it exists.
        appId : (typeof globalVars !== "undefined" && globalVars.appId) ? globalVars.appId : "com.webos.app.screensaver"
        function set(param) {
            call("luna://com.webos.notification/", param)
        }
    }
    function loadJSONData(url, targetVar, callback) {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function () {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status === 200) {
                    var jsonData = JSON.parse(xhr.responseText)
                    eval(targetVar + " = jsonData")
                    callback()
                } else {
                    console.error("Error loading JSON data:", xhr.statusText)
                    name.text = xhr.statusText
                }
            }
        }
        xhr.open("GET", url)
        xhr.send()
    }
}
