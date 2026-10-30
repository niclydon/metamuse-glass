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

A photo-inspection turn (Glass camera → assistant description → spoken reply; [audio/m5-nova-photo-inspection-reply.wav](audio/m5-nova-photo-inspection-reply.wav)), then the display work: telemetry-rich HUD cards, full-screen animated avatar modes with amplitude-driven lip-sync, and touchpad/button mode switching. Evidence: the card series in [screenshots/](screenshots/) (cards 4–9) and the full-motion captures ([screenshots/prism-milestone-tour.mp4](screenshots/prism-milestone-tour.mp4)).
