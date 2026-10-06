[中文](README.md) | English

# RK-KVM

A userspace IP-KVM for the Tronlong TL3506-MiniEVM-NAND (Rockchip RK3506, armv7l). It captures the
target's HDMI through a USB capture device and emulates keyboard and mouse over the board's USB OTG
port using FunctionFS, enabling remote control from a browser. It provides an authenticated web
interface and an admin console that can execute commands.

This board's kernel does not provide `uvcvideo` or `usb_f_hid`, so both video capture and HID are
implemented in userspace. No kernel rebuild is required.

## Features

**Video**

- Reads the USB capture device in userspace via libusb/libuvc and outputs an MJPEG stream.
- Enumerates the capture device's supported resolutions and frame rates at runtime; switchable in the UI.
- Supports passthrough and software re-encoding (libjpeg-turbo + NEON) with adjustable quality.
- Supports both HTTP MJPEG and WebSocket (`/vws`) transport; the UI shows measured frame rate and bitrate.
- Supports automatic black-border detection/alignment and single-frame snapshots.

**Keyboard/mouse (HID)**

A FunctionFS userspace HID gadget providing:

- Keyboard (boot protocol).
- Relative mouse.
- Absolute pointer (positions by on-screen coordinates, avoiding border/offset issues).
- Consumer Control (multimedia and function keys: volume, play/pause, previous/next, mute, calculator, mail, home, etc.).

**Audio**

Captures HDMI audio from the capture device via userspace UAC (libusb isochronous) since the kernel
lacks `snd-usb-audio`, and streams it to the browser over WebSocket. The board's ALSA source can also
be selected.

**Virtual media**

Exposes an image file or block device (such as an SD card or card reader) to the target through the
USB `mass_storage` function; switchable and can be set read-only.

**Web frontend**

- Canvas MJPEG rendering, automatic black-border alignment, pointer lock and absolute follow, fullscreen,
  and full keyboard capture (Keyboard Lock).
- Buttons for key combinations such as Ctrl+Alt+Del and Alt+Tab.
- A toggleable virtual keyboard (F1–F12, PrtScn, ScrLk, Pause, navigation and multimedia keys) with
  Ctrl/Shift/Alt/Win modifier lock; LEDs follow the real keyboard.

**Other**

- Power/reset: pulses GPIO to emulate the target's power and reset buttons; the configuration is
  persistent and a status-sense pin is supported.
- Authentication: session cookie and SHA-256 (salted) password; all endpoints require login.
- Admin console: a tabbed layout to run a shell (root), edit configuration and password, operate GPIO,
  and reboot or shut down. SvcManager columns can manage other daemons via the svcmgr API (local or remote).

## Architecture

```
Target HDMI ──USB capture──► RK3506 host port ──libusb/libuvc (userspace)──► MJPEG HTTP / WS
             └─audio───────► RK3506 host port ──libusb iso (UAC)───────────► PCM → WS playback
Target USB  ◄──OTG cable──── RK3506 OTG port   ──FunctionFS HID───────────► keyboard/mouse reports
Target USB  ◄──OTG cable──── RK3506 OTG port   ──mass_storage function────► virtual disk/image
GPIO (J4)   ───────────────► power/reset button (via transistor/optocoupler)
Browser ──HTTPS──► gateway ──► board :8080 (web / video / audio / control / admin)
                              └─(optional) svcmgr :8083 to manage other daemons
```

Main processes:

| Process | Role | Listen |
|---|---|---|
| `rkkvm-hid` | FunctionFS HID gadget and line-protocol control service | `127.0.0.1:5001` |
| `rkkvm-video` | UVC capture, HTTP/WS, audio and virtual-media control, web, auth, admin, GPIO | `0.0.0.0:8080` |
| `uac_capture` | userspace UAC audio capture (spawned on demand by `rkkvm-video`/S53, writes a FIFO) | none |

`rkkvm-video` forwards control commands to `rkkvm-hid` over local TCP.

## Layout

```
src/    hid_ffs.c          FunctionFS HID gadget source
        video_uvc.c        UVC capture, HTTP/WS, web/auth/admin/GPIO/audio/virtual media
        uac_capture.c      userspace UAC audio capture (isochronous)
        gpioscan.c         GPIO short/jumper scanner
        usbdump.c          USB descriptor dump tool
deploy/ rkkvm.html         main UI   admin.html admin console   login.html login page
        S49kvmgpio.sh      configure jumper GPIO at boot
        S51rkkvm-hid.sh / S52rkkvm-video.sh / S53rkkvm-audio.sh   service autostart
        kvm.conf           example GPIO config
        audio.conf         example audio source config (UAC / ALSA)
        gpiotest.sh        single-pair GPIO short detection
tools/  kvmctl.py          host-side HID test client
cmake-armv7-musl.cmake     cross-compile toolchain file
build.sh                   one-shot cross build
```

## Build

Requires Linux with `cmake`, `make`, and `curl`. `build.sh` downloads the Bootlin armv7-eabihf (musl)
toolchain, cross-compiles libusb, libuvc, and libjpeg-turbo (all static), and then builds the programs:

```bash
./build.sh
# outputs: src/rkkvm-hid  src/rkkvm-video  src/gpioscan  src/uac_capture  src/usbdump
```

## Deployment

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

Open `http://<board>:8080/`; unauthenticated requests redirect to `/login`. The default account is
`admin` / `admin`; change it after the first login. To restore the default password, delete
`/userdata/passwd` and restart the service.

### Cross-subnet access (optional)

If the board is on a private subnet (for example 192.168.50.0/24), forward ports on the gateway or
terminate TLS with a reverse proxy:

```
firewall-cmd --permanent --zone=public --add-forward-port=port=18080:proto=tcp:toport=8080:toaddr=<board>
```

Using Caddy (HTTPS; Keyboard Lock requires a secure context):

```
https://<gateway>:18443 {
    tls internal
    # the admin "Service manager" reaches the board's svcmgr via a same-origin path,
    # avoiding mixed-content blocking on an HTTPS page
    handle_path /svcmgr/* {
        reverse_proxy <board>:8083
    }
    handle {
        reverse_proxy <board>:8080 { flush_interval -1 }
    }
}
```

## HTTP API

All endpoints require login.

| Path | Description |
|---|---|
| `/` `/admin` `/login` `/logout` | UI / admin / login / logout |
| `/stream` `/snapshot` | MJPEG stream / single JPEG frame |
| `/vws` | WebSocket MJPEG video (low latency) |
| `/aws` | WebSocket audio playback (`?src=alsa` for the board source) |
| `/status` | status (resolution, frame rate, bitrate, GPIO, power) |
| `/formats` | modes supported by the capture device (JSON) |
| `/setres?w=&h=&fps=` | switch resolution and frame rate |
| `/setenc?mode=&quality=` | passthrough / software re-encode |
| `/msd?card=\|file=\|off=\|ro=` | virtual media (block device / image / off / read-only) |
| `/gpio?n=&act=read\|high\|low\|pulse&ms=` | GPIO control |
| `/gpiocfg?power=&reset=&status=` | save jumper GPIO config |
| `/ws` | WebSocket low-latency keyboard/mouse control |
| `/passwd` (POST `old=&new=`) | change login password |
| `/admin/exec` (POST `cmd=`) | run a shell in admin (root) |

## Audio

- This board's kernel does not provide `snd-usb-audio`; the capture device's HDMI audio is read via
  userspace UAC (libusb isochronous).
- `uac_capture` reads PCM from the capture device into the FIFO `/run/rk.pcm`; `S53rkkvm-audio.sh` can
  also use `arecord` to read the board sound card into `/run/rk2.pcm`.
- The source is selected in `/userdata/audio.conf`; the browser plays it over WebSocket `/aws`
  (`/aws?src=alsa` uses the board source).

## Virtual media

- Exposes a block device or image file to the target through the gadget `mass_storage` function:
  `/msd?card=1` (auto-select the first available block device), `/msd?file=/userdata/msd.img`,
  `/msd?off=1` (off), `/msd?ro=1` (read-only).
- Requires kernel `usb_f_mass_storage` (built into this board). A sample 8 MB FAT image is
  `/userdata/msd.img`.
- Adding or removing gadget functions at runtime requires care; see "Known limitations".

## Service manager (optional)

- The admin console's "Service manager" tab manages other daemons (such as `frpc` and `easytier`)
  through the **svcmgr** API; each column can be local or a custom address with an API token.
- svcmgr is now a standalone project: <https://github.com/Nacano12345/rk-svcmgr>
  (`make CROSS=arm-linux- STATIC=1`).
- On the board: binary `/userdata/svcmgr`, autostart `/etc/init.d/S60svcmgr.sh`, token
  `/userdata/services/token`, instance DB `/userdata/services/services.conf`.
- Under the HTTPS admin, it is reached via the gateway Caddy `/svcmgr/*` same-origin reverse proxy;
  see the Caddy configuration above.

## Related projects

- [rk-svcmgr](https://github.com/Nacano12345/rk-svcmgr): the generic multi-instance service manager
  extracted from this project's admin console (single-file C, HTTP API and built-in web UI).

## GPIO jumper

- On the NAND version of the TL3506-MiniEVM, EXPORT(J2) cannot be used as GPIO: its FSPI pins are the
  SPI NAND signals, which the manual explicitly marks as unusable on the NAND version. Miswiring can
  cause a crash or even data corruption.
- A workable option is to reuse the UART0 debug header J4 (pin1=3V3, pin2=GND, pin3/4 = UART0 TX/RX =
  `gpio22`/`gpio23`) as GPIO for the power/reset jumper (exportable via `/sys/class/gpio`, verified
  working). Doing so disables the debug serial console.
- Always isolate with a transistor or optocoupler before connecting to the target's power button;
  do not connect directly.
- `gpioscan` and `gpiotest` help probe pins and confirm shorts.

## Known limitations

- This board's kernel does not provide `uvcvideo`, `usb_f_hid`, or `snd-usb-audio`; video, HID, and
  USB audio are implemented in userspace. It still requires `FunctionFS`, `libusb` (usbfs), `configfs`,
  `usb_f_mass_storage`, and similar support, all of which are present.
- Do not unbind the ffs gadget's UDC at runtime; this kernel tends to hang on that operation. Reboot
  the board after updating the program.
- Sessions are held in memory; you must log in again after a reboot (see the reset procedure above).
- Browsers require a user gesture before audio playback starts; sound begins after the first page click.
- The admin console executes arbitrary commands as root and therefore has broad privileges. Use it
  only on a trusted network and change the default password promptly.

## Acknowledgements

Parts of this project's design were informed by the following open-source projects:

- PiKVM: <https://github.com/pikvm/pikvm> (<https://pikvm.org>)
- OneKVM: <https://github.com/mofeng-git/One-KVM>

## License

[MIT](LICENSE) © 2026 Nacano12345
