# PSP USB Video Class (UVC) plugin

## What's this?

This is a plugin that lets you stream your PSP screen to your computer via USB.

It follows the USB Video Class (UVC) standard, therefore it should work on any OS.

## Supported formats and resolutions

* 480x272 YUY2 @ 30 FPS and 60 FPS (WIP)

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

There is no button combination. The plugin shuts its USB driver down in
`module_stop` when the system unloads it (for example when the game exits).

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

Note: No, it *doesn't* stream audio. For that use a 3.5mm jack to jack adapter.
