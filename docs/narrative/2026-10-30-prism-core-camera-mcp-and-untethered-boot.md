# Camera-to-chat, cutting the USB cord, the Prism Core MCP control plane, and flashing eMMC over Wi-Fi

October 30, 2026. The fourth narrative in the `glass-os` record, picking up directly after `docs/narrative/2026-10-30-first-light-and-first-voice.md`. Once M1–M4 proved that Google Glass Explorer Edition (`[unit serial redacted]`, board `"notle"` / `glass_1`, OMAP4430 ES2.2) could boot our Alpine Linux initramfs in RAM, pair with the Meta Muse app over Bluetooth Low Energy on a 2012 `3.4.94` kernel, and render both the Glance Full-Body Chibi Nova (`240×320`) and AIPI Full-Face Chibi Nova (`240×240`) with real-time bone-conduction lip-sync, five questions remained before the unit could leave the bench cable for good:

1. Could the physical camera button (`KEY_CAMERA`, keycode `212`) snap a photo, display it on the prism alongside Chibi Nova, upload it into the active Meta Muse conversation thread, and speak Nova's visual description back through the bone-conduction transducer?
2. Would the unit survive being unplugged from `the build host` — automatically switching the micro-USB mux from RNDIS Ethernet (`usb`) to the mono earbud (`audio`), keeping its BLE pairing credentials across NTP clock steps, and archiving every voice and camera turn locally on the glasses?
3. Could we expose an authenticated over-the-wire control plane (`glass-agentd` + `glass-mcp`) and a live 640×360 prism mirror (`/mirror`) so agents and the second system can inspect, record, and drive the glasses over Wi-Fi?
4. Could the **MetaMuse** (`fb-hello`) and **NexusOS** (`prismd`) overlays co-reside on a shared **Prism Core** with a Stage-2 on-prism boot chooser and wireless OTA updates, without burning through the 570 mAh battery or cooking the OMAP4430 at idle?
5. Could the entire system be persisted onto the eMMC `userdata` (`/dev/mmcblk0p10`) and `boot` (`/dev/mmcblk0p7`) partitions wirelessly over Wi-Fi so a cold reboot no longer required `fastboot boot` over USB?

Every one of those five arcs landed on hardware during the same early-morning session.

---

## Chapter 1 — Camera-to-Muse-Chat (`M5`) and MP3 TTS decoding on the OMAP4 ABE

Early research (`docs/research/03-bare-metal-and-rewrite-depth.md`) had treated the 5 MP OmniVision `OV5680` camera as the hardest peripheral to preserve without Android because Google's camera HAL talks to TI's Ducati Cortex-M3 coprocessor over `rpmsg-omx` (`/dev/rpmsg-omx1`) using proprietary SYS/BIOS firmware (`/system/vendor/firmware/ducati-m3-core0.xem3`). Inspecting `upstream/glass-xec-mainline/src/glass-camera.c` showed that `/dev/rpmsg-omx1` can be driven directly from a standalone C binary (`/usr/bin/glass-camera`) once two conditions are met:

1. **Early boot firmware loading (`t ≈ 7.8s`):** The stock `3.4.94` kernel's `omap-rproc` driver requests `ducati-m3-core0.xem3` via the legacy sysfs firmware loader (`/sys/class/firmware/ducati-m3-core0.xem3/{loading,data}`) roughly `7.8` seconds after kernel start and times out after `60` seconds (`rproc0: request_firmware failed: -11`). Because the unit had booted in RAM before `/mnt/system/vendor/firmware/ducati-m3-core0.xem3` was symlinked into `/lib/firmware/`, later attempts to unbind/rebind `omap-rproc.0` hung in `wait_for_completion()` inside `rproc_unregister()`. We updated both `muse-gadget-overlay/rootfs-overlay/init` and `muse-gadget-overlay/rootfs-overlay/etc/glass/rcS` to mount `/dev/mmcblk0p8` (`PARTNAME=system`) read-only at `/mnt/system`, symlink `ducati-m3-core0.xem3`, `ducati-m3-core0-2gb.xem3`, and `tesla-dsp.xe64T` into `/lib/firmware/`, and spawn a 60-second background sysfs firmware feeder at `t < 1s` of boot.
2. **Bridging NV12 camera frames and MP3 TTS streams inside `glass-wav`:**
   - `/usr/bin/glass-camera -y -w 640 -h 360 -n 6` outputs raw `640×360` `NV12` (`Y` plane `230,400` bytes + interleaved `UV` plane `115,200` bytes = `345,600` bytes/frame), taking 6 frames (`0.2s`) so `OV5680` auto-exposure and auto-white-balance settle. Converting `640×360` NV12 to BGRA32 and PNG scanlines in pure Python on the Cortex-A9 took ~3.8 seconds; writing `cmd_nv12_conv` in C inside `muse-gadget-overlay/src/glass-wav.c` (`glass-wav nv12-conv /tmp/snap.nv12 640 360 /run/glass/camera-frame.bgra /tmp/snap.scanlines`) dropped the conversion time to **~2 ms**.
   - On the cloud side, `musegadget`'s `LinkSession.send_message(thread_id, text, attachments=[image_path])` uploads the PNG via `LinkSession.upload_file()` (`POST /files`) and attaches `items=[{"type": "image", "file_id": ...}]` to `POST /chat/stream`. However, `LinkSession.stream_tts(message_id)` (`POST /tts/stream`) returns **24 kHz mono MP3 (`audio/mpeg`)**, whereas `glass-wav play` previously only accepted `RIFF/WAVE` PCM16LE files. Rather than pulling in `ffmpeg` on the Glass, we vendored single-header `minimp3.h` (`muse-gadget-overlay/src/minimp3.h`) directly into `muse-gadget-overlay/src/glass-wav.c`. When `glass-wav play` sees an MP3 stream (anything without a `RIFF` header), it decodes each MP3 frame in-process, resamples to `48,000 Hz` stereo S16LE for `/dev/snd/pcmC0D0p`, and writes the real-time speech envelope (`rms * 3`) to `/run/glass/nova-rms` every `40 ms` so Chibi Nova's mouth animates in sync with the spoken MP3 reply.

On the prism (`muse-gadget-overlay/src/fb-hello.c`), we added two camera UI surfaces and wired the physical top-frame camera button (`EV_KEY`, `code == 212` `KEY_CAMERA`, `value == 0`) to invoke `/usr/sbin/glass-muse-camera`:
- **Card 7 (`FULL-BODY + HUD` with Camera Viewfinder Inset):** Whenever `/run/glass/camera-active` is `1`, `draw_camera_inset()` renders a `144×81` cyan-bordered thumbnail of `/run/glass/camera-frame.bgra` in the top-right corner of the conversation HUD (`muse-gadget-overlay/docs/screenshots/card-10-nova-camera-hud.png`).
- **Card 9 (`CAMERA SNAP + CHIBI PIP`):** Renders the captured `640×360` photo full-screen across the prism with a bottom-left `100×120` Full-Body Chibi Nova picture-in-picture overlay and a semi-transparent subtitle bar at the bottom (`muse-gadget-overlay/docs/screenshots/card-11-nova-camera-pip.png`).

Running `/usr/sbin/glass-muse-camera` end-to-end uploaded `/tmp/glass-photo.png` to the conversation thread, received the assistant's message, and streamed `38,016` bytes of MP3 TTS (`muse-gadget-overlay/docs/audio/m5-nova-photo-inspection-reply.mp3` and `.wav`) through the bone-conduction transducer:

> *"You're running a sleek custom interface with a little cosmic character on the left and a status screen showing that Nova is online and ready to talk."*

---

## Chapter 2 — Cutting the USB cord: the `usb_mux` `"None"` quirk and the pre-NTP token-refresh trap

Unplugging the Glasses from `the build host` exposed two subtle hardware/software interactions that only happen off the bench cable:

### 1. `/sys/devices/platform/usb_mux.0/headset` returns `"None"`, not `"0"`
Google Glass shares its micro-USB receptacle between USB data/charging (`mux_mode = usb`) and the custom micro-USB mono earbud (`mux_mode = audio`) via a Fairchild FSA9480 USB switch (`usb_mux.0`). Our initial `glass-audio-route` checked `[ "$HEADSET_IN" != "0" ]`. On the stock `3.4.94` kernel, when no analog ID resistor is latched, `/sys/devices/platform/usb_mux.0/headset` returns the literal string `"None"`, and when a micro-USB earbud is plugged in while untethered (`twl6030_usb/online == 0`), `mux_mode` stayed stuck on `usb` unless switched.

We updated `muse-gadget-overlay/rootfs-overlay/usr/sbin/glass-audio-route` so `auto` mode checks `[ "$HEADSET_IN" != "None" ] || [ "$USB_ONLINE" = "0" ]`, flipping `/sys/devices/platform/usb_mux.0/mux_mode` to `audio` whenever the USB cable is unplugged from a host computer while keeping both the `Headset Playback` DAC (`14`) and `Earphone Playback` / `DL1 PDM Switch` bone-conduction paths active simultaneously. A lightweight background watcher (`/usr/sbin/glass-usb-audio-watch`, polling `twl6030_usb/online` every 2 seconds) re-runs `glass-audio-route` the moment the USB cable is plugged or unplugged.

### 2. The `Service._maybe_refresh()` clock-skew token wipe
When we started `musegadget -v run --run-as root` as a standing daemon, it crashed with `RuntimeError: Not paired — run 'musegadget pair' first` and **deleted `/var/lib/musegadget/pairing.json`**.

Inspecting `musegadget/service.py` revealed why: `Service._maybe_refresh()` checked:

```python
if not force and time.time() - self._pairing.access_token_saved_at < TOKEN_REFRESH_INTERVAL:
    return
```

Because `pairing.json` had been saved when the OMAP4 RTC was at an earlier epoch (or copied across an NTP step), `time.time() - access_token_saved_at` exceeded `10,800` seconds even though `access_token` itself was still completely valid (`fetch_vms_with_status` returned `HTTP 200`). `_maybe_refresh()` unconditionally called `client.refresh_device_token(refresh_token)`, which returned `HTTP 401`, and the `401` handler executed `delete_pairing(self._config.data_dir)` — wiping the valid `pairing.json`!

We restored `pairing.json` from our encrypted local backup and patched `Service._maybe_refresh()` in `musegadget/service.py` so token refresh is only attempted when `force=True` (i.e., when `fetch_vms_with_status` actually returns `401`). Immediately afterward, `musegadget -v run --run-as root` stayed up continuously (`registered with the Muse` as `the Glass node`).

### 3. On-device untethered turn archiving
Because untethered voice turns happen away from `the build host`, we updated `muse-gadget-overlay/rootfs-overlay/usr/sbin/glass-muse-voice` so every voice turn automatically archives its inputs and outputs under `/var/lib/musegadget/recordings/turn-<epoch>/` (`question.wav`, `reply.wav`, `transcript.txt`, and `prism.bgra`), and updated `muse-gadget-overlay/build/glass-ctl.sh` to auto-fallback from `172.16.42.1` (USB RNDIS) to `the unit's Wi-Fi address` (Wi-Fi) whenever the USB cable is unplugged.

---

## Chapter 3 — The Prism Core control plane (`glass-agentd`, `glass-mcp`, 10s camera clips, and `/mirror`)

To make the Glasses a first-class citizen of the estate over Wi-Fi (`the unit's Wi-Fi address`), we built a two-part control plane:

1. **On-device HTTP daemon (`muse-gadget-overlay/rootfs-overlay/usr/sbin/glass-agentd`, port `8080`):**
   A single-file Python `ThreadingHTTPServer` listening on `0.0.0.0:8080` that converts `/dev/fb0` (`640×360` BGRA32) to PNG in-memory (`GET /screenshot.png`, ~40 ms over Wi-Fi), serves raw BGRA frames (`GET /screenshot.bgra`), reports full telemetry (`GET /status` and `GET /health-audit`), downloads archived recordings (`GET /download?path=...`), controls cards/HUD/brightness/audio/chimes, triggers voice turns (`POST /voice-turn`), camera snaps (`POST /camera-snap`), 1–15s video clips (`POST /camera-record`), direct cloud chat + spoken TTS (`POST /ask-nova`), live OS hot-swapping (`POST /os/switch`), and verified OTA updates (`POST /ota/apply`).
2. **Host MCP server (`muse-gadget-overlay/mcp/glass_mcp.py`, 18 tools):**
   Speaks JSON-RPC 2.0 over stdio and auto-discovers the Glasses on Wi-Fi (`the unit's Wi-Fi address:8080`) or USB (`172.16.42.1:8080`):
   - **Core & Telemetry:** `glass_status`, `glass_health_audit`, `glass_set_card`, `glass_set_hud`, `glass_set_brightness`, `glass_set_audio_route`, `glass_chime`, `glass_exec`
   - **Capture & Video:** `glass_screenshot`, `glass_camera_snap`, `glass_camera_record` (1–15s clips, default 10s), `glass_prism_record` (live prism HUD video recording), `glass_fetch_archives`
   - **Composite AI & HUD:** `glass_ask_nova` (text or camera+text prompt -> Meta Muse chat -> spoken MP3 TTS on bone conduction with Chibi lip-sync), `glass_notify_hud`, `glass_voice_turn`
   - **Dual-Boot & OTA:** `glass_switch_os`, `glass_ota_update`

### Solving the 10-second clip bandwidth bottleneck over 2.4 GHz Wi-Fi
Our first test of `glass_camera_record` in fallback mode captured 15 raw `640×360×4` BGRA frames (`13.8 MB` for 3 seconds, or `27.6 MB` for 10 seconds at 3 fps), which timed out a 30-second HTTP download over the BCM4330's 2.4 GHz Wi-Fi link. Piping the raw frame stream through `gzip -1` on the Glass inside `muse-gadget-overlay/rootfs-overlay/usr/sbin/glass-camera-record` (`${OUT_BASE}.bgra.gz`) compressed a 10-second, 30-frame clip from `27,648,000` bytes down to **`1,390,769` bytes (`20×` smaller)**, downloading over Wi-Fi and muxing via `ffmpeg` into `.mp4` and `.gif` in under 3 seconds. When `/dev/rpmsg-omx1` is active after cold boot, `glass-camera-record` uses the Ducati M3 hardware H.264 encoder (`/usr/bin/glass-camera -w 640 -h 360 -r 15 -k 1500 -n 150`, `OMX.TI.DUCATI1.VIDEO.H264E`) directly.

### Live Prism Mirror (`/mirror`)
To let **NexusOS** running on the host (or any browser/WebView on the LAN) display and control what the wearer sees on the Google Glass prism in real time, we added `GET /mirror` (`GET /mirror` on the daemon (token-authenticated)) directly to `glass-agentd`. It streams `/screenshot.png` at ~3–4 fps (~25 KB/frame) inside a pixel-accurate `640×360` viewport with a live telemetry header (`OS`, `BAT % / mV`, `AUDIO mux`, `CARD`, and live `YOU` / `NOVA` transcript) and one-click buttons for `Tap Temple (Voice)`, `Swipe <- / ->`, `Snap Camera -> Nova`, and hot-swapping between `Vox (Nova)`, `NexusOS (Prism)`, and `Boot Chooser`.

---

## Chapter 4 — Stage-2 Dual-Boot Chooser, the 1-finger temple-grab bug, and the 300 MHz thermal pass

In `the companion project (not included in this repo)`, the parallel **NexusOS** track had built `/usr/bin/prismd` (`128,424` bytes, static ARMv7 binary, SHA-256 `d582bd1922a615308d65ce41aeed5bdd6649fa49be671b9d1cb824c33f19d336`), including a graphical Stage-2 boot chooser (`prismd --choose 5`, which displays `Which system? tap: Vox (Muse, Nova) / swipe: Nexus OS` on the prism) and `/etc/glass/hud`.

Because both **MetaMuse (`fb-hello`)** and **NexusOS (`prismd`)** share the exact same stock XE24 `3.4.94` kernel and Alpine ARMv7 base initramfs, we installed `/usr/bin/prismd`, `/etc/prism/prismd.conf`, `/etc/prism/token` (`0600`), and `/etc/glass/hud` onto the live unit and updated `/etc/inittab` to respawn `/etc/glass/hud`. Calling `glass_switch_os(target="chooser" | "nexus" | "vox")` over Wi-Fi verified all three screens on the physical prism:
- **Stage-2 Boot Chooser (`prism-choose`):** `muse-gadget-overlay/docs/screenshots/card-12-dual-boot-chooser.png`
- **NexusOS (`prism-linking` -> the second system's link service):** `muse-gadget-overlay/docs/screenshots/card-13-nexusos-prism-live.png` (capture not included in this repo)
- **MetaMuse (`Card 7: FULL-BODY + HUD`):** `muse-gadget-overlay/docs/screenshots/card-8-nova-fullbody-hud.png`

### Catching a bug in `/etc/glass/hud` and the 1-finger temple-grab switch
Two bugs surfaced during live testing and were fixed immediately:
1. When `prismd --choose` was running and an external `glass_switch_os("vox")` call wrote `vox` to `/run/glass/choice` and killed `prismd`, `/etc/glass/hud` resumed after `choice=$(/usr/bin/prismd --choose ...)` with an empty `$choice`, defaulted to `choice=nexus`, and overwrote `/run/glass/choice` with `nexus`! Updating `/etc/glass/hud` to check `after_choice=$(cat /run/glass/choice)` after `prismd --choose` exits fixed the race completely.
2. We initially bound a `>= 1.5s` 1-finger hold on the temple touchpad in `src/fb-hello.c` to open the Stage-2 Boot Chooser. The moment the owner unplugged the Glasses from `the build host` and picked them up by the right temple arm (`07:49:49 UTC`), his thumb naturally rested on the capacitive touchpad for `> 1.5s`, triggering `prismd --choose`, which timed out 5 seconds later into `NexusOS` (`prism-linking`)! When the owner asked *"can you see what's on screen on the glasses now?"*, `glass_screenshot` over Wi-Fi immediately revealed `PRISM Linking` on the screen. We switched the unit back to `vox` over Wi-Fi and pushed an OTA update to `src/fb-hello.c` requiring a **two-finger hold (`tp_max_fingers >= 2 && dt >= 2000`)** to summon the boot chooser from the touchpad so holding the glasses by the temple arm never switches OSes by accident.

### Thermal, CPU & battery optimization pass
Running `glass_health_audit` before unplugging revealed that `fb-hello` was consuming **`13%` CPU continuously** even when Nova was completely idle (`READY`, `rms == 0`), keeping the OMAP4430 `interactive` governor pinned at **`600 MHz`** (`-208 mA` draw, `40.7 °C` PCB temperature).

Inspecting `muse-gadget-overlay/src/fb-hello.c` showed why: every `80 ms` (`12.5 fps`), the render loop called `memcmp(fbm, backbuf, 921600)` where `fbm` is `/dev/fb0` — uncached write-combining VRAM (`ioremap_wc`), which is ~25× slower to read from the ARM CPU than normal cached SDRAM — and executed `ioctl(fb, FBIOPAN_DISPLAY, &var)` even when not a single pixel had changed.

We made three changes:
1. Added a cached L1/L2 SDRAM shadow buffer (`uint32_t *prevbuf = calloc(1, map_len)`) in `src/fb-hello.c` so `memcmp(prevbuf, backbuf, map_len)` runs entirely in cached RAM, and `/dev/fb0` (`memcpy(fbm, ...)` + `FBIOPAN_DISPLAY` + `DISPC_GO_LCD2`) is only touched on frames where pixels actually changed.
2. Made the `poll(pfds, nfds, poll_ms)` timeout adaptive: **`75 ms` (`13.3 fps`)** when Nova is speaking (`rms > 50`), listening/thinking (`nova-state != READY`), or showing a toast, and **`600 ms` (`1.67 fps`)** when idle in `READY` state (while `poll()` on `/dev/input/event*` still wakes in **`< 1 ms`** on any touchpad tap, swipe, or camera button press).
3. Switched the OMAP4 CPU frequency governor in `etc/glass/rcS` to `ondemand` (`300 MHz` idle floor) and stopped `bluetoothd -d` once `/var/lib/musegadget/pairing.json` is present.

| Metric (`glass_health_audit`) | Before Pass | After Pass |
|---|---|---|
| **OMAP4 CPU Clock (Idle on Card 7)** | `600 MHz` (`interactive`) | **`300 MHz`** (`ondemand` minimum clock) |
| **`fb-hello` Idle CPU Usage** | `13%` | **`2%`** (**`93%` total CPU idle**) |
| **Available RAM (`MemFree + Cached`)** | `525.0 MB` / `582.0 MB` | **`525.5 MB` available** (`90.3%` free/reclaimable) |
| **Battery & PCB Temperatures** | `23.5 °C` bat / `40.7 °C` PCB | **`22.7 °C` bat / `36.9 °C` PCB** |

### Wi-Fi SSH and `glass-agentd` token hardening
Following `etc/glass/sshd` in the companion project, we replaced `dropbear -B` (which allowed blank-password root logins during bench bring-up) with `/etc/glass/sshd` (`dropbear -F -E -R -s -g -K 5 -p 22`), enforcing Ed25519 public-key authentication only (`-s -g`) and terminating the unauthenticated `telnetd` daemon unless `/etc/prism/bench` is present. In `glass-agentd`, we added `_check_auth()` requiring `Authorization: Bearer <token>` (or `?token=<token>`) matching `/etc/prism/token` (`0600`) for all Wi-Fi requests outside `/health`. Live probes from `the build host` confirmed `HTTP/1.0 401 Unauthorized` on unauthenticated HTTP requests and `Permission denied (publickey)` on password SSH attempts.

---

## Chapter 5 — Flashing `userdata` and `boot` wirelessly over Wi-Fi

When the owner unplugged the Glasses and asked *"what happens if i reboot them?"*, checking `/dev/mmcblk0p7` over Wi-Fi caught a critical state: because the session had originally started via `fastboot boot out/boot-m0.img` (RAM-only boot per the M1 safety gate), `/dev/mmcblk0p7` still held Google's factory XE24 `boot.img` (`sha256: 2b5a010a8e3d230b0bb85a9892473a6ce4be925f2acebba39878997445478de7`), and `/dev/mmcblk0p10` (`userdata`) had no filesystem yet.

Inspecting `/sys/class/block/mmcblk0p*/uevent` also caught an important partition-numbering detail on `notle` (`glass_1`):
- `mmcblk0p1`: `xloader` (`128 KB` — **never write**)
- `mmcblk0p2`: `bootloader` (`256 KB` — **never write**)
- `mmcblk0p3`: `fpga` (`512 KB` — **never write**)
- `mmcblk0p4`: `bootconfig` (`256 KB`)
- `mmcblk0p5`: `misc` (`256 KB`)
- `mmcblk0p6`: `recovery` (`8,192 KB`)
- **`mmcblk0p7`: `boot` (`8,192 KB` = `8,388,608` bytes)**
- `mmcblk0p8`: `system` (`1,048,576 KB`, stock XE24 `ext4` mounted read-only at `/mnt/system`)
- `mmcblk0p9`: `cache` (`786,432 KB`)
- **`mmcblk0p10`: `userdata` (`13,699,584 KB` = `14.03 GB`)**

We updated `resolve_boot_dev()` in `glass-agentd` to resolve `PARTNAME=boot` dynamically from `/sys/class/block/mmcblk0p*/uevent` (`/dev/mmcblk0p7`). Then, with the Glasses sitting untethered on Wi-Fi (`the unit's Wi-Fi address`) at `98%` battery, we executed the two-stage persistence flow entirely over the air:

1. **Formatted `/dev/mmcblk0p10` (`PARTNAME=userdata`, `14.03 GB`) as `ext4`** using `/mnt/system/bin/make_ext4fs /dev/mmcblk0p10` (`3,424,896` 4 KB blocks, `856,800` inodes), mounted it at `/mnt/userdata`, copied the complete live rootfs (`/bin /etc /home /init /lib /root /sbin /usr /var` — including Python 3.12, `musegadget`, `/var/lib/musegadget/pairing.json`, `sdk_token`, `nova_fullbody.bin`, `nova_sprites.bin`, `/etc/wpa_supplicant/wpa_supplicant.conf`, `/etc/prism/token`, `fb-hello`, `prismd`, `glass-agentd`, `/etc/glass/rcS`, `/etc/glass/hud`, and `/etc/glass/sshd`), and wrote `/mnt/userdata/etc/glass-muse-rootfs` so `/init` automatically `switch_root`s into `/dev/mmcblk0p10` on every cold boot.
2. **Rebuilt `muse-gadget-overlay/out/boot-m0.img` (`8,284,160` bytes, `98%` of the 8 MiB ceiling, SHA-256 `a7dd5e0833d3caa7e68460c0f6bf11215372b698f3129476257ae5ac78472f80`)** and flashed it to `/dev/mmcblk0p7` (`PARTNAME=boot`) wirelessly via `glass_ota_update(target="boot")`. On-device, `glass-agentd` verified the `ANDROID!` header, extracted the embedded `5,090,464`-byte kernel and confirmed its SHA-256 matched stock XE24 `3.4.94` (`83476052edc649505c480700d88e1a7d743232d6181b970556722cf4bdb79def`), wrote the image to `/dev/mmcblk0p7`, and read back all `8,284,160` bytes from `/dev/mmcblk0p7` to verify the flashed SHA-256 matched `a7dd5e0833d3caa7e68460c0f6bf11215372b698f3129476257ae5ac78472f80`.

---

## Chapter 6 — First Cold Reboot off eMMC, the `userdata` Firmware Path Catch, and `glasshub` Off-Head Desk Sleep

We added a 19th MCP tool — **`glass_reboot`** (`POST /reboot` in `muse-gadget-overlay/rootfs-overlay/usr/sbin/glass-agentd`, calling `sync` and `reboot -f` after flushing the HTTP response) — and triggered the unit's first untethered cold reboot off eMMC (`3768952`).

The stock bootloader loaded `boot-m0.img` from `/dev/mmcblk0p7`, `/init` detected `/etc/glass-muse-rootfs` on `/dev/mmcblk0p10` (`12.9 GB` `ext4` mounted at `/`, `250.8 MB` used), and `fb-hello` lit the prism on `Card 7` (`FULL-BODY + HUD`). However, three behaviors surfaced immediately on that first cold boot:
1. `the unit's Wi-Fi address` (`wlan0`) did not come back online over Wi-Fi.
2. Pressing the physical Camera button snapped two photos locally (`/var/lib/musegadget/photos/photo-1542590576.png` and `photo-1542590726.png`) and wrote `SENDING PHOTO TO NOVA...` to the prism HUD, where the status text remained stuck.
3. Tapping the temple touchpad to start a voice turn with Nova did nothing.

### Diagnosing the first cold `switch_root` over USB RNDIS (`172.16.42.1`)
Plugging the USB cable back into `the build host` brought up USB RNDIS (`172.16.42.1` on `the host RNDIS interface`, `1.80 ms` RTT), confirming that `/dev/mmcblk0p10` had mounted as `/` and `rcS` + `dropbear` + `fb-hello` + `glass-agentd` were all running normally. Running `/usr/sbin/glass-wifi up` over SSH printed the exact root cause verbatim:

```text
glass-wifi: missing /system/etc/firmware/fw_bcmdhd.bin
```

Tracing the two failure modes explained all three symptoms at once:
1. **Why `wlan0` didn't join Wi-Fi after `switch_root` (`725b866`):** In `muse-gadget-overlay/build/build-m0.sh`, the Broadcom BCM4330 firmware blobs (`fw_bcmdhd.bin`, `bcmdhd.cal`, `bcm4330.hcd`) are staged in the initramfs at `/system/etc/firmware/`. When we populated `/mnt/userdata` (`/dev/mmcblk0p10`) with `for d in bin etc home lib root sbin usr var; do cp -a "/$d" /mnt/userdata/; done`, the top-level `system` and `vendor` directories from initramfs were not in that loop, and `glass-wifi` only checked `/system/etc/firmware/fw_bcmdhd.bin` rather than falling back to `/mnt/system/vendor/firmware/fw_bcmdhd.bin` and `/mnt/system/etc/wifi/bcmdhd.cal` on `/dev/mmcblk0p8`.
2. **Why `glass-muse-camera` left `SENDING PHOTO TO NOVA...` on screen and `glass-muse-voice` exited before recording (`25358c3`):**
   - With `wlan0` down, `glass-muse-camera` saved the PNG to `/var/lib/musegadget/photos/`, wrote `SENDING PHOTO TO NOVA...` to `/run/glass/nova-reply`, and then failed inside `muse_api.fetch_vms_with_status()` with `set -e` active, exiting before updating `/run/glass/nova-state` or `/run/glass/nova-reply`.
   - Before the reboot, `VM_ID` and `THREAD_ID` had been cached in `/run/glass/voice.env` — which lives on the `/run` RAM `tmpfs` and was cleared on reboot. When `glass-muse-voice --tap` ran with an empty `/run/glass/voice.env`, it attempted a pre-flight `fetch_vms_with_status()` network lookup *before* playing the listening chime (`glass-wav chime start`), failed because `wlan0` was offline (and because its fallback snippet called `config.load_state()` instead of `config.load_json(config.PAIRING_FILE)`), and exited at `[ -z "$VM_ID" ]`.

We fixed all three permanently (`725b866`, `25358c3`):
- Copied `fw_bcmdhd.bin`, `bcmdhd.cal`, and `bcm4330.hcd` into `/system/etc/firmware/` on `/dev/mmcblk0p10` (`userdata`), updated `muse-gadget-overlay/rootfs-overlay/init` to copy `/system` and `/vendor` into `/newroot` before `switch_root` if absent, and updated `muse-gadget-overlay/rootfs-overlay/usr/sbin/glass-wifi` so `FW_BIN` and `FW_CAL` fall back to `/mnt/system/vendor/firmware/fw_bcmdhd.bin` and `/mnt/system/etc/wifi/bcmdhd.cal`.
- Updated `muse-gadget-overlay/rootfs-overlay/usr/sbin/glass-muse-voice` to read and persist `VM_ID` and `THREAD_ID` in `/var/lib/musegadget/voice.env` on eMMC (in addition to `/run/glass/voice.env`) using `config.load_json(config.PAIRING_FILE)`, so touchpad taps start recording immediately across cold reboots without requiring a pre-flight network call.
- Wrapped the cloud upload blocks in both `glass-muse-camera` and `glass-muse-voice` so network failures update `/run/glass/nova-state` (`WIFI OFFLINE`) and `/run/glass/nova-reply` cleanly instead of leaving stale `SENDING PHOTO TO NOVA...` text on the prism.

### `glasshub` Don/Doff Off-Head Detection and Automatic Desk Sleep (`efba2be`)
When the owner asked *"does it handle off head detection? like will it put itself to sleep when it knows it's been placed down on a desk?"*, we inspected the stock XE24 `3.4.94` kernel's `glasshub` driver (`CONFIG_INPUT_GLASSHUB=y`, I2C bus 4 address `0x35`, `/sys/bus/i2c/devices/4-0035/`).

Decompressing the stock kernel image (`zImage`) confirmed that Google's inward-facing IR proximity/wink microcontroller (`glasshub`) exposes `don_doff_enable`, `don_doff` (`1` when worn on the head, `0` when taken off), and `proxraw` (`65535` when off-head on the desk) directly under `/sys/bus/i2c/devices/4-0035/`.

We wired hardware don/doff detection and automatic desk sleep into `muse-gadget-overlay/src/fb-hello.c` and `muse-gadget-overlay/rootfs-overlay/usr/sbin/glass-agentd` (`efba2be`):
1. **Enable `glasshub` on startup:** `ensure_panel_on()` writes `1` to `/sys/bus/i2c/devices/4-0035/don_doff_enable` and initializes `/run/glass/prism-power` to `AWAKE`.
2. **Automatic Off-Head / Desk Sleep (`set_prism_sleep(1)`):** When `don_doff != 1` (off-head) and no touchpad/camera-button input, active voice turn (`LISTENING`/`THINKING`/`SPEAKING`), or MCP wake event has occurred for **`15 seconds` after taking the Glasses off your head** (or **`60 seconds`** if sitting untouched on a desk since boot, configurable via `/run/glass/sleep-timeout-sec`), `fb-hello` blanks `/dev/fb0` (`fb0/blank = 1`), powers off the Himax HX7309 LCoS LED (`panel-notle-dpi/enabled = 0`), writes `SLEEP` to `/run/glass/prism-power`, skips `render_frame()` completely (**`0%` CPU**), and sleeps in `poll(pfds, nfds, 1000)`.
3. **Instant Wake (`set_prism_sleep(0)`):** The prism wakes immediately (`panel-notle-dpi/enabled = 1`, `fb0/blank = 0`, `/run/glass/prism-power = AWAKE`) the moment any of three events occurs:
   - Putting the Glasses back on your head (`don_doff` transitions to `1`, displaying `ON HEAD: PRISM AWAKE`),
   - Touching the temple touchpad or pressing the physical camera button (`poll()` wake on `/dev/input/event*`), or
   - Calling an MCP display tool (`/card`, `/hud`, `/brightness` write `/run/glass/wake`).
4. **Live Verification & 10-Second Wireless Screen Recording:** We flashed the updated `boot-m0.img` (`8,286,208` bytes, SHA-256 `b87397d45c587fe090cd81e3ca5436737c8185b9f4408728a0f4da0e3d740cd9`, stock XE24 `3.4.94` kernel `83476052...`) to `/dev/mmcblk0p7`, synced `/dev/mmcblk0p10`, verified `glass_status` over Wi-Fi (`the unit's Wi-Fi address`, `power_state: "AWAKE"`, `on_head: false`), and recorded a live 10-second (`60 frames @ 6 fps`) prism screen capture over Wi-Fi via `glass_prism_record`.

See `CHANGES.md` (`0.2.0` / `0.2.1`) and `muse-gadget-overlay/RESULTS.md` for the chronological diff summary and tool reference.

> Executed by Antigravity (model: gemini-argon)
