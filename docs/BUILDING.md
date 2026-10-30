# Building and first boot

## You will need

- A Google Glass Explorer Edition (this build was developed and verified on a 2014 unit, board `glass_1`), unlocked or unlockable. Unlocking wipes the device's user data.
- The official XE24 factory image, `signed-glass_1-img-5585826.zip`, from Google's Glass system-image download page. It is both the source of the stock kernel and firmware blobs, and your recovery floor. Verify it before use: SHA-1 `46430bc827267796566f0a73d9b503059759561c`, SHA-256 `76a9d3822077e993bba62c586e5e7cbb1217a81171d78033ff7c06ba285f2363`.
- A build host with Python 3, a C cross toolchain for ARMv7 musl, and Android platform-tools (fastboot).
- A 2.4 GHz Wi-Fi network (the BCM4330 has no 5 GHz radio) and, for the gadget build, a phone with the Muse app and a Muse Gadget SDK token from gadgets.muse.ai.

## Safety rules, before anything else

- **Never write xloader, bootloader, fpga, or the sensor-hub partition.** No public source, no recovery. The flash wrapper in this project enforces an allowlist (boot, recovery, system, cache, userdata) in code.
- Inspect the battery before the first charge. The cell is sealed, 570 mAh, and over a decade old. Any swelling, seam gap, or softness: stop, do not charge it.
- Test every image from RAM first: `fastboot boot <image>`. Flash only after a RAM boot has passed.
- Charge supervised, from a plain 5 V source, the first few times.

## Build

```sh
# 1. Place the verified factory zip where the pipeline expects it
#    (see build/build-m0.sh header for the exact layout).

# 2. Optionally pre-provision Wi-Fi (derived PSK only is stored):
export GLASS_WIFI_SSID="your-2.4ghz-ssid"
export GLASS_WIFI_PASS="your-passphrase"

# 3. Build the M0 boot image (stock kernel + Prism initramfs):
build/build-m0.sh

# 4. Check the result against the ceiling and the kernel hash:
#    out/boot-m0.img must be <= 8,388,608 bytes, and the kernel inside
#    must hash to 83476052edc649505c480700d88e1a7d743232d6181b970556722cf4bdb79def.
```

The firmware blobs the radio needs are extracted from the factory `system.img` during the build; verify them against [../firmware/README.md](../firmware/README.md).

## First boot and pairing

```sh
fastboot boot out/boot-m0.img      # RAM only; a power cycle restores stock
```

From there the milestones in [MILESTONES.md](MILESTONES.md) are the runbook: confirm the prism card, join Wi-Fi (`glass-wifi add`), then install the gadget bundle (`build/build-m3-bundle.py` produces it) and run the pairing flow with the Muse app open at Settings > Devices with Developer mode on. Pairing binds to the device that pairs — always pair on the Glass itself. Voice runs through `glass-muse-voice` once paired; expect to supply the SDK token and your call target at run time, because neither is ever baked into an image.

## Operator environment variables

Nothing site-specific is baked into the images or scripts in this repo; the operator supplies it at run time:

- `GLASS_WIFI_SSID` / `GLASS_WIFI_PASS` — build-time Wi-Fi provisioning (only the derived PSK is stored).
- `GLASS_IP` / `GLASS_WIFI_IP` — how `build/glass-ctl.sh` and `mcp/glass_mcp.py` find the unit on Wi-Fi (the USB RNDIS address, 172.16.42.1, is probed automatically).
- `GLASS_AGENT_TOKEN` (or `~/.config/glass-mcp/token`) — the Bearer token `glass-agentd` requires on Wi-Fi; on the device it lives at `/etc/prism/token` (mode 0600).
- `BT_BDADDR` — the unit's own Bluetooth address, required by `glass-bt-up` (printed by the stock system, or by `hciconfig` after a stock boot); it can also be stored on the device in `/var/lib/musegadget/bdaddr`.
- `PRISM_HOST` — the second system's host address, used by the dual-boot chooser's Wi-Fi path (the USB dock link is probed automatically).
