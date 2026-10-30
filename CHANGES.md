# metamuse-glass changes

## 1.0.1 — 2026-10-30 — Naming cleanup

- The overlay codename used during the first hours of this repo ("Vox") is retired; the firmware is referred to descriptively, and Prism is the hardware's device name, not a platform brand. Early HUD captures in docs/screenshots/ still show the VOX label the first builds rendered on the prism; they are kept as the historical record.

## 1.0.0 — 2026-10-30 — Public release

- First public release of the MetaMuse firmware for Prism: a non-Android Linux system for Google Glass Explorer Edition that pairs with the Muse app over BLE and places live voice calls into a Muse chat, verified end to end on a 2014 Explorer unit (milestones M0–M5, builder's log in docs/RESULTS.md).
- Included: the build pipeline (build/), the native sources (src/: framebuffer HUD, ALSA resampling bridge, BLE pairing service and L2CAP shim), the rootfs overlay and on-device tools (rootfs-overlay/), sprite tooling (tools/), firmware provenance and hashes (firmware/), screenshots and recorded voice turns (docs/screenshots/, docs/audio/).
- Documentation: narrative, architecture (docs/HOW-IT-WORKS.md), milestones, building instructions, prior art and credits. Planning and code created with Google's Gemini 4 Argon via Antigravity, directed and hardware-tested by the owner.
- Pre-publication scrub: no credentials, pairing identifiers, or private infrastructure details are included; Wi-Fi and SDK credentials are supplied at build/run time by design. In three screenshots the unit's Bluetooth MAC and its LAN address are blacked out, and the milestone-tour video, which cycles those frames, is not included. Firmware binaries are not distributed; they are extracted from Google's factory image and verified against the hashes in firmware/README.md.
- Not included: the stock kernel or any Google/Broadcom binaries, the Muse Gadget SDK itself (installed at run time), and the sprite source art (the builder tool that packs it is included; its inputs are set by environment variable).
