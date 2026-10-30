# Prior art and credits

Checked October 29–30, 2026, before and during the build. This page exists so the claims elsewhere in the repo are exactly as strong as the evidence, no stronger.

## Who did what first

- **First Linux port to Glass, period:** the postmarketOS project, 2017 (device codename `google-glass`), which demoed a desktop environment on the Explorer. The port is archived in their tree today and likely broken, but it was first, and later work (including the October 2026 projects) builds on its kernel-config provenance.
- **First modern non-Android system for Glass:** [glass-xec-mainline](https://github.com/Lasimeri/glass-xec-mainline) by Lasimeri — an Alpine Linux userland on Google's stock XE24 kernel, plan dated October 3, 2026, in daily use by October 7, 2026, with display, Wi-Fi, audio, touchpad, battery, and camera working, plus a mainline-kernel port in progress. MIT licensed. This project learned its platform layer from that work — the userland assembly pattern, the firmware placement, the device-tree groundwork — and says so wherever it matters.
- **The only custom ROM of the Android era:** a 2016 AOSP 5.1.1 build from XDA (jtxdriggers). The complete custom-firmware record for Explorer Glass is one ROM long.
- **The standing community:** AnotherGlass (a MyGlass replacement, still maintained), the GlassKit preservation project, and the OpenMyGlass revival effort (October 2026; companion apps and ROM tooling, built with heavy AI-agent involvement, paused before running on hardware as of this writing).

## What this project adds, stated precisely

- As of the October 2026 check, **no published work** had turned Google Glass into a voice endpoint for a modern AI assistant through a public gadget SDK with Bluetooth LE pairing to a phone app. Meta's Muse Gadget SDK opened publicly on October 2, 2026; its own guide ecosystem notes that nothing in it targets glasses. This firmware is, to the best of a sourced search, the first published port of that SDK to Glass — phrased that way on purpose.
- **No published operating system for Glass authored by an AI coding agent** was found. Its planning and code were created with Google's Gemini 4 Argon under a human-written brief and milestone contract, with a human directing, flashing, and testing on the hardware. That division of labor is the claim, and the repo's docs (the brief-driven milestones, the builder's results log) are its evidence.

## Standing on

Google's published Glass kernel sources and factory images; the Alpine Linux project; Meta's Muse Gadget SDK (the `musegadget` Linux client); the minimp3 decoder (public domain/CC0, `src/minimp3.h`); and the glass-xec-mainline platform work above. Third-party notices are collected in [../NOTICE.md](../NOTICE.md).
