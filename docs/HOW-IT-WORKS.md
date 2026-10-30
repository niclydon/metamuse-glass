# How it works

The stack, layer by layer, with the three problems that actually decided whether the project lived.

## The stack

- **Boot chain (Google's, untouched):** mask ROM → X-Loader → U-Boot-based bootloader → kernel. No public source exists for the Glass bootloaders, so this layer is treated as fixed. Only the boot partition's contents are ours.
- **Kernel (Google's, untouched):** the stock XE24 kernel, Linux 3.4.94, extracted from the factory image and verified by SHA-256 (`83476052…`) before every repack. Its own command line already enables a serial console on ttyO2.
- **Userland (ours):** Alpine Linux armv7 (musl, BusyBox) as an initramfs, assembled on the build host with apk in `--no-scripts` mode so no ARM code runs during the build, then checked statically (every ELF ARM EABI5 hard-float, every interpreter resolvable inside the root). No systemd; a BusyBox init and rc scripts run the show.
- **UI (ours):** native C programs writing the framebuffer at /dev/fb0 (the Himax HX7309 LCoS is a plain DSS framebuffer device; nothing in its path needs Android). The HUD program renders the milestone cards, call state, telemetry, and the animated full-screen modes, plus an on-device settings card (volume, brightness, audio output, desk sleep, OS switch) driven from the temple touchpad, with the back power button as its shortcut.
- **Voice (Meta's SDK, our plumbing):** the Muse Gadget SDK's Linux client, `musegadget`, hand-installed for a non-systemd armv7 system, plus the wrappers in `rootfs-overlay/usr/sbin/` (`glass-muse-voice`, `glass-muse-camera`) and the C bridges in `src/`. Since firmware 0.2.3 the voice path is fully realtime: microphone and reply stream simultaneously, and the transcripts render on the prism word by word as they are spoken.

## Hard problem 1: a modern userland on a 3.4 kernel

Mostly a solved problem by the time we got here — the glass-xec-mainline project had already proven Alpine on this kernel — with two wrinkles of our own. The musegadget client needs Python 3.9+ and modern TLS; both fit, because Alpine's musl userland asks little of the kernel. Native tools are compiled with a 32-bit `time_t` ABI (`-U_TIME_BITS -U_FILE_OFFSET_BITS`) to match what the old kernel and the stock libraries expect. The whole boot image must fit 8,388,608 bytes, the size of the boot partition; the build refuses anything larger.

## Hard problem 2: Bluetooth pairing (the go/no-go)

The Muse pairing handshake is a BLE GATT exchange with the phone app, and the first attempt failed the way the brief feared: BlueZ's GATT server on a 3.4 kernel would not carry it. What worked, in `src/ble_server.py` and `src/glass-ble-shim.c`:

- **Raw HCI advertising.** When the BlueZ path stalls, the pairing service falls back to the four standard Bluetooth 4.0 HCI LE commands (Set Advertising Parameters, Set Advertising Data, Set Scan Response Data, Set Advertise Enable) issued directly, broadcasting the MuseGadget service UUID and manufacturer data exactly as the phone app expects.
- **An L2CAP address-type shim.** A small LD_PRELOAD-style shim corrects the address-type handling between the SDK's expectations and what this kernel's Bluetooth stack presents, with the ATT MTU pinned (`ExchangeMTU = 23`) to what the old stack reliably supports.
- With those two in place, the over-the-air trace runs clean: subscription to notifications, the encrypted pairing exchange, and `pairing_confirmed` from the app. Pairing state persists on the device; pairing binds to the device that pairs, so this all happens on the Glass itself.

The lesson worth publishing: the silicon was never the problem. The BCM4330 does BLE fine. The gap was a decade of userspace Bluetooth assumptions, and it closed at the HCI layer, below all of them.

## Hard problem 3: audio at the wrong sample rate, on the wrong ABI

The SDK's voice client is strict: WAV in and out, PCM16LE mono at exactly 24 kHz, no resampling on its side. The Glass codec path runs at 48 kHz stereo, and the 3.4 kernel's ALSA ioctl numbers predate the modern ABI. `src/glass-wav.c` bridges both gaps in C: it speaks the old `SNDRV_PCM_IOCTL_*` values directly, upsamples the 24 kHz mono reply to 48 kHz stereo for the TWL6040's bone-conduction output, and decimates 48 kHz stereo capture to the 24 kHz mono the SDK wants from the microphone. Replies play with no underruns; the recorded turns are in `docs/audio/`.

## Networking

Wi-Fi is wpa_supplicant over nl80211 (the kernel has WEXT compiled out), with the BCM4330 firmware and board calibration placed where the driver looks for them (`firmware/README.md`). A current CA store rides in the initramfs, which is what lets a 2014 kernel make 2026 TLS connections. Credentials are never baked into images in this repo: the build takes the SSID and passphrase from environment variables and stores only the derived PSK, and the on-device `glass-wifi` tool prompts rather than embeds.

## The control plane

Once the gadget worked, the glasses grew the interface everything else on the desk already had. `glass-agentd` (`rootfs-overlay/usr/sbin/glass-agentd`) is a small HTTP daemon on the device serving JSON and PNG endpoints: status and telemetry, prism screenshots, archived voice and camera turns, and controls for cards, brightness, audio routing, camera stills and clips, voice turns, OS switching, and over-the-air boot updates with SHA-256 readback. On Wi-Fi every route requires a Bearer token (held on the device at `/etc/prism/token`, mode 0600); SSH is keys-only via `rootfs-overlay/etc/glass/sshd`. A `/mirror` page streams the prism at a few frames per second for a browser.

On the host, `mcp/glass_mcp.py` wraps the daemon in 19 MCP tools (`glass_status`, `glass_health_audit`, `glass_screenshot`, `glass_set_card`, `glass_set_hud`, `glass_set_brightness`, `glass_set_audio_route`, `glass_chime`, `glass_voice_turn`, `glass_camera_snap`, `glass_camera_record`, `glass_prism_record`, `glass_ask_nova`, `glass_notify_hud`, `glass_fetch_archives`, `glass_switch_os`, `glass_ota_update`, `glass_exec`, `glass_reboot`), so an MCP-capable assistant can check the glasses' health, look through the prism, snap a photo, or reboot them into the second, local system they dual-boot via the stage-2 chooser (`rootfs-overlay/etc/glass/hud`, hot-swap under 200 ms, guarded by a two-finger hold).

Site-specific values are never baked in: the unit's Wi-Fi address, its Bluetooth address, the second system's host, and the agent token all arrive through environment variables (see [BUILDING.md](BUILDING.md)).
