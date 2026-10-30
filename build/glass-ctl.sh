#!/usr/bin/env bash
# ============================================================================
# glass-ctl.sh — Safe Host Operator Script for Google Glass (notle)
# ============================================================================
# Enforces hard guardrails in code:
#   1. Partition allowlist: ONLY boot, recovery, system, cache, userdata
#   2. Partition denylist:  NEVER xloader, bootloader, or fpga
#   3. Default M1 flow uses `fastboot boot` (loads into RAM without flashing)
# ============================================================================
set -euo pipefail

OVERLAY_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$OVERLAY_DIR/out"
BOOT_M0_IMG="$OUT_DIR/boot-m0.img"
GLASS_IP="${GLASS_IP:-172.16.42.1}"

assert_partition_allowed() {
    local part="$1"
    case "$part" in
        xloader|bootloader|fpga|glasshub)
            echo "[FATAL GUARDRAIL] Refusing operation on forbidden partition '$part' (brick hazard — see docs/SAFE-FLASHING.md and docs/milestones/M1-first-card.md)." >&2
            exit 99
            ;;
        boot|recovery|system|cache|userdata)
            return 0
            ;;
        *)
            echo "[FATAL GUARDRAIL] Partition '$part' is not on the explicit allowlist (boot|recovery|system|cache|userdata)." >&2
            exit 98
            ;;
    esac
}

cmd_status() {
    echo "=== USB Devices (Google Vendor 18d1 / TI OMAP 0451) ==="
    lsusb | grep -iE '18d1:|0451:|google|texas' || echo "  (no Google/OMAP USB device currently detected on bus)"
    echo ""
    echo "=== ADB Devices ==="
    adb devices -l || true
    echo ""
    echo "=== Fastboot Devices ==="
    fastboot devices -l || true
    echo ""
    echo "=== USB RNDIS Host Interface (172.16.42.0/24) ==="
    ip -br addr show | grep -E '172\.16\.42|usb|enx|eth' || echo "  (no RNDIS interface up yet)"
}

cmd_unit_check() {
    echo "=== Running non-destructive ADB unit inspection (docs/UNIT.md) ==="
    adb wait-for-device
    echo "ro.build.display.id : $(adb shell getprop ro.build.display.id | tr -d '\r')"
    echo "ro.build. fingerprint: $(adb shell getprop ro.build.fingerprint | tr -d '\r')"
    echo "ro.product.board    : $(adb shell getprop ro.product.board | tr -d '\r')"
    echo "ro.serialno         : $(adb shell getprop ro.serialno | tr -d '\r')"
    echo "kernel              : $(adb shell uname -a | tr -d '\r')"
    echo "meminfo             : $(adb shell grep MemTotal /proc/meminfo | tr -d '\r')"
    echo "battery capacity    : $(adb shell cat /sys/class/power_supply/bq27520-0/capacity 2>/dev/null | tr -d '\r')%"
    echo "battery voltage_now : $(adb shell cat /sys/class/power_supply/bq27520-0/voltage_now 2>/dev/null | tr -d '\r') uV"
}

cmd_boot_m0() {
    local img="${1:-$BOOT_M0_IMG}"
    [ -f "$img" ] || { echo "Missing boot image: $img (run build/build-m0.sh first)" >&2; exit 1; }
    echo "=== Booting $img into RAM via 'fastboot boot' (zero flash writes) ==="
    fastboot boot "$img"
}

cmd_flash_safe() {
    local part="${1:-}"
    local img="${2:-}"
    [ -n "$part" ] && [ -n "$img" ] || { echo "Usage: $0 flash <boot|recovery|system|cache|userdata> <image>" >&2; exit 1; }
    assert_partition_allowed "$part"
    [ -f "$img" ] || { echo "Image file not found: $img" >&2; exit 1; }
    echo "=== Flashing allowlisted partition '$part' with $img ==="
    fastboot flash "$part" "$img"
}

cmd_net_up() {
    # Configure the host side of the USB RNDIS link at 172.16.42.2/24 if DHCP hasn't assigned it yet
    local iface="${1:-}"
    if [ -z "$iface" ]; then
        iface="$(ip -o link show | awk -F': ' '{print $2}' | grep -E '^(usb0|enx)' | head -n 1 || true)"
    fi
    [ -n "$iface" ] || { echo "No usb0/enx* RNDIS interface found on host yet." >&2; exit 1; }
    echo "=== Bringing up host RNDIS interface $iface (172.16.42.2/24 -> Glass $GLASS_IP) ==="
    sudo ip link set "$iface" up
    if ! ip -4 addr show dev "$iface" | grep -q '172\.16\.42\.'; then
        sudo ip addr add 172.16.42.2/24 dev "$iface" || true
    fi
    ping -c 2 -W 2 "$GLASS_IP"
}

cmd_ssh() {
    shift || true
    ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "root@$GLASS_IP" "$@"
}

cmd_screenshot() {
    local out_png="${1:-$OVERLAY_DIR/docs/screenshots/m1-prism-framebuffer.png}"
    mkdir -p "$(dirname "$out_png")"
    local tmp_raw
    tmp_raw="$(mktemp /tmp/glass-fb0.XXXXXX.raw)"
    echo "=== Capturing /dev/fb0 (640x360 BGRA32) from Glass ($GLASS_IP) ==="
    ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "root@$GLASS_IP" \
        "dd if=/dev/fb0 bs=46080 count=20 2>/dev/null" > "$tmp_raw"
    python3 - "$tmp_raw" "$out_png" <<'PY'
import sys
from PIL import Image
raw_path, out_path = sys.argv[1], sys.argv[2]
raw = open(raw_path, "rb").read()
w, h = 640, 360
img = Image.frombytes("RGBA", (w, h), raw[:w*h*4], "raw", "BGRA")
img.convert("RGB").save(out_path)
print(f"Saved {w}x{h} screenshot -> {out_path}")
PY
    rm -f "$tmp_raw"
}

cmd_card() {
    local arg="${1:-tour}"
    if [ "$arg" = "tour" ] || [ "$arg" = "auto" ]; then
        echo "=== Resuming 4-card auto-tour on Glass prism ==="
        ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "root@$GLASS_IP" \
            "rm -f /run/glass/card && killall -9 fb-hello 2>/dev/null || true"
    else
        echo "=== Pinning Glass prism to card $arg (0=M1, 1=M2, 2=M3, 3=M4) ==="
        ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "root@$GLASS_IP" \
            "echo '$arg' > /run/glass/card && killall -9 fb-hello 2>/dev/null || true"
    fi
}

cmd_record() {
    local secs="${1:-10}"
    local out_prefix="${2:-$OVERLAY_DIR/docs/screenshots/prism-live-capture}"
    mkdir -p "$(dirname "$out_prefix")"
    local tmp_dir
    tmp_dir="$(mktemp -d /tmp/glass-rec.XXXXXX)"
    echo "=== Recording /dev/fb0 from Glass ($GLASS_IP) for ~${secs}s ==="
    local i=0
    local end=$(( $(date +%s) + secs ))
    while [ "$(date +%s)" -lt "$end" ]; do
        local frame_raw
        frame_raw="$(printf "%s/frame-%04d.raw" "$tmp_dir" "$i")"
        ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "root@$GLASS_IP" \
            "dd if=/dev/fb0 bs=46080 count=20 2>/dev/null" > "$frame_raw"
        i=$((i + 1))
        sleep 0.5
    done
    python3 - "$tmp_dir" "$out_prefix" <<'PY'
import glob, os, sys
from PIL import Image
tmp_dir, prefix = sys.argv[1], sys.argv[2]
raws = sorted(glob.glob(os.path.join(tmp_dir, "frame-*.raw")))
w, h = 640, 360
pngs = []
for idx, rpath in enumerate(raws):
    raw = open(rpath, "rb").read()
    if len(raw) < w * h * 4:
        continue
    img = Image.frombytes("RGBA", (w, h), raw[:w*h*4], "raw", "BGRA").convert("RGB")
    ppath = os.path.join(tmp_dir, f"frame-{idx:04d}.png")
    img.save(ppath)
    pngs.append(img.convert("P", palette=Image.ADAPTIVE))
if pngs:
    gif_out = f"{prefix}.gif"
    pngs[0].save(gif_out, save_all=True, append_images=pngs[1:], duration=800, loop=0)
    print(f"Saved animated GIF ({len(pngs)} frames) -> {gif_out}")
PY
    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -y -framerate 2 -pattern_type glob -i "$tmp_dir/frame-*.png" \
            -vf "fps=30,format=yuv420p" -c:v libx264 -pix_fmt yuv420p "${out_prefix}.mp4" 2>/dev/null || true
        echo "Saved MP4 video -> ${out_prefix}.mp4"
    fi
    rm -rf "$tmp_dir"
}

case "${1:-status}" in
    status|check-usb)
        cmd_status
        ;;
    unit-check)
        cmd_unit_check
        ;;
    boot-m0)
        cmd_boot_m0 "${2:-$BOOT_M0_IMG}"
        ;;
    flash)
        cmd_flash_safe "${2:-}" "${3:-}"
        ;;
    net-up)
        cmd_net_up "${2:-}"
        ;;
    ssh)
        cmd_ssh "$@"
        ;;
    screenshot)
        cmd_screenshot "${2:-}"
        ;;
    card)
        cmd_card "${2:-tour}"
        ;;
    record)
        cmd_record "${2:-10}" "${3:-}"
        ;;
    *)
        cat <<EOF
Usage: $0 <command> [args...]

Commands:
  status                        Check USB bus, adb, fastboot, and RNDIS link
  unit-check                    Inspect connected Glass unit over ADB (docs/UNIT.md)
  boot-m0 [boot.img]            RAM-boot out/boot-m0.img via 'fastboot boot' (no flash writes)
  flash <partition> <img_path>  Flash allowlisted partition (boot|recovery|system|cache|userdata ONLY)
  net-up [iface]                Configure host USB RNDIS interface (172.16.42.2/24) and ping Glass
  ssh [cmd...]                  SSH into Glass over USB RNDIS (root@172.16.42.1)
  screenshot [out.png]          Capture /dev/fb0 (640x360) from Glass and save as PNG
  card <0|1|2|3|tour>           Pin a milestone card (0=M1, 1=M2, 2=M3, 3=M4) or run auto-tour
  record [seconds] [out_prefix] Record live /dev/fb0 frames to animated .gif and .mp4
EOF
        exit 1
        ;;
esac
