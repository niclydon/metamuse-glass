# Milestones

The build ran against a written brief with one deliberate cliff in the middle. Each milestone below lists what "done" meant and the evidence on file. The builder's full log, including the dead ends, is [RESULTS.md](RESULTS.md).

## M0 — Build pipeline (no hardware)

Reproducible pipeline: unpack the XE24 factory image, keep the stock kernel byte-for-byte (hash-pinned), stage the Alpine initramfs with our tools, repack inside the 8 MiB boot ceiling. Evidence: `build/build-m0.sh`, the image info output reproduced in RESULTS.md, static userland checks.

## M1 — First boot

`fastboot boot` of the M0 image on the physical unit: our init runs, the framebuffer draws the first Prism card on the prism. Evidence: [screenshots/m1-prism-framebuffer.png](screenshots/m1-prism-framebuffer.png), [screenshots/card-1-m1-prism-kernel.png](screenshots/card-1-m1-prism-kernel.png). Nothing was flashed; a power cycle returned the unit to stock.

## M2 — Network

Wi-Fi association on the 2.4 GHz network, DHCP lease, and an HTTPS request to a public endpoint succeeding on the modern CA store. Evidence: [screenshots/card-2-m2-wifi-tls13.png](screenshots/card-2-m2-wifi-tls13.png), ping and TLS results in RESULTS.md.

## M3 — Pairing (the go/no-go)

The Muse phone app pairing handshake, completed over BLE from the 3.4 kernel, after the BlueZ GATT path failed and the raw-HCI + L2CAP-shim path replaced it. Had this failed, the overlay's architecture would have changed; the brief required a stop-and-report rather than a workaround of unknown character. It did not fail. Evidence: the over-the-air pairing trace in RESULTS.md, [screenshots/card-3-m3-muse-ble-pair.png](screenshots/card-3-m3-muse-ble-pair.png).

## M4 — Voice

A live, end-to-end encrypted `musegadget voice-call` from the glasses: spoken question in, spoken reply out through the bone-conduction transducer, the Prism HUD live throughout, via the `glass-wav` resampling bridge. Evidence: [audio/m4-nova-question-24k.wav](audio/m4-nova-question-24k.wav), [audio/m4-nova-reply-24k.wav](audio/m4-nova-reply-24k.wav), [audio/m4-nova-full-turn-24k.wav](audio/m4-nova-full-turn-24k.wav), [screenshots/card-5-nova-voice-call.png](screenshots/card-5-nova-voice-call.png), and the frame-captured [screenshots/nova-voice-call-live.gif](screenshots/nova-voice-call-live.gif).

## M5 — Camera and the animated HUD

A camera-to-chat turn — the physical camera button snaps a frame, the photo uploads into the active Muse conversation, and the assistant's spoken description streams back in MP3 with real-time lip-sync (the `glass-wav` bridge decodes MP3 in-process via minimp3; the Ducati coprocessor's firmware is fed through its legacy loader in the first second of boot) — then the display work: telemetry-rich HUD cards, full-screen animated avatar modes with amplitude-driven lip-sync, and touchpad/button mode switching. Evidence: [screenshots/card-10-nova-camera-hud.png](screenshots/card-10-nova-camera-hud.png), [screenshots/card-11-nova-camera-pip.png](screenshots/card-11-nova-camera-pip.png), the live capture [screenshots/nova-camera-chat-live.gif](screenshots/nova-camera-chat-live.gif), [audio/m5-nova-photo-inspection-reply.wav](audio/m5-nova-photo-inspection-reply.wav), the card series in [screenshots/](screenshots/) (cards 4–9), and a montage of the on-device captures on Mux: [player.mux.com/lyqjE7U5dS3wIptPiF1YHMsHGsMhjkIdxUMGFj79VNA](https://player.mux.com/lyqjE7U5dS3wIptPiF1YHMsHGsMhjkIdxUMGFj79VNA).

## After 1.0.0 — firmware 0.2.x

The 1.0.0 release was the gadget. The 0.2.x firmware line turned it into a platform; the session record is in [narrative/](narrative/) and RESULTS.md.

### Control plane (0.2.0–0.2.1)

`glass-agentd`, an authenticated HTTP daemon on the glasses (screenshots, telemetry, archived voice/camera turns, controls for cards, brightness, audio routing, camera, voice, and updates), wrapped by `mcp/glass_mcp.py` — 19 MCP tools by 0.2.1, including `glass_camera_record` (1–15 s clips), `glass_prism_record`, `glass_switch_os`, `glass_ota_update`, and `glass_reboot`. SSH hardened to keys-only at the same time.

### Dual-boot, thermal pass, untethered boot (0.2.0–0.2.1)

A second, local system co-resides on the same hardware layer behind a stage-2 chooser (`rootfs-overlay/etc/glass/hud`): hot-swap in under 200 ms, summoned by a two-finger 2-second touchpad hold so picking the glasses up by the temple arm can never trigger it. Cached framebuffer diffing, adaptive polling, and an ondemand 300 MHz idle floor took idle HUD CPU from 13% to 2% (board ~4 °C cooler). The rootfs and boot image were then written to internal storage over Wi-Fi with SHA-256 readback verification: the glasses cold-boot standalone, no cable, no fastboot. A glasshub don/doff pass sleeps the prism when the glasses are off-head.

### Realtime voice and the settings card (0.2.3)

Voice went fully realtime: microphone and reply stream simultaneously (`POST /voice/realtime`), with transcripts rendering on the prism word by word. The glasses also gained their own settings card — volume, brightness, audio output, desk sleep, and the OS switch from the temple touchpad, with the physical back button as its shortcut — and a camera/USB-mux fix so shooting a photo while docked no longer drops the USB data lines.
