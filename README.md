# PSP USB Video Class (UVC) plugin

## What's this?

This is a plugin that lets you stream your PSP screen to your computer via USB.

It follows the USB Video Class (UVC) standard, therefore it should work on any OS.

## Supported formats and resolutions

* 480x272 YUY2 @ 30 FPS and 60 FPS (WIP)

## Audio

The device also carries the game audio as a standard USB Audio Class 1.0
capture device (44.1 kHz, 16-bit, stereo), so it shows up next to the video as
an audio input ("line in") in OBS, mpv, ffmpeg, etc. No driver is needed.

* Only audio played by **games** (GAME.TXT) is captured; the XMB's own sounds
  are not.
* Capture starts when an application opens the audio input and stops when it
  closes it, so it costs nothing while you are only watching video.
* It needs a CFW that exports `sctrlHENPatchSyscall` from SystemControl (PRO,
  ARK and ME do). Without it the audio input is present but silent; the log
  says so.
* If the host never configures the device (cable in, no `usb_attach` within
  3 s) the plugin re-registers with a smaller descriptor set: first without the
  class-specific audio endpoint descriptor, then video only. The log says
  which one was tried. Video therefore keeps working even if the host rejects
  the audio interface; the cost is that a charger-only connection may also
  trigger it, and audio is then off until the next reboot.
* `make UVC_AUDIO=0` builds the video-only device (no audio interface at all).
  Use it if the host stops enumerating the device with the audio interface
  present.
* `make UVC_AUDIO_HOOKS=0` keeps the audio device but skips the `sceAudio`
  hooks (the input is silent). Use it to tell a USB problem from a hook problem.
* Measured on a PSP-1000: the isochronous endpoint keeps completing only if one
  request is queued at a time, after the previous one has completed (queueing
  several ahead stalls after the first). The packet rate that results is not 1
  per millisecond, so every packet is sized by the real time elapsed since the
  previous one; that keeps the stream at exactly 44.1 kHz. Two other ways of
  feeding the endpoint are tried automatically if that one ever stalls.
* Every time the host opens the audio input, a 0.5 s 440 Hz beep is sent first.
  It proves the host is receiving samples; `make UVC_AUDIO_BEEP=0` removes it.
  The `levels` line in the log shows the loudest sample of the game audio going
  in and of what is sent to USB.
* The `Audio` lines in `uvc.log` show packets sent, errors, and what the
  capture saw (`hooks` is a bitmask of the 12 hooked sceAudio calls).
* The audio streaming endpoint is a standard 9-byte UAC 1.0 isochronous
  endpoint descriptor (bRefresh and bSynchAddress included), laid out the way the
  PSP bus driver expects (7 bytes inline, the rest in the extension).
* Isochronous audio only reaches the host because the plugin patches two
  instructions of the PSP's USB bus driver (sceUSB_Driver) at run time, after checking they
  are the expected ones: the stock frame targeting made nearly every packet miss the
  host's poll.
* Audio and video are separate USB functions: they are not
  synchronised by the plugin. Expect the audio to be a few tens of ms ahead of
  the picture; most capture software has an audio delay setting.

## Download and installation

**Download**:

* [uvc.prx](https://github.com/xerpi/psp-uvc-usb-video-class/releases)

**Installation**:

1. Copy `uvc.prx` to `ms0:/seplugins/`
2. Add `uvc.prx` to `ms0:/seplugins/VSH.TXT` and/or `ms0:/seplugins/GAME.TXT`
3. Reboot your PSP

**Logging**

The plugin never writes to the screen. It logs to a file next to the PRX
(`uvc.prx` -> `uvc.log`, e.g. `ms0:/seplugins/uvc.log`). Every session starts
with a line giving the build date, time and timezone of the PRX, followed by
the result of every USB driver call, USB state changes, framebuffer changes
and a streaming summary every 5 seconds. The file is appended to, so a XMB
session and a game session appear one after the other. Any USB or frame error
is logged with its error code; nothing is retried or worked around silently.

**Stopping**

There is no button combination. The plugin shuts its USB driver down when the
system unloads it (`module_stop`) and when the system reboots, which is what
happens when a game exits via HOME -> Exit Game (`module_reboot_before`).
Both log `... entered` and `... done` lines.

**Viewing**

On Windows I recommend PotPlayer for viewing, and on Linux I recommend *mpv* or *mplayer*.
* `mpv av://v4l2:/dev/videoN --profile=low-latency --untimed`
* `mplayer tv:// -tv driver=v4l2:device=/dev/videoN`

You can use OBS to capture/live-stream the incoming video from the PSP.

**Compilation**

* If you want to compile the source code, [pspsdk](https://github.com/pspdev/pspsdk/) is needed.

## Troubleshooting

* If you use Windows 10 you might have to change the Camera access permissions on the Privacy Settings.
* Check `uvc.log` first: it says which step failed and with which error code.
* Remember that if anything goes wrong (like the PSP not booting) you can always press R at boot to enter recovery mode.

