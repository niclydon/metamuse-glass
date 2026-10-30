#!/usr/bin/env bash
# ============================================================================
# M0 Build Pipeline — Meta Muse Gadget Overlay for Google Glass (XE24 / notle)
# ============================================================================
# Unpacks the archived XE24 factory image, preserves Google's stock 3.4.94
# kernel byte-for-byte, extracts proprietary firmware blobs from system.img,
# cross-compiles static ARMv7 utilities (/usr/bin/fb-hello, /usr/bin/glass-wav),
# builds the non-systemd Alpine armv7 initramfs overlay with USB RNDIS lifeline
# (172.16.42.1) + serial logging (ttyO2,115200n8) + /dev/fb0 test pattern HUD,
# and repacks out/boot-m0.img within the 8 MiB boot partition limit.
#
# Guardrail: Never touches xloader, bootloader, or fpga. Never builds or patches
# a kernel.
# ============================================================================
set -euo pipefail

export PATH="$PATH:/usr/sbin:/sbin"

OVERLAY_DIR="$(cd "$(dirname "$0")/.." && pwd)"
REPO_ROOT="$(cd "$OVERLAY_DIR/.." && pwd)"
UPSTREAM_XEC="$REPO_ROOT/upstream/glass-xec-mainline"
BOOTIMG_INFO_PY="$REPO_ROOT/tools/bootimg-info.py"

WORK_DIR="$OVERLAY_DIR/work"
FW_DIR="$OVERLAY_DIR/firmware"
OUT_DIR="$OVERLAY_DIR/out"
CACHE_DIR="$WORK_DIR/cache"

FACTORY_ZIP="${GLASS_FACTORY_ZIP:-$HOME/workspace/google-glass/factory-images/signed-glass_1-img-5585826.zip}"
EXPECTED_ZIP_SHA256="76a9d3822077e993bba62c586e5e7cbb1217a81171d78033ff7c06ba285f2363"
EXPECTED_BOOT_SHA256="05431030c392e27c9054781e6c110fed880edde309a3811c6af9609e3d0e5396"
EXPECTED_KERNEL_SHA256="83476052edc649505c480700d88e1a7d743232d6181b970556722cf4bdb79def"
EXPECTED_KERNEL_SIZE=5090464
BOOT_PARTITION_LIMIT=8388608

# Stock XE24 boot.img header parameters (must match byte-for-byte)
BOOT_BASE="0x80000000"
BOOT_PAGESIZE="2048"
BOOT_KERNEL_OFFSET="0x00008000"
BOOT_RAMDISK_OFFSET="0x01000000"
BOOT_SECOND_OFFSET="0x00f00000"
BOOT_TAGS_OFFSET="0x00000100"
BOOT_CMDLINE="console=ttyO2,115200n8 vmalloc=500M androidboot.console=ttyO2 androidboot.carrier=wifi-only product_type=w cpuidle_sysfs_switch"

# Alpine 3.21.3 armv7 userland packages for initramfs
ALPINE_MIRROR="https://dl-cdn.alpinelinux.org/alpine/v3.21"
ALPINE_MINIROOTFS_FILE="alpine-minirootfs-3.21.3-armv7.tar.gz"
ALPINE_MINIROOTFS_URL="$ALPINE_MIRROR/releases/armv7/$ALPINE_MINIROOTFS_FILE"
ALPINE_MINIROOTFS_SHA256="b32bde0fb6bb6aa6bf9ffbc4be4bff6bef6afff2bd4ba434300c8cd2088fd92"

CC_ARM="${CC_ARM:-arm-linux-gnueabihf-gcc}"
CC_HOST="${CC_HOST:-gcc}"

log() {
    printf '\033[1;36m[build-m0]\033[0m %s\n' "$*"
}

die() {
    printf '\033[1;31m[build-m0 ERROR]\033[0m %s\n' "$*" >&2
    exit 1
}

check_prereqs() {
    log "Checking host build tools..."
    for cmd in "$CC_ARM" "$CC_HOST" python3 simg2img debugfs bsdtar curl sha256sum unzip; do
        command -v "$cmd" >/dev/null 2>&1 || die "Required command '$cmd' not found in PATH"
    done
    [ -f "$FACTORY_ZIP" ] || die "Missing factory image archive at $FACTORY_ZIP"
}

verify_and_extract_factory() {
    mkdir -p "$WORK_DIR/factory" "$WORK_DIR/stock-boot" "$FW_DIR" "$OUT_DIR" "$CACHE_DIR"

    log "Verifying XE24 factory archive SHA-256..."
    local zip_sha
    zip_sha="$(sha256sum "$FACTORY_ZIP" | awk '{print $1}')"
    if [ "$zip_sha" != "$EXPECTED_ZIP_SHA256" ]; then
        die "Factory zip SHA-256 mismatch: got $zip_sha, expected $EXPECTED_ZIP_SHA256"
    fi

    if [ ! -f "$WORK_DIR/factory/boot.img" ] || [ ! -f "$WORK_DIR/factory/system.img" ]; then
        log "Extracting boot.img and system.img from XE24 factory zip (ignoring bootloader/xloader/fpga)..."
        unzip -o "$FACTORY_ZIP" boot.img system.img -d "$WORK_DIR/factory"
    fi

    local boot_sha
    boot_sha="$(sha256sum "$WORK_DIR/factory/boot.img" | awk '{print $1}')"
    [ "$boot_sha" = "$EXPECTED_BOOT_SHA256" ] || die "Stock boot.img SHA-256 mismatch: $boot_sha"
    log "Verified stock XE24 boot.img SHA-256: $boot_sha"
}

build_host_tools_and_unpack_boot() {
    log "Building host bootimg tool..."
    "$CC_HOST" -O2 -Wall -Wextra -o "$WORK_DIR/bootimg" "$UPSTREAM_XEC/tools/bootimg/bootimg.c"

    log "Unpacking stock XE24 boot.img..."
    "$WORK_DIR/bootimg" unpack "$WORK_DIR/factory/boot.img" "$WORK_DIR/stock-boot"

    local k_size k_sha
    k_size="$(stat -c '%s' "$WORK_DIR/stock-boot/kernel")"
    k_sha="$(sha256sum "$WORK_DIR/stock-boot/kernel" | awk '{print $1}')"
    [ "$k_size" -eq "$EXPECTED_KERNEL_SIZE" ] || die "Stock kernel size mismatch: got $k_size, expected $EXPECTED_KERNEL_SIZE"
    [ "$k_sha" = "$EXPECTED_KERNEL_SHA256" ] || die "Stock kernel SHA-256 mismatch: got $k_sha, expected $EXPECTED_KERNEL_SHA256"
    log "Verified stock XE24 3.4.94 kernel: $k_size bytes, SHA-256 $k_sha"
}

extract_firmware_blobs() {
    if [ -s "$FW_DIR/fw_bcmdhd.bin" ] && [ -s "$FW_DIR/bcmdhd.cal" ] && [ -s "$FW_DIR/bcm4330.hcd" ]; then
        log "Proprietary firmware blobs already extracted in $FW_DIR"
        return 0
    fi

    log "Unsparsing XE24 system.img and extracting Wi-Fi/BT/audio firmware blobs..."
    local raw_sys="$WORK_DIR/factory/system.raw.img"
    if [ ! -f "$raw_sys" ]; then
        simg2img "$WORK_DIR/factory/system.img" "$raw_sys"
    fi

    debugfs -R "dump /vendor/firmware/fw_bcmdhd.bin $FW_DIR/fw_bcmdhd.bin" "$raw_sys" 2>/dev/null
    debugfs -R "dump /etc/wifi/bcmdhd.cal $FW_DIR/bcmdhd.cal" "$raw_sys" 2>/dev/null
    debugfs -R "dump /vendor/firmware/bcm4330.hcd $FW_DIR/bcm4330.hcd" "$raw_sys" 2>/dev/null
    debugfs -R "dump /vendor/firmware/tesla-dsp.xe64T $FW_DIR/tesla-dsp.xe64T" "$raw_sys" 2>/dev/null || true
    debugfs -R "dump /etc/firmware/dss_fpga.img $FW_DIR/dss_fpga.img" "$raw_sys" 2>/dev/null || true

    rm -f "$raw_sys"

    for req in fw_bcmdhd.bin bcmdhd.cal bcm4330.hcd; do
        [ -s "$FW_DIR/$req" ] || die "Failed to extract required firmware blob $req from XE24 system.img"
    done
    log "Extracted firmware blobs into $FW_DIR:"
    ls -lh "$FW_DIR"
}

build_arm_binaries() {
    local sysroot="$WORK_DIR/musl-sysroot"
    rm -rf "$sysroot"
    mkdir -p "$sysroot"
    bsdtar -xzpf "$CACHE_DIR/$ALPINE_MINIROOTFS_FILE" -C "$sysroot"
    local musl_dev_apk
    musl_dev_apk="$(ls "$CACHE_DIR"/musl-dev-[0-9]*.apk | head -n 1)"
    bsdtar -xzpf "$musl_dev_apk" -C "$sysroot" \
        --exclude='.PKGINFO' --exclude='.SIGN*'

    local gcc_inc
    gcc_inc="$("$CC_ARM" -print-file-name=include)"
    local musl_flags=(
        -nostdinc
        -isystem "$sysroot/usr/include"
        -isystem "$gcc_inc"
        -idirafter /usr/arm-linux-gnueabihf/include
        -B"$sysroot/usr/lib"
        -L"$sysroot/usr/lib"
        -L"$sysroot/lib"
        -Wl,--dynamic-linker=/lib/ld-musl-armhf.so.1
        -U_TIME_BITS -U_FILE_OFFSET_BITS
        -Os -s -Wall -Wextra
    )

    log "Cross-compiling native Alpine musl ARMv7 binaries with $CC_ARM..."
    "$CC_ARM" "${musl_flags[@]}" \
        -o "$OUT_DIR/fb-hello" "$OVERLAY_DIR/src/fb-hello.c"
    "$CC_ARM" "${musl_flags[@]}" \
        -o "$OUT_DIR/glass-wav" "$OVERLAY_DIR/src/glass-wav.c" -lm
    "$CC_ARM" "${musl_flags[@]}" -shared -fPIC \
        -o "$OUT_DIR/libglass_ble_shim.so" "$OVERLAY_DIR/src/glass-ble-shim.c"

    # Also build upstream prism console helpers for interactive USB/SSH terminal mirroring
    "$CC_ARM" "${musl_flags[@]}" \
        -o "$OUT_DIR/glass-fb" "$UPSTREAM_XEC/tools/glass-fb/glass-fb.c"
    "$CC_ARM" "${musl_flags[@]}" \
        -o "$OUT_DIR/glass-console" "$UPSTREAM_XEC/tools/glass-console/glass-console.c"

    rm -rf "$sysroot"
    log "Built native musl ARMv7 binaries:"
    ls -lh "$OUT_DIR/fb-hello" "$OUT_DIR/glass-wav" "$OUT_DIR/libglass_ble_shim.so" "$OUT_DIR/glass-fb" "$OUT_DIR/glass-console"
}

fetch_alpine_apks() {
    if [ ! -f "$CACHE_DIR/$ALPINE_MINIROOTFS_FILE" ]; then
        log "Downloading $ALPINE_MINIROOTFS_FILE..."
        curl -fSL -o "$CACHE_DIR/$ALPINE_MINIROOTFS_FILE" "$ALPINE_MINIROOTFS_URL"
    fi
    if [ ! -f "$CACHE_DIR/APKINDEX.tar.gz" ]; then
        log "Downloading Alpine v3.21 armv7 APKINDEX..."
        curl -fSL -o "$CACHE_DIR/APKINDEX.tar.gz" "$ALPINE_MIRROR/main/armv7/APKINDEX.tar.gz"
        bsdtar -xzf "$CACHE_DIR/APKINDEX.tar.gz" -C "$CACHE_DIR" APKINDEX
    fi

    # Resolve package versions from APKINDEX for initramfs essentials
    for pkg in musl-dev busybox-extras skalibs-libs utmps-libs dropbear wpa_supplicant; do
        local ver file
        ver="$(awk -v pkg="$pkg" '
            /^P:/ { p = substr($0, 3) }
            /^V:/ { if (p == pkg) { print substr($0, 3); exit } }
        ' "$CACHE_DIR/APKINDEX")"
        [ -n "$ver" ] || die "Package $pkg not found in Alpine APKINDEX"
        file="${pkg}-${ver}.apk"
        if [ ! -f "$CACHE_DIR/$file" ]; then
            log "Downloading $file..."
            curl -fSL -o "$CACHE_DIR/$file" "$ALPINE_MIRROR/main/armv7/$file"
        fi
    done
}

build_initramfs() {
    local stage="$WORK_DIR/initramfs-stage"
    rm -rf "$stage"
    mkdir -p "$stage"

    log "Staging Alpine armv7 base + dropbear + wpa_cli + modern CA store into initramfs..."
    bsdtar -xzpf "$CACHE_DIR/$ALPINE_MINIROOTFS_FILE" -C "$stage"

    for pkg in busybox-extras skalibs-libs utmps-libs dropbear; do
        local apk
        apk="$(ls "$CACHE_DIR/${pkg}"-[0-9]*.apk | head -n 1)"
        bsdtar -xzpf "$apk" -C "$stage" \
            --exclude='.PKGINFO' --exclude='.SIGN*' \
            --exclude='.pre-*' --exclude='.post-*' --exclude='.trigger'
    done

    # Extract only wpa_cli and wpa_passphrase from wpa_supplicant apk (stock wpa_supplicant
    # binary runs from read-only /mnt/system/bin/wpa_supplicant on /dev/mmcblk0p8)
    local wpa_apk
    wpa_apk="$(ls "$CACHE_DIR"/wpa_supplicant-[0-9]*.apk | head -n 1)"
    bsdtar -xzpf "$wpa_apk" -C "$stage" sbin/wpa_cli sbin/wpa_passphrase

    # Populate /bin & /sbin busybox symlinks for initramfs environment
    for app in sh ash mount umount mkdir mknod mdev sleep cat echo dmesg \
               ip ifconfig route udhcpd udhcpc insmod modprobe ls ln rm cp mv \
               chmod chown grep sed awk cut head tail tr sort uniq wc \
               ps kill killall pidof top free df du find xargs tar gzip xz unxz \
               date hwclock ntpd poweroff reboot halt getty login passwd \
               vi less clear reset stty tty sync uname hostname id env \
               seq expr test printf true false yes nohup watch hexdump \
               dd stat touch mktemp basename dirname readlink realpath \
               md5sum sha256sum wget nc ping traceroute netstat; do
        if [ ! -e "$stage/bin/$app" ] && [ ! -e "$stage/sbin/$app" ] && \
           [ ! -e "$stage/usr/bin/$app" ] && [ ! -e "$stage/usr/sbin/$app" ]; then
            ln -sf /bin/busybox "$stage/bin/$app"
        fi
    done

    # Passwordless root login on serial console and USB RNDIS SSH (172.16.42.1) + host SSH pubkeys
    sed -i 's|^root:[^:]*:|root::|' "$stage/etc/passwd"
    if [ -f "$stage/etc/shadow" ]; then
        sed -i 's|^root:[^:]*:|root::|' "$stage/etc/shadow"
    fi
    echo "glass-muse" > "$stage/etc/hostname"
    mkdir -p -m 0700 "$stage/root/.ssh"
    if compgen -G "$HOME/.ssh/*.pub" >/dev/null 2>&1; then
        cat "$HOME/.ssh/"*.pub > "$stage/root/.ssh/authorized_keys"
        chmod 0600 "$stage/root/.ssh/authorized_keys"
    fi

    # Optional build-time Wi-Fi provisioning (in .gitignored out/ only; never committed)
    if [ -n "${GLASS_WIFI_SSID:-}" ] && [ -n "${GLASS_WIFI_PASS:-}" ]; then
        log "Pre-populating /etc/wpa_supplicant/wpa_supplicant.conf for SSID '$GLASS_WIFI_SSID'..."
        mkdir -p "$stage/etc/wpa_supplicant"
        python3 - "$GLASS_WIFI_SSID" "$GLASS_WIFI_PASS" "$stage/etc/wpa_supplicant/wpa_supplicant.conf" <<'PY'
import hashlib, sys
ssid, pw, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
psk = hashlib.pbkdf2_hmac("sha1", pw.encode("utf-8"), ssid.encode("utf-8"), 4096, 32).hex()
with open(out_path, "w") as f:
    f.write("ctrl_interface=/run/wpa_supplicant\nupdate_config=1\n\nnetwork={\n")
    f.write(f'    ssid="{ssid}"\n    psk={psk}\n}}\n')
PY
        chmod 0600 "$stage/etc/wpa_supplicant/wpa_supplicant.conf"
    fi

    # Trim APK metadata & unused tools while KEEPING /etc/ssl/certs/ca-certificates.crt,
    # ssl_client, libssl.so.3, and libcrypto.so.3 (xz-packed for initramfs density)
    rm -rf "$stage/var/cache/apk" "$stage/lib/apk" \
           "$stage/usr/share/ca-certificates" "$stage/etc/ssl/certs/ca-cert-"* \
           "$stage/etc/ssl/certs/"*.0 "$stage/etc/ssl/misc" "$stage/etc/ssl/openssl.cnf.dist" \
           "$stage/usr/sbin/dropbearconvert" "$stage/usr/bin/dropbearkey" \
           "$stage/sbin/apk" "$stage/usr/bin/scanelf" \
           "$stage/usr/bin/getent" "$stage/usr/bin/getconf" "$stage/usr/bin/iconv" \
           "$stage/usr/lib/libapk.so"* "$stage/usr/lib/ossl-modules" \
           "$stage/usr/lib/engines-3" "$stage/etc/udhcpd.conf"

    bsdtar -cf - -C "$stage" usr/lib/libcrypto.so.3 usr/lib/libssl.so.3 \
        | xz --check=crc32 --arm --lzma2=preset=9e,dict=8MiB > "$stage/usr/lib/openssl.tar.xz"
    rm -f "$stage/usr/lib/libcrypto.so.3" "$stage/usr/lib/libssl.so.3"

    # Overlay our muse-gadget-overlay scripts and configs
    cp -a "$OVERLAY_DIR/rootfs-overlay/." "$stage/"
    chmod 0755 "$stage/init" \
               "$stage/etc/glass/rcS" \
               "$stage/etc/glass/console" \
               "$stage/usr/sbin/glass-gadget" \
               "$stage/usr/sbin/glass-wifi" \
               "$stage/usr/sbin/glass-audio-route" \
               "$stage/usr/sbin/glass-bt-up" \
               "$stage/usr/sbin/glass-muse-voice" \
               "$stage/usr/sbin/glass-muse-camera" \
               "$stage/usr/sbin/glass-agentd"

    # Install our static ARMv7 binaries + BLE L2CAP shim
    install -m 0755 "$OUT_DIR/fb-hello" "$stage/usr/bin/fb-hello"
    install -m 0755 "$OUT_DIR/glass-wav" "$stage/usr/bin/glass-wav"
    install -m 0755 "$OUT_DIR/libglass_ble_shim.so" "$stage/usr/lib/libglass_ble_shim.so"
    install -m 0755 "$OUT_DIR/glass-fb" "$stage/usr/bin/glass-fb"
    install -m 0755 "$OUT_DIR/glass-console" "$stage/usr/bin/glass-console"

    if [ -f "$OVERLAY_DIR/tools/build-nova-sprites.py" ] && [ ! -f "$OUT_DIR/nova_sprites_compact.bin.xz" ]; then
        python3 "$OVERLAY_DIR/tools/build-nova-sprites.py" || true
    fi
    if [ -f "$OUT_DIR/nova_sprites_compact.bin.xz" ]; then
        mkdir -p "$stage/usr/share/glass"
        install -m 0644 "$OUT_DIR/nova_sprites_compact.bin.xz" "$stage/usr/share/glass/nova_sprites.bin.xz"
    fi

    # Stage Wi-Fi and Bluetooth firmware directly at /system/etc/firmware/,
    # /system/vendor/firmware/, and /system/etc/wifi/ (exact paths compiled into XE24 bcmdhd)
    mkdir -p "$stage/system/etc/firmware" "$stage/system/vendor/firmware" \
             "$stage/system/etc/wifi" "$stage/etc/wifi" \
             "$stage/vendor/firmware" "$stage/lib/firmware"
    cp -f "$FW_DIR/fw_bcmdhd.bin" "$stage/system/etc/firmware/fw_bcmdhd.bin"
    cp -f "$FW_DIR/bcmdhd.cal" "$stage/system/etc/firmware/bcmdhd.cal"
    cp -f "$FW_DIR/bcm4330.hcd" "$stage/system/etc/firmware/bcm4330.hcd"
    ln -sf /system/etc/firmware/bcm4330.hcd "$stage/lib/firmware/BCM4330B1.hcd"
    ln -sf /system/etc/firmware/bcm4330.hcd "$stage/lib/firmware/bcm4330.hcd"
    ln -sf /system/etc/firmware/fw_bcmdhd.bin "$stage/system/vendor/firmware/fw_bcmdhd.bin"
    ln -sf /system/etc/firmware/bcm4330.hcd "$stage/system/vendor/firmware/bcm4330.hcd"
    ln -sf /system/etc/firmware/fw_bcmdhd.bin "$stage/vendor/firmware/fw_bcmdhd.bin"
    ln -sf /system/etc/firmware/bcm4330.hcd "$stage/vendor/firmware/bcm4330.hcd"
    ln -sf /system/etc/firmware/bcmdhd.cal "$stage/system/etc/wifi/bcmdhd.cal"
    ln -sf /system/etc/firmware/bcmdhd.cal "$stage/etc/wifi/bcmdhd.cal"
    ln -sf /mnt/system/vendor/firmware/tesla-dsp.xe64T "$stage/lib/firmware/tesla-dsp.xe64T"
    ln -sf /mnt/system/vendor/firmware/tesla-dsp.xe64T "$stage/vendor/firmware/tesla-dsp.xe64T"
    ln -sf /mnt/system/vendor/firmware/tesla-dsp.xe64T "$stage/system/vendor/firmware/tesla-dsp.xe64T"
    ln -sf /mnt/system/vendor/firmware/ducati-m3-core0.xem3 "$stage/lib/firmware/ducati-m3-core0.xem3"
    ln -sf /mnt/system/vendor/firmware/ducati-m3-core0.xem3 "$stage/vendor/firmware/ducati-m3-core0.xem3"
    ln -sf /mnt/system/vendor/firmware/ducati-m3-core0.xem3 "$stage/system/vendor/firmware/ducati-m3-core0.xem3"

    # Record build provenance inside the rootfs
    cat > "$stage/etc/glass-build-info" <<EOF
PROJECT=glass-os/muse-gadget-overlay
MILESTONE=M0
BOARD=notle (Google Glass Explorer Edition)
KERNEL_VERSION=3.4.94-stock-xe24
KERNEL_SHA256=$EXPECTED_KERNEL_SHA256
USB_RNDIS_IP=172.16.42.1
SERIAL_CONSOLE=ttyO2,115200n8
EOF

    log "Packing deterministic newc initramfs archive (injecting /dev/console and /dev/null)..."
    python3 "$OVERLAY_DIR/build/pack-cpio.py" "$stage" "$OUT_DIR/initramfs-m0.cpio.gz"
    ls -lh "$OUT_DIR/initramfs-m0.cpio.gz"
}

pack_and_verify_boot_img() {
    local out_boot="$OUT_DIR/boot-m0.img"

    log "Packing $out_boot with unmodified XE24 kernel and M0 initramfs..."
    "$WORK_DIR/bootimg" pack "$out_boot" \
        "$WORK_DIR/stock-boot/kernel" \
        "$OUT_DIR/initramfs-m0.cpio.gz" \
        --cmdline "$BOOT_CMDLINE" \
        --base "$BOOT_BASE" \
        --pagesize "$BOOT_PAGESIZE" \
        --kernel-offset "$BOOT_KERNEL_OFFSET" \
        --ramdisk-offset "$BOOT_RAMDISK_OFFSET" \
        --second-offset "$BOOT_SECOND_OFFSET" \
        --tags-offset "$BOOT_TAGS_OFFSET"

    log "Inspecting $out_boot with tools/bootimg-info.py..."
    python3 "$BOOTIMG_INFO_PY" "$out_boot" | tee "$OUT_DIR/boot-m0.info.txt"

    local img_size
    img_size="$(stat -c '%s' "$out_boot")"
    if [ "$img_size" -gt "$BOOT_PARTITION_LIMIT" ]; then
        die "boot-m0.img ($img_size bytes) exceeds 8 MiB boot partition limit ($BOOT_PARTITION_LIMIT bytes)!"
    fi

    # Unpack out/boot-m0.img into a verification directory and confirm the embedded kernel
    # is byte-for-byte identical to Google's stock XE24 kernel.
    rm -rf "$WORK_DIR/verify-boot"
    "$WORK_DIR/bootimg" unpack "$out_boot" "$WORK_DIR/verify-boot"
    local repacked_k_sha
    repacked_k_sha="$(sha256sum "$WORK_DIR/verify-boot/kernel" | awk '{print $1}')"
    [ "$repacked_k_sha" = "$EXPECTED_KERNEL_SHA256" ] || \
        die "Embedded kernel SHA-256 mismatch after repack: $repacked_k_sha"

    local boot_m0_sha
    boot_m0_sha="$(sha256sum "$out_boot" | awk '{print $1}')"
    local pct=$(( img_size * 100 / BOOT_PARTITION_LIMIT ))

    log "=================================================================="
    log "M0 BUILD SUCCEEDED"
    log "  Output image : $out_boot"
    log "  Image size   : $img_size / $BOOT_PARTITION_LIMIT bytes (${pct}% of 8 MiB boot partition)"
    log "  Image SHA256 : $boot_m0_sha"
    log "  Kernel SHA256: $repacked_k_sha (matches stock XE24 3.4.94 byte-for-byte)"
    log "  RAM boot cmd : fastboot boot $out_boot"
    log "=================================================================="
}

main() {
    check_prereqs
    verify_and_extract_factory
    build_host_tools_and_unpack_boot
    extract_firmware_blobs
    fetch_alpine_apks
    build_arm_binaries
    build_initramfs
    pack_and_verify_boot_img
}

main "$@"
