[中文](README.md) | English

# RK-KVM

A **userspace IP-KVM** on the **Tronlong TL3506-MiniEVM-NAND (Rockchip RK3506, armv7l)**:
capture the target's HDMI through a USB capture dongle and emulate keyboard/mouse (HID) via the
board's USB OTG port, for remote control from a browser. Includes an authenticated web UI and an
admin console that can run a shell.

> Because this kernel has **no `uvcvideo` driver and no `usb_f_hid`**, the project implements
> both video capture and HID **entirely in userspace** — no kernel rebuild required (handy when
> no BSP/SDK is available for the board).

## Features

- **Video**: userspace UVC (libusb + libuvc) reading a USB capture dongle, output as MJPEG
  - Enumerates the capture device's supported resolutions/frame rates at runtime, switchable in the UI
  - Software re-encode to lower bitrate (libjpeg-turbo + NEON), adjustable quality
  - Transport via **HTTP MJPEG** or **WebSocket (`/vws`)**; live FPS/bitrate readout
  - Auto black-border detection/alignment, direct view and single-frame snapshot
- **Keyboard/mouse (HID)**: FunctionFS userspace HID gadget
  - Keyboard (boot protocol)
  - Relative mouse
  - **Absolute pointer** (positions by on-screen coordinates, fixing border/offset issues)
  - **Consumer Control** (multimedia/function keys: volume, play/pause, prev/next, mute, calculator, mail, home, …)
- **Audio**: userspace UAC (libusb isochronous) captures HDMI audio from the capture dongle
  (the board kernel lacks `snd-usb-audio`), streamed to the browser; can switch to the board ALSA source
- **Virtual media**: exposes an **image file or block device** (e.g. SD/card reader) to the target
  via the USB `mass_storage` function; switchable, can be read-only
- **Web frontend**: Canvas MJPEG rendering, **auto black-border alignment**, pointer lock / absolute
  follow, fullscreen, full keyboard capture (Keyboard Lock), Ctrl+Alt+Del / Alt+Tab buttons
  - **Toggleable virtual keyboard**: F1–F12 / PrtScn / ScrLk / Pause / navigation / multimedia keys;
    Ctrl/Shift/Alt/Win modifier lock; LEDs follow the real keyboard
- **Power/reset jumper**: pulse GPIO to emulate the target's power/reset button (configurable,
  persistent), with a status-sense pin
- **Login auth**: session cookie + SHA-256 (salted) password; all endpoints protected
- **Admin console**: tabbed layout; run a shell (root), edit config/password, GPIO, reboot/shutdown;
  and add **SvcManager columns** to manage other daemons via the svcmgr API (local or remote)

## Architecture

```
Target HDMI ──USB dongle──► RK3506 host port ──libusb/libuvc (userspace)──► MJPEG HTTP / WS
             └─audio──────► RK3506 host port ──libusb iso (UAC)───────────► PCM → WS playback
Target USB  ◄──OTG cable─── RK3506 OTG port   ──FunctionFS HID───────────► keyboard/mouse reports
Target USB  ◄──OTG cable─── RK3506 OTG port   ──mass_storage function────► virtual disk/image
GPIO (J4)   ──────────────► power/reset button (via transistor/optocoupler)
Browser ──HTTPS──► gateway ──► board :8080  (web / video / audio / control / admin)
                              └─(optional) svcmgr :8083 to manage other daemons
```

Main processes:

| Process | Role | Listen |
|---|---|---|
| `rkkvm-hid` | FunctionFS HID gadget + line-protocol control service | `127.0.0.1:5001` |
| `rkkvm-video` | UVC capture + HTTP/WS + audio/virtual-media control + web + auth + admin + GPIO | `0.0.0.0:8080` |
| `uac_capture` | userspace UAC audio capture (spawned on demand by `rkkvm-video`/S53, writes a FIFO) | none |

`rkkvm-video` forwards control commands to `rkkvm-hid` over local TCP.

## Layout

```
src/    hid_ffs.c          FunctionFS HID gadget source
        video_uvc.c        UVC capture + HTTP/WS + web/auth/admin/GPIO/audio/virtual-media source
        uac_capture.c      userspace UAC audio capture (isochronous)
        gpioscan.c         GPIO short/jumper scanner
        usbdump.c          USB descriptor dump tool
deploy/ rkkvm.html         main UI   admin.html admin console   login.html login page
        S49kvmgpio.sh      configure jumper GPIO at boot
        S51rkkvm-hid.sh / S52rkkvm-video.sh / S53rkkvm-audio.sh  service autostart
        kvm.conf           example GPIO config
        audio.conf         example audio source config (UAC / ALSA)
        gpiotest.sh        single-pair GPIO short detection
tools/  kvmctl.py          host-side HID test client
cmake-armv7-musl.cmake     cross-compile toolchain file
build.sh                   one-shot cross build
```

## Build

Requires Linux + `cmake`/`make`/`curl`. `build.sh` downloads the Bootlin armv7-eabihf (musl)
toolchain and cross-compiles libusb / libuvc / libjpeg-turbo (all static), then builds the programs:

```bash
./build.sh
# outputs: src/rkkvm-hid  src/rkkvm-video  src/gpioscan  src/uac_capture  src/usbdump
```

## Deploy (to the board)

```bash
# programs and web files
scp src/rkkvm-hid src/rkkvm-video src/uac_capture src/usbdump root@<board>:/userdata/
scp deploy/rkkvm.html deploy/admin.html deploy/login.html root@<board>:/userdata/
scp deploy/kvm.conf deploy/audio.conf root@<board>:/userdata/
# init scripts
scp deploy/S49kvmgpio.sh deploy/S51rkkvm-hid.sh deploy/S52rkkvm-video.sh \
    deploy/S53rkkvm-audio.sh root@<board>:/etc/init.d/
ssh root@<board> 'chmod +x /userdata/rkkvm-* /userdata/uac_capture /userdata/usbdump /etc/init.d/S4* /etc/init.d/S5*'
```

Open `http://<board>:8080/`; unauthenticated requests redirect to `/login`.
**Default account `admin` / `admin` — change it in the admin console immediately.**
(Forgot password: delete `/userdata/passwd` and restart the service to restore `admin`/`admin`.)

### Expose to other networks (optional)
If the board is on a private subnet (e.g. 192.168.50.0/24), port-forward on the gateway, or
terminate TLS with a reverse proxy:

```
firewall-cmd --permanent --zone=public --add-forward-port=port=18080:proto=tcp:toport=8080:toaddr=<board>
```

Or with Caddy (HTTPS; Keyboard Lock needs a secure context):

```
https://<gateway>:18443 {
    tls internal
    # admin "Service manager" columns reach the board's svcmgr via a same-origin path
    # (avoids mixed-content blocking on an HTTPS page)
    handle_path /svcmgr/* {
        reverse_proxy <board>:8083
    }
    handle {
        reverse_proxy <board>:8080 { flush_interval -1 }
    }
}
```

## HTTP API (all require login)

| Path | Description |
|---|---|
| `/` `/admin` `/login` `/logout` | UI / admin / login / logout |
| `/stream` `/snapshot` | MJPEG stream / single JPEG frame |
| `/vws` | WebSocket MJPEG video (low latency) |
| `/aws` | WebSocket audio playback (`?src=alsa` for board source) |
| `/status` | status (resolution/fps/bitrate/GPIO/power) |
| `/formats` | modes supported by the capture device (JSON) |
| `/setres?w=&h=&fps=` | switch resolution/frame rate |
| `/setenc?mode=&quality=` | passthrough / software re-encode |
| `/msd?card=\|file=\|off=\|ro=` | virtual media (block device/image/off/read-only) |
| `/gpio?n=&act=read\|high\|low\|pulse&ms=` | GPIO control |
| `/gpiocfg?power=&reset=&status=` | save jumper GPIO config |
| `/ws` | WebSocket low-latency keyboard/mouse |
| `/passwd` (POST `old=&new=`) | change login password |
| `/admin/exec` (POST `cmd=`) | run a shell in admin (root) |

## Audio

- The board kernel has **no `snd-usb-audio`**, so HDMI audio from the capture dongle is read via
  **userspace UAC** (libusb isochronous).
- `uac_capture` reads PCM from the dongle into the FIFO `/run/rk.pcm`; `S53rkkvm-audio.sh` can also
  use `arecord` to read the board sound card into `/run/rk2.pcm`.
- Source selectable in `/userdata/audio.conf`; the browser plays it over WebSocket `/aws`
  (`/aws?src=alsa` for the board source).

## Virtual media

- Exposes a **block device or image file** to the target via the gadget `mass_storage` function:
  `/msd?card=1` (auto-pick the first available block device), `/msd?file=/userdata/msd.img`,
  `/msd?off=1` (off), `/msd?ro=1` (read-only).
- Requires kernel `usb_f_mass_storage` (built into this board). Sample 8 MB FAT image: `/userdata/msd.img`.
- Adding/removing gadget functions at runtime must be done with care (see "Known limitations").

## Service manager (optional)

- The admin console's "**Service manager**" tab manages other daemons (e.g. `frpc` / `easytier`)
  through the **svcmgr** API; each column chooses **local** or **custom address + API token**.
- svcmgr is now a standalone project: **<https://github.com/Nacano12345/rk-svcmgr>**
  (`make CROSS=arm-linux- STATIC=1`).
- On the board: binary `/userdata/svcmgr`, autostart `/etc/init.d/S60svcmgr.sh`, token
  `/userdata/services/token`, instance DB `/userdata/services/services.conf`.
- Under the HTTPS admin, it is reached via the gateway Caddy `/svcmgr/*` **same-origin reverse proxy**
  to avoid mixed-content blocking (see the Caddy config above).

## Related projects

- [**rk-svcmgr**](https://github.com/Nacano12345/rk-svcmgr) — the generic multi-instance service
  manager extracted from this project's admin console (single-file C, HTTP API + built-in web UI).

## GPIO jumper (important)

- On the NAND version of the TL3506-MiniEVM, **EXPORT(J2) is unusable as GPIO**: its FSPI pins are
  the SPI NAND signals, which the manual explicitly marks "not usable on the NAND version" —
  miswiring can crash or even corrupt data.
- Workable option: reuse the **UART0 debug header J4** (pin1=3V3, pin2=GND, pin3/4 = UART0 TX/RX =
  `gpio22`/`gpio23`) as GPIO for the power/reset jumper (exportable via `/sys/class/gpio`, verified
  working). This disables the debug serial console.
- **Always isolate with a transistor/optocoupler** before connecting to the target's power button; never wire directly.
- `gpioscan` / `gpiotest` help probe pins and confirm shorts.

## Known limitations

- This kernel has **no `uvcvideo` / `usb_f_hid` / `snd-usb-audio`**, so video, HID and USB audio are
  all implemented in userspace; it still needs `FunctionFS`, `libusb` (usbfs), `configfs`,
  `usb_f_mass_storage`, etc. (all present on this board).
- Do **not** unbind the ffs gadget's UDC at runtime: this kernel tends to hang on that operation;
  just reboot the board after updating the program.
- Password/session: sessions live in memory; you must log in again after a reboot (reset above).
- Browsers require a user gesture before audio playback starts (sound begins after the first click).
- The admin console runs arbitrary commands as root — extremely powerful; use it only on a trusted
  network and always change the default password.

## Acknowledgements

Some inspiration comes from the excellent open-source IP-KVM projects below — with respect:

- **PiKVM** — <https://github.com/pikvm/pikvm> (<https://pikvm.org>)
- **OneKVM** — <https://github.com/mofeng-git/One-KVM>

## License

[MIT](LICENSE) © 2026 Nacano12345
