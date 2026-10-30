# First light on the prism, a 3.4-kernel Bluetooth miracle, and Nova speaking through the bone

October 30, 2026. The third narrative in the `glass-os` record, written the night the physical Google Glass Explorer Edition (`[unit serial redacted]`) came out of the drawer, booted our non-Android Linux system in RAM, passed the M3 Bluetooth Low Energy pairing gate that the research docs had marked as the project's single largest technical risk, and spoke Nova's voice through the temple bone-conduction transducer while streaming live subtitles and a 20-segment VU meter onto the 640×360 LCoS prism.

---

## Chapter 1 — The 8 MiB needle and the 32-bit `time_t` trap (M0)

Before the glasses ever touched a USB cable on `the build host`, the M0 build pipeline (`muse-gadget-overlay/build/build-m0.sh`) had to thread a hard physical needle: the `notle` (`glass_1`) partition table gives `boot` exactly `8,388,608` bytes (`8.00 MiB`). Google's unmodified XE24 `3.4.94` kernel (`zImage`, SHA-256 `83476052edc649505c480700d88e1a7d743232d6181b970556722cf4bdb79def`) already consumes `5,090,464` bytes (`4.85 MiB`) of that budget, leaving `3,298,144` bytes (`3.14 MiB`) for the entire initramfs.

Three facts discovered by inspecting the decompressed XE24 `vmlinux` image shaped how that `3.14 MiB` was spent:

1. **The stock kernel only compiles in the `gzip` (`inflate`) initramfs decompressor.** `unxz`, `unlzma`, `unlzo`, and `bunzip2` are compiled out of the kernel's early boot path. Yet M2 requires modern TLS 1.3 and a current Mozilla CA bundle (`/etc/ssl/certs/ca-certificates.crt`), which in Alpine 3.21 pulls in `libcrypto.so.3` and `libssl.so.3`. Compressed with `gzip -9`, the initramfs overflowed the 8 MiB partition by 284 KB. The fix was two-stage compression: `build-m0.sh` packs `libcrypto.so.3` and `libssl.so.3` into `/usr/lib/openssl.tar.xz` using `xz --check=crc32 --arm --lzma2=preset=9e` inside the `gzip`-compressed cpio archive, and `/init` extracts it into RAM tmpfs via `busybox tar -xJf` on boot. That brought `out/boot-m0.img` down to `8,089,600` bytes (`7.71 MiB` — `299,008` bytes under the ceiling).
2. **Modern 32-bit ARM toolchains break 3.4 kernel structs via 64-bit `time_t`.** Both GCC 13 (`arm-linux-gnueabihf-gcc`) and Alpine 3.21 `musl` enable `_TIME_BITS=64` by default on 32-bit ARM, inflating `sizeof(struct input_event)` from `16` bytes to `24` bytes and redirecting `ioctl()` to `__ioctl_time64` (which shifts `SNDRV_PCM_IOCTL_SYNC_PTR` from `0xc0844123` to `0xc0884123`). Every on-device C binary in `muse-gadget-overlay/src/` (`fb-hello`, `glass-wav`, `glass-fb`, `glass-console`) is compiled with `-U_TIME_BITS -U_FILE_OFFSET_BITS` so its structs and ioctl numbers match the 2012 OMAP4 `3.4.94` kernel ABI byte-for-byte.
3. **Runtime-writable `bcmdhd` and `bluetooth` sysfs parameters.** Decompressing `vmlinux` confirmed that `bcmdhd` defaults to `/system/vendor/firmware/fw_bcmdhd.bin` and `/system/etc/wifi/bcmdhd.cal` (both exposed as writable sysfs nodes under `/sys/module/bcmdhd/parameters/`), and that the kernel's Bluetooth subsystem hides BLE behind `/sys/module/bluetooth/parameters/enable_le`, which defaults to `N` (`0`) until `Y` is written at runtime.

---

## Chapter 2 — Waking a sleeping Explorer and first light on the prism (M1)

When the unit was first plugged into `the build host`, `lsusb` reported `Bus 001 Device 011: ID 18d1:9001 Google Inc. Glass 1`, and `adb devices` was empty: the unit was booted into stock XE24 with USB Debugging disabled and its screen asleep. Holding the power button for 15 seconds while 5V USB VBUS is applied does not hard-off the `twl6030` PMIC — USB power keeps the rail awake. Unplugging USB, holding the power button to shut down, holding down the physical camera button on the top frame, and plugging the USB cable back in dropped the OMAP4430 cleanly into `NOTLE0201` fastboot mode:

```text
Bus 001 Device 013: ID 18d1:dead Google Inc.
product: notle
serialno: [unit serial redacted]
version-bootloader: NOTLE0201
unlocked: yes
```

The bootloader was already unlocked (`unlocked: yes`), meaning no `fastboot oem unlock` (and no `userdata` wipe) was required. Following the hard safety rule in `docs/milestones/M1-first-card.md`, we touched zero flash partitions and booted `out/boot-m0.img` directly into SDRAM:

```text
$ ./muse-gadget-overlay/build/glass-ctl.sh boot-m0
downloading 'boot.img'...
OKAY [  0.296s]
booting...
OKAY [  0.056s]
finished. total time: 0.352s
```

Four seconds later, the Glass enumerated on `the build host` as an RNDIS Ethernet gadget (`18d1:4ee2`, `the host RNDIS interface`), assigned `172.16.42.1/24` on the Glass and `172.16.42.2/24` on `the build host`, and accepted root SSH logins with `582 MB` of usable RAM (`1 GB` Explorer Edition after OMAP4 carveouts) and battery telemetry reporting `4.199 V` charging cleanly at `27.7 °C`.

On `/dev/fb0`, `fb-hello` (`muse-gadget-overlay/src/fb-hello.c`) executed the two OMAP4 DSS workarounds required on the stock `3.4.94` kernel:
1. Toggling `/sys/devices/platform/omapdss/manager2/panel-notle-dpi/enabled` `0 -> 1` so `panel-notle-dpi` does not skip LCoS LED illumination after bootloader handoff.
2. Writing `fix.smem_start` (`0x9f800000`) directly to `DISPC_GFX_BA0` (`0x48041080`) and `DISPC_GFX_BA1` (`0x48041084`) and latching `DISPC_GO_LCD2` (`0x20`) in `DISPC_CONTROL2` (`0x48041238`) via `/dev/mem`, bypassing the broken `FBIOPAN_DISPLAY` path in `dsscomp`.

```text
00:00:01 glass-os-rcS: USB RNDIS gadget active on rndis0 (172.16.42.1/24)
00:00:01 glass-os-rcS: resetting panel-notle-dpi (0 -> 1) to enable LCoS LED illumination
00:00:02 glass-os-rcS: panel-notle-dpi enabled (brightness 120)
00:00:02 glass-os-rcS: fb-hello: /dev/fb0 640x360 32bpp stride=640 smem=0x9f800000 len=2764800 kernel=3.4.94-01990-g1cee760235d ram=582MB board="OMAP4430 R0004" dispc=mapped
```

The first pixel-accurate `640×360` screenshot was pulled directly from `/dev/fb0` over SSH (`./muse-gadget-overlay/build/glass-ctl.sh screenshot`) and saved to `muse-gadget-overlay/docs/screenshots/card-1-m1-prism-kernel.png`.

---

## Chapter 3 — 2.4 GHz Wi-Fi, synchronous NTP, and TLS 1.3 (M2)

With `496 MB` of RAM free inside the initramfs tmpfs, we streamed the `25.46 MiB` Alpine 3.21 `armv7` M2/M3/M4 runtime bundle (`out/m3-overlay.tar.gz`, built by `muse-gadget-overlay/build/build-m3-bundle.py`) directly into `/` over USB RNDIS.

Running `/usr/sbin/glass-wifi up` brought up the Broadcom `BCM4330` on `wlan0` using `wpa_supplicant -Dnl80211` (since `CONFIG_WIRELESS_EXT` is compiled out of `notle_defconfig`) with `fw_bcmdhd.bin` and `bcmdhd.cal` pointed at `/system/etc/firmware/`. `wlan0` associated on 2.4 GHz Channel 3 (`2422.0 MHz`, `-35 dBm`, `58.5 MBit/s MCS 6`) and acquired `the unit's Wi-Fi address/24` via DHCP.

The very first `curl -I https://example.com` test returned a classic embedded-Linux failure:

```text
curl: (60) SSL certificate problem: certificate is not yet valid
```

Two bugs had conspired:
1. In `glass-wifi`, `ntpd -n -q -p pool.ntp.org` had been backgrounded (`&`), so `glass-wifi up` returned while the OMAP4 RTC was still sitting in 2018.
2. Under BusyBox `ash`, `echo -n > /etc/resolv.conf` in the DHCP hook wrote a literal `-n` as the first line of `/etc/resolv.conf`.

Fixing `glass-wifi` (`muse-gadget-overlay/rootfs-overlay/usr/sbin/glass-wifi`) to strip any stray `-n` line and run the first `ntpd -n -q -p pool.ntp.org` pass synchronously stepped the clock to `2026-10-30 04:52:44 UTC`. Immediately afterward, both `curl` and Python 3.12's `urllib.request` validated modern certificates over TLS 1.3:

```text
HTTP/2 200
status: 200 TLS: TLSv1.3
```

---

## Chapter 4 — The M3 Go/No-Go: making BlueZ 5.79 and `musegadget` pair on a 2012 3.4.94 kernel

Every piece of prior research (`docs/research/03-bare-metal-and-rewrite-depth.md`, `briefs/MUSE-GADGET-PROTOTYPE-BRIEF.md`) flagged **M3 — Meta `musegadget` BLE GATT peripheral pairing on the stock `3.4.94` kernel** — as the project's explicit go/no-go gate. Linux 3.4's Bluetooth Management (`mgmt`) socket speaks version `1.1`, predating almost the entire modern BlueZ BLE peripheral API.

When we enabled BLE (`echo Y > /sys/module/bluetooth/parameters/enable_le`) and tried to bring up `/dev/ttyO1` and run `musegadget pair`, we hit three distinct kernel-era walls — and solved all three in userspace without touching a single byte of the kernel:

### Wall 1: `hciattach bcm43xx` and the `BCM4330B1.hcd` filename
Stock Android's `bluetooth.default.so` loads `/system/vendor/firmware/bcm4330.hcd`. Standard BlueZ `hciattach /dev/ttyO1 bcm43xx 3000000 flow - [Glass BT address]` queries the controller's local name over HCI (`BCM4330B1`) and searches `/lib/firmware/` for `<local_name>.hcd`. Staging `/system/etc/firmware/bcm4330.hcd` at `/lib/firmware/BCM4330B1.hcd` allowed `hciattach` to upload the Broadcom patchram, switch `/dev/ttyO1` to `3,000,000` baud, and bring `hci0` `UP RUNNING` (`HCI Version: 4.0 (0x6) Revision: 0x14f6`, `USI BCM4330B1 37.4 MHz Class 1.5 ExtLNA WLCSP`).

### Wall 2: Missing `org.bluez.LEAdvertisingManager1` (`MGMT_OP_READ_ADV_FEATURES`)
When `bluetoothd 5.79` started on kernel `3.4.94`, it registered `org.bluez.GattManager1` on `/org/bluez/hci0` (`GATT application registered: /org/musegadget/app0`), but omitted `org.bluez.LEAdvertisingManager1` because kernel `3.4.94` lacks `MGMT_OP_READ_ADV_FEATURES` (`0x003d`, introduced in Linux 4.1). Furthermore, `dbus-python` threw `ValueError: Unable to guess signature from an empty dict` on `RegisterApplication(APP_PATH, {})`.

In `muse-gadget-overlay/src/ble_server.py`, we:
1. Passed explicit D-Bus signatures (`dbus.ObjectPath(APP_PATH), dbus.Dictionary({}, signature="sv")`) to `RegisterApplication`.
2. Added an HCI LE advertising fallback (`_hci_le_advertise_start` / `_hci_le_advertise_stop`) that programs the BCM4330 directly via the four standard Bluetooth 4.0 HCI LE commands (`0x08 0x0006` LE Set Advertising Parameters, `0x08 0x0008` LE Set Advertising Data with the 128-bit Muse service UUID `7fdd3d1c-38ea-46cf-8b46-314ecf5f240c` and manufacturer data `0xffff: 00`, `0x08 0x0009` LE Set Scan Response Data with the local name `MuseGadget<device suffix>`, and `0x08 0x000a` LE Set Advertise Enable), and re-enables advertising whenever a BLE client disconnects.

Immediately, `the build host` (`[build host BT address]`) discovered the Glass over the air:

```text
[NEW] Device [Glass BT address] MuseGadget<device suffix>
  UUIDs: 7fdd3d1c-38ea-46cf-8b46-314ecf5f240c
  ManufacturerData Key: 0xffff (65535) Data: 00
  RSSI: -62 dBm
```

### Wall 3: `l2cap_sock_getname()` `BDADDR_BR_EDR` (0) and the 23-byte `L2CAP_CID_ATT` MTU
When `the build host` connected over BLE to `[Glass BT address]`, the connection succeeded at the HCI LE link layer, but `bluetoothd` logged:

```text
bluetoothd[1073]: src/device.c:att_connect_cb()ATT connect
< ACL Data TX: Handle 68 flags 0x00 dlen 9
      ATT: Error Response (0x01)
        Error: Request Not Supported (0x06)
        Exchange MTU Request (0x02) on handle 0x0000
```

And when `the build host` subscribed to `0xFD03` notifications and wrote a command to `0xFD02`, `musegadget` received the command and sent its response, but `bt_att_chan_write()` inside `bluetoothd` dropped the socket with `EMSGSIZE` (`Message too large`).

Inspecting the Linux 3.4 kernel source (`net/bluetooth/l2cap_sock.c` and `net/bluetooth/l2cap_core.c`) and `bluetoothd` (`src/adapter.c`, `src/device.c`, `src/shared/gatt-server.c`) revealed two exact root causes:
1. **`sockaddr_l2.l2_bdaddr_type` was added in Linux 3.5.** On kernel `3.4.94`, `l2cap_sock_getname()` writes 12 bytes (`l2_family`, `l2_psm`, `l2_bdaddr`, `l2_cid`) and leaves `l2_bdaddr_type = 0` (`BDADDR_BR_EDR`). When `bluetoothd`'s `att_connect_cb()` called `bt_io_get(io, ..., BT_IO_OPT_DEST_TYPE, &dst_type)`, it saw `dst_type == BDADDR_BR_EDR (0)` and attached the LE ATT connection as a **BR/EDR ATT connection** (where `ATT_EXCHANGE_MTU_REQ` is forbidden by the Bluetooth Core Spec!).
2. **Kernel `3.4.94` clamps `chan->omtu` on `L2CAP_CID_ATT` (`4`) to `L2CAP_DEFAULT_MIN_MTU = 23` bytes.** Any `write()` larger than 23 bytes to the ATT L2CAP socket returns `EMSGSIZE`.

We wrote a 62-line `LD_PRELOAD` library, `muse-gadget-overlay/src/glass-ble-shim.c` (`libglass_ble_shim.so`), which intercepts `getsockname()`, `getpeername()`, `accept()`, and `accept4()` and sets `l2_bdaddr_type = BDADDR_LE_PUBLIC (1)` whenever `l2_family == AF_BLUETOOTH (31)` and `l2_cid == 4` (`L2CAP_CID_ATT`). And we set `ExchangeMTU = 23` in `/etc/bluetooth/main.conf` alongside `_ASSUMED_MTU = 23` in `src/ble_server.py`. Because `musegadget`'s framing layer (`ble_framing.py`) already chunks notifications to `mtu - 3 = 20` payload bytes (`23` ATT bytes total), setting the MTU to `23` made every GATT discovery PDU (`<= 23B`) and every `musegadget` notification chunk (`23B`) fit inside the kernel's 23-byte `L2CAP_CID_ATT` channel without a single `EMSGSIZE`.

We then ran a complete over-the-air BLE pairing verification from `the build host` (`[build host BT address]`) to the Glass (`[Glass BT address]`):
- GATT primary service discovery found `0000fd5f-0000-1000-8000-00805f9b34fb` and both characteristics (`0xFD02` RX, `0xFD03` TX).
- Subscribing to `0xFD03` and sending `get_device_info` returned the reassembled JSON response over BLE notifications (`device_id: <device-id>`, `protocol_version: 5`).
- Executing the v5 ECDH P-256 + HKDF-SHA256 + AES-GCM-256 handshake (`pairing_client_hello` -> `pairing_ready` -> encrypted `pairing_client_finished` -> decrypted `pairing_confirmed`) and an encrypted post-pairing `wifi_scan` command (`wifi_scan_result`, 9 networks returned) succeeded end-to-end over the air.

**M3 Verdict: GO.**

---

## Chapter 5 — Reverse-engineering `audio.primary.glass_1.so` and `libtinyalsa.so` (M4)

With BLE pairing and Wi-Fi working, M4 required two audio paths on the OMAP4 ABE + TWL6040 ("Notle") sound card: bone-conduction transducer playback and digital microphone capture, bridged to `musegadget`'s strict `24,000 Hz` 16-bit mono PCM16LE WAV format.

When `glass-wav` (`muse-gadget-overlay/src/glass-wav.c`) first tried to open `/dev/snd/pcmC0D0c`, the kernel rejected `SNDRV_PCM_IOCTL_HW_PARAMS` with `EINVAL` (`Invalid argument`). Rather than guessing at OMAP4 ABE constraints, we inspected Google's stock `/mnt/system/lib/hw/audio.primary.glass_1.so` and `/mnt/system/lib/libtinyalsa.so` directly from the Glass's read-only `/dev/mmcblk0p8` (`system`) partition:

1. **Exact PCM devices and parameters:**
   - Playback uses `/dev/snd/pcmC0D0p` (`00-00: Multimedia`) at `48,000 Hz`, `2` channels, `S16_LE`, `period_size = 1056`, `periods = 4`.
   - Voice capture uses `/dev/snd/pcmC0D2c` (`00-02: Voice`), NOT `pcmC0D0c`, at `48,000 Hz`, `2` channels, `S16_LE`, `period_size = 1056`, `periods = 4`.
   - In `libtinyalsa.so`'s `pcm_open()`, `SNDRV_PCM_HW_PARAM_SAMPLE_BITS` (`8`) = `16`, `SNDRV_PCM_HW_PARAM_FRAME_BITS` (`9`) = `32`, `SNDRV_PCM_HW_PARAM_CHANNELS` (`10`) = `2`, `SNDRV_PCM_HW_PARAM_PERIODS` (`15`) = `4`, and `SNDRV_PCM_HW_PARAM_RATE` (`11`) = `48000` must all be marked with `integer = 1`, with `SNDRV_PCM_HW_PARAM_PERIOD_SIZE` (`13`) set to `min = 1056` before calling `SNDRV_PCM_IOCTL_HW_REFINE` (`0xc25c4110`, `604` bytes) and `SNDRV_PCM_IOCTL_HW_PARAMS` (`0xc25c4111`, `604` bytes).
2. **Exact 24-entry TWL6040 + OMAP4 ABE mixer table (`glass-audio-route`):**
   - Stock `audio.primary.glass_1.so` routes the internal **bone-conduction transducer** through `DL1 Mixer Multimedia=1`, `DL1 Media Playback Volume=120`, `SDT DL Volume=120`, `DL1 PDM Switch=1`, `Earphone Playback Switch=1`, and `Earphone Playback Volume=15`.
   - It routes the **micro-USB mono earbud** via `Headset Left Playback='HS DAC'`, `Headset Right Playback='HS DAC'`, `Headset Playback Volume=14`, and switches `/sys/devices/platform/usb_mux.0/mux_mode` to `audio` when `/sys/devices/platform/usb_mux.0/headset` reads `1`.
   - It routes the on-frame **digital microphone (`DMic0`)** via `MUX_UL10='DMic0L'`, `MUX_UL11='DMic0R'`, `MUX_VX0='DMic0L'`, `MUX_VX1='DMic0R'`, `DMIC1 UL Volume=146,146`, `Voice Capture Mixer Capture=1`, and `AUDUL Voice UL Volume=120`.

Updating `src/glass-wav.c` and `rootfs-overlay/usr/sbin/glass-audio-route` with those exact parameters made both bone-conduction playback and digital microphone capture work on the very first run (`48,000` frames captured at `24,000 Hz` mono PCM16LE, `47,882 / 48,000` non-zero samples, `peak=1156`, `rms=208.7`).

---

## Chapter 6 — Nova speaking through the bone-conduction transducer and live prism subtitles

With M1–M4 verified, we tested an end-to-end `musegadget voice-call` from the Glass itself over Wi-Fi (`the unit's Wi-Fi address`) into Meta's Muse cloud:

1. **Identity-binding finding:** Transplanting an active `pairing.json` into `/var/lib/musegadget/pairing.json` on the Glass while keeping the Glass's own `identity.json` (`the Glass node`) succeeded without error: `muse_api.fetch_vms_with_status` returned `HTTP 200` (`1` leased VM), and `LinkSession` established the encrypted Noise WebSocket session (`Noise session established`) and opened `hatch.voice.session`.
2. **Leading-silence VAD requirement:** Our first WAV call sent speech at `t = 0.0s` immediately after `hatch.voice.session` and timed out with a 44-byte `reply.wav` because the server-side VAD requires ~0.5–1.0 seconds of initial audio stream to arm before `input_audio_buffer.speech_started` triggers. Prepending `1.0s` of leading silence triggered `input_audio_buffer.speech_started`, `conversation.item.input_audio_transcription.delta`, `hatch.voice.turn_started`, `response.output_audio_transcript.delta`, and `continuous_audio_video.delta` immediately.
3. **Live Prism Subtitles + VU Meter + Bone-Conduction Playback:** We added Card 4 (`VOX // NOVA VOICE CALL [LIVE]`) to `src/fb-hello.c`, rendering the live session state (`STAT`), caller transcript (`YOU`), streaming 4-line Nova response (`NOVA`), and a 20-segment color-coded VU meter driven by the real-time `pcm_rms()` of the `24 kHz` audio stream:
   - **Prompt:** *"Oh in one sentence describe a lighthouse."*
   - **Nova's spoken reply (`7.32s`, `175,680` frames @ `24,000 Hz` mono PCM16LE, `peak=15538`, `rms=1492.8`):** *"It's a tall tower throwing a steady sweeping beam across dark water to warn ships and point them to safe passage."*
   - As `glass-wav play /tmp/muse-voice/reply.wav` played Nova's reply through `/dev/snd/pcmC0D0p`, the owner heard Nova's voice clearly through the Google Glass bone-conduction transducer while watching the live subtitles and VU meter on the prism.

All 48+ live `/dev/fb0` frames across both the **Full-Body Chibi Nova** modes (`Card 7` and `Card 8`, built from Glance's full-body animations) and the **Full-Face Chibi Nova** modes (`Card 5` and `Card 6`, built from AIPI's close-up face animations), the Synaptics RMI4 Multi-Touch Protocol A touchpad gesture switcher (`SWIPE+` / `SWIPE-` / `TAP` / `TAP2` + `KEY_CAMERA`), and the `24 kHz` question/reply WAV files were captured off the Glass and committed to the repository:
- **Full-Body Chibi Nova + Muse Gadget Split HUD (`Card 7`):** `muse-gadget-overlay/docs/screenshots/card-8-nova-fullbody-hud.png`
- **Full-Body Chibi Nova Stand-Up Stage (`Card 8`, Glance Mode):** `muse-gadget-overlay/docs/screenshots/card-9-nova-fullbody-standup.png`
- **Multi-Mode Full-Body + Full-Face Live Lip-Sync GIF & MP4:** `muse-gadget-overlay/docs/screenshots/nova-fullbody-live.gif` and `muse-gadget-overlay/docs/screenshots/nova-fullbody-live.mp4`
- **Full-Face Chibi Nova + Muse Gadget Split HUD (`Card 5`):** `muse-gadget-overlay/docs/screenshots/card-6-nova-fullscreen-hud.png`
- **Full-Face Centered Chibi Nova Portrait (`Card 6`):** `muse-gadget-overlay/docs/screenshots/card-7-nova-fullscreen-portrait.png`
- **Full-Face Chibi Nova Live Lip-Sync GIF & MP4:** `muse-gadget-overlay/docs/screenshots/nova-chibi-fullscreen-live.gif` and `muse-gadget-overlay/docs/screenshots/nova-chibi-fullscreen-live.mp4`
- **Diagnostic Chibi Nova Stage (`Card 4`):** `muse-gadget-overlay/docs/screenshots/card-5-nova-chibi-live.png`
- **Live Nova Voice Call Screenshot, GIF & MP4:** `muse-gadget-overlay/docs/screenshots/card-5-nova-voice-call.png`, `muse-gadget-overlay/docs/screenshots/nova-voice-call-live.gif`, and `muse-gadget-overlay/docs/screenshots/nova-voice-call-live.mp4`
- **Preserved 24 kHz WAV Audio Files:** `muse-gadget-overlay/docs/audio/m4-nova-question-24k.wav`, `m4-nova-reply-24k.wav`, `m4-nova-full-turn-24k.wav`

See `CHANGES.md` (`0.2.0`) and `muse-gadget-overlay/RESULTS.md` for the chronological diff summary and operator commands.

> Executed by Antigravity (model: gemini-argon)
