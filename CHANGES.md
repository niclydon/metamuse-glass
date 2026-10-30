# metamuse-glass changes

## 1.2.0 — 2026-10-30 — Sync to the 23-tool build (firmware 0.2.3)

- The `glass-mcp` server grows from 19 to 23 tools: `glass_settings`, `glass_sensors`, `glass_speak`, and `glass_wifi_heal`, backed by four new `glass-agentd` endpoints (`/settings`, `/sensors`, `/speak`, `/wifi/heal`). The firmware version label is unchanged (0.2.3); this sync brings the public tree to the current shipped build.
- Wi-Fi self-heal: a `glass-wifi heal` subcommand resets the BCM4330 SDIO bus when the radio hangs (traced to a `dhd_check_hang` after camera captures), the `rcS` watchdog now verifies gateway reachability instead of trusting the interface address, and the camera path heals automatically after a shot when the network is unreachable.
- Scrub discipline maintained on the delta: the new code arrived with the author's LAN gateway as a hardcoded fallback in three places; in this tree the gateway is derived from the route table at runtime (overridable via `GLASS_GATEWAY`) and no network addresses, identifiers, or pairing material are baked in.
- Docs: README, HOW-IT-WORKS, NARRATIVE, and MILESTONES updated to the 23-tool set; RESULTS gains the session record for the new tools and the SDIO auto-heal. The dock narrative chapter remains unpublished: it documents the second system's dock daemon, which is not part of this repo.

## 1.1.0 — 2026-10-30 — Launch polish: sync to firmware 0.2.3

- Code synced to the shipped firmware 0.2.3 state: camera-to-chat as shipped, the `glass-agentd` control plane, the 19-tool `glass-mcp` server (`mcp/`), `glass-camera-record`, the stage-2 dual-boot chooser (`etc/glass/hud`), keys-only SSH (`etc/glass/sshd`), realtime streaming voice, the on-device settings card, and the thermal and untethered-boot work. New narrative chapters under docs/narrative/ carry the session record, and docs/RESULTS.md is extended through the 0.2.x sessions.
- Scrub pass on everything synced: no LAN addresses, device identifiers, or pairing material are baked in anywhere; site-specific values (the unit's Wi-Fi address, its Bluetooth address, the second system's host, the agent token) are supplied through environment variables — see docs/BUILDING.md. This pass also removes a Bluetooth-address default that 1.0.0 had shipped in `glass-bt-up`.
- Docs: the README feature set is brought current (realtime voice, settings card, the MCP tool list, dual-boot, untethered boot, thermal numbers) with the article cover at docs/images/cover.png; MILESTONES gains the 0.2.x record, and its broken milestone-tour video link is replaced with the published Mux montage of the same captures; NARRATIVE's 2014 provenance is corrected (the unit was collected in person in San Francisco on February 28, 2014; the April 2014 shipment was a $225 prescription frame).
- Gallery: card-10 and card-11 (camera), the live camera-chat capture, and the article's Bluetooth and pickup-day cards are added to docs/screenshots/. The settings-card capture is not included: its telemetry corner shows the unit's Wi-Fi address.

## 1.0.1 — 2026-10-30 — Naming cleanup

- The overlay codename used during the first hours of this repo ("Vox") is retired; the firmware is referred to descriptively, and Prism is the hardware's device name, not a platform brand. Early HUD captures in docs/screenshots/ still show the VOX label the first builds rendered on the prism; they are kept as the historical record.

## 1.0.0 — 2026-10-30 — Public release

- First public release of the MetaMuse firmware for Prism: a non-Android Linux system for Google Glass Explorer Edition that pairs with the Muse app over BLE and places live voice calls into a Muse chat, verified end to end on a 2014 Explorer unit (milestones M0–M5, builder's log in docs/RESULTS.md).
- Included: the build pipeline (build/), the native sources (src/: framebuffer HUD, ALSA resampling bridge, BLE pairing service and L2CAP shim), the rootfs overlay and on-device tools (rootfs-overlay/), sprite tooling (tools/), firmware provenance and hashes (firmware/), screenshots and recorded voice turns (docs/screenshots/, docs/audio/).
- Documentation: narrative, architecture (docs/HOW-IT-WORKS.md), milestones, building instructions, prior art and credits. Planning and code created with Google's Gemini 4 Argon via Antigravity, directed and hardware-tested by the owner.
- Pre-publication scrub: no credentials, pairing identifiers, or private infrastructure details are included; Wi-Fi and SDK credentials are supplied at build/run time by design. In three screenshots the unit's Bluetooth MAC and its LAN address are blacked out, and the milestone-tour video, which cycles those frames, is not included. Firmware binaries are not distributed; they are extracted from Google's factory image and verified against the hashes in firmware/README.md.
- Not included: the stock kernel or any Google/Broadcom binaries, the Muse Gadget SDK itself (installed at run time), and the sprite source art (the builder tool that packs it is included; its inputs are set by environment variable).
