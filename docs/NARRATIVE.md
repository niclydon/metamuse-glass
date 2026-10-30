# The drawer, the dead platform, and the new OS

A narrative of the project, written for readers; the technical record lives in the other docs.

## 2014

The owner bought into the Google Glass Explorer program in February 2014. Google never shipped the unit to him: Explorers collected theirs in person, and on February 28, 2014 he sat for a fitting at Google's San Francisco office, 350 The Embarcadero, welcome letter and all, and carried the future home on the plane. (For years he told this story with the glasses arriving in April after two cancelled orders. Checking the receipts for the article this repo accompanies, that paper trail turned out to belong to a $225 prescription frame ordered afterwards; the glasses themselves were on his face within eight days of paying for them. Memory is a bad database. Check your receipts.) He wore the future for a while, the way several thousand people did. Glass's consumer story ended the following January; the last Explorer mail in his archive is the wind-down notice of January 15, 2015. The glasses went into a drawer, where they stayed, through the enterprise editions, the quiet years, and the final end of all Glass support in September 2023.

## What Google accidentally left behind

The last firmware Google ever shipped, XE24, in December 2019, is usually described as a shutdown notice. Read it again and it is something else: Google removed the Google account, removed the backend connection, and left a self-contained computer — a 640×360 prism display, a touchpad, an IMU, Wi-Fi, Bluetooth, a microphone, a camera, and a bone-conduction speaker, running a Linux kernel whose source Google had published. A dead product, but a complete machine, owing nothing to anyone's cloud.

In October 2026 the owner took the glasses out of the drawer and asked the only interesting question left about them: how much of this can be rewritten? The research (done first, argued about honestly, and summarized in PRIOR-ART.md) said: everything from the kernel up. The bootloaders have no public source and stay untouched; the camera runs on closed firmware; otherwise the machine is open in every way that matters.

## The scene that woke up

The plan was nearly scooped by its own prior-art check. Three weeks earlier, in the same month, another builder had published glass-xec-mainline: Alpine Linux daily-driven on Glass, on the same reasoning, down to the same stock kernel. And postmarketOS turned out to have ported Glass itself back in 2017. The honest article, and this repo, credits all of it.

But two parts of the plan came back unclaimed. Nobody had hung a modern AI assistant's gadget SDK off the glasses — Meta's Muse Gadget SDK had itself only opened to the public on October 2, 2026, and nothing in its world targeted glasses. And nobody had published an operating system for Glass written by an AI coding agent. The owner aimed at exactly those two gaps.

## Argon

The build brief was written as a milestone contract with one explicit cliff: milestone M3, Bluetooth LE pairing from a 2010s kernel, had no public precedent, and if it failed, the builder was to stop and report rather than improvise. The brief went to Google's Gemini 4 Argon, through Antigravity. To keep the model's usage quota free for the run, the owner's other Antigravity workers were wound down for the evening. A Google AI, given a dead Google product, a written contract, and a quota cleared in its honor.

It built the pipeline first, hardware-free, the way the contract asked. When the unit came out of the drawer — battery inspected before charging, because an eleven-year-old sealed cell earns that much respect — the milestones fell in order: the framebuffer card, Wi-Fi and modern TLS on a 2014 kernel, and then the cliff. BlueZ would not pair. The build went underneath it, to raw HCI commands and a shim at the L2CAP layer, and the phone app answered with `pairing_confirmed`. After that, the voice call was almost an anticlimax, which is what a good cliff does to the rest of the mountain: a question spoken into a pair of glasses, an answer returned through the bone behind the ear, an animated face on the prism keeping time with the words.

## What it became next

The first public release was the gadget: pair, talk, photograph, describe. What happened after is the part nobody had put in the brief. The glasses grew a control plane — a small authenticated daemon on the device and a 19-tool MCP server on the host — and stopped being a demo you operate and became a device other software can build on. They learned to record video, to stream voice in both directions at once with the words appearing on the prism as they are spoken, to keep their own settings on a card you drive from the touchpad. They learned to share their hardware with a second, local system behind a guarded chooser, to run cool enough to wear, and finally to boot themselves from their own storage and take updates over the air, no cable anywhere in the story. The session-by-session record of that second act is in [narrative/](narrative/).

## What it is, and is not

The result makes the glasses a voice endpoint for Meta's Muse. That is a cloud service, reached over the internet, and this repo says so plainly wherever it matters. The glasses also remain a complete little Linux machine that boots in under a minute and answers SSH; what else they become is, at last, a matter of local imagination rather than a product roadmap. The drawer is empty now. That was the point.
