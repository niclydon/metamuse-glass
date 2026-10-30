# How it works

Prism (the base) and Vox (this overlay), layer by layer, with the three problems that actually decided whether the project lived.

## The stack

- **Boot chain (Google's, untouched):** mask ROM → X-Loader → U-Boot-based bootloader → kernel. No public source exists for the Glass bootloaders, so this layer is treated as fixed. Only the boot partition's contents are ours.
- **Kernel (Google's, untouched):** the stock XE24 kernel, Linux 3.4.94, extracted from the factory image and verified by SHA-256 (`83476052…`) before every repack. Its own command line already enables a serial console on ttyO2.
- **Userland (ours):** Alpine Linux armv7 (musl, BusyBox) as an initramfs, assembled on the build host with apk in `--no-scripts` mode so no ARM code runs during the build, then checked statically (every ELF ARM EABI5 hard-float, every interpreter resolvable inside the root). No systemd; a BusyBox init and rc scripts run the show.
- **UI (ours):** native C programs writing the framebuffer at /dev/fb0 (the Himax HX7309 LCoS is a plain DSS framebuffer device; nothing in its path needs Android). The HUD program renders the milestone cards, call state, telemetry, and the animated full-screen modes.
- **Voice (Meta's SDK, our plumbing):** the Muse Gadget SDK's Linux client, `musegadget`, hand-installed for a non-systemd armv7 system, plus the wrappers in `rootfs-overlay/usr/sbin/` (`glass-muse-voice`, `glass-muse-camera`) and the C bridges in `src/`.

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
