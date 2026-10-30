# Notices

This repository contains original work under the MIT License (see LICENSE), and incorporates or builds upon the following third-party material:

- **glass-xec-mainline** — Copyright (c) 2026 Lasimeri, MIT License. Platform-layer concepts and groundwork (userland assembly, firmware placement, device-tree research) derive from this project: https://github.com/Lasimeri/glass-xec-mainline. Its full license text is available at that repository.
- **minimp3** — `src/minimp3.h`, by lieff, released under CC0/public domain terms as stated in the file header.
- **Meta Muse Gadget SDK** — the `musegadget` Linux client is Meta's, under its own license (Apache-2.0), and is installed at build/run time; it is not vendored in this repository. https://github.com/facebookincubator/muse-gadget-sdk
- **Alpine Linux** — the userland base is assembled from Alpine packages under their respective licenses at build time; no Alpine binaries are distributed here.
- **Google** — the stock XE24 kernel and the BCM4330 firmware blobs are Google's/Broadcom's, extracted by the builder from Google's factory image; they are not distributed in this repository (see firmware/README.md). Google Glass, Android, and related marks are Google's.
- **Meta, Muse** — product names used to describe interoperability; this is an independent hobbyist project, not affiliated with or endorsed by Meta or Google.
