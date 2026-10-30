#!/usr/bin/env python3
"""Build the M2/M3/M4 non-systemd Alpine armv7 runtime bundle for Google Glass.

Resolves and fetches the transitive closure of Alpine v3.21 armv7 packages for:
  - M2: wpa_supplicant, iw, curl
  - M3: dbus, bluez, bluez-deprecated, python3, py3-dbus, py3-gobject3,
        py3-cryptography, py3-websockets, plus musegadget (from
        ~/workspace/muse-gadget-sdk-latest/linux/src/musegadget)
  - M4: alsa-utils (amixer/aplay/arecord for TWL6040 bone-conduction routing)

Outputs:
  muse-gadget-overlay/out/m3-overlay.tar.gz
"""
import os
import shutil
import subprocess
import sys

OVERLAY_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
WORK_DIR = os.path.join(OVERLAY_DIR, "work")
CACHE_DIR = os.path.join(WORK_DIR, "cache")
STAGE_DIR = os.path.join(WORK_DIR, "m3-stage")
OUT_TAR = os.path.join(OVERLAY_DIR, "out", "m3-overlay.tar.gz")
MUSE_SDK_SRC = os.path.expanduser("~/workspace/muse-gadget-sdk-latest/linux/src/musegadget")
ALPINE_MIRROR = "https://dl-cdn.alpinelinux.org/alpine/v3.21"

ROOT_PKGS = [
    "wpa_supplicant",
    "iw",
    "curl",
    "alsa-utils",
    "dbus",
    "bluez",
    "bluez-deprecated",
    "python3",
    "py3-dbus",
    "py3-gobject3",
    "py3-cryptography",
    "py3-websockets",
]


def parse_index(path: str, repo: str):
    pkgs = {}
    provides = {}
    cur = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                if "P" in cur:
                    cur["repo"] = repo
                    pkgs[cur["P"]] = cur
                    provides[cur["P"]] = cur["P"]
                    for tok in cur.get("p", "").split():
                        k = tok.split("=")[0]
                        provides[k] = cur["P"]
                cur = {}
            elif ":" in line:
                k, v = line.split(":", 1)
                cur[k] = v
    return pkgs, provides


def ensure_indices():
    os.makedirs(CACHE_DIR, exist_ok=True)
    main_idx = os.path.join(CACHE_DIR, "APKINDEX")
    comm_idx = os.path.join(CACHE_DIR, "APKINDEX.community")
    if not os.path.exists(main_idx):
        tgz = os.path.join(CACHE_DIR, "APKINDEX.tar.gz")
        subprocess.run(
            ["curl", "-fSL", "-o", tgz, f"{ALPINE_MIRROR}/main/armv7/APKINDEX.tar.gz"],
            check=True,
        )
        subprocess.run(["bsdtar", "-xzf", tgz, "-C", CACHE_DIR, "APKINDEX"], check=True)
    if not os.path.exists(comm_idx):
        tgz = os.path.join(CACHE_DIR, "APKINDEX-community.tar.gz")
        subprocess.run(
            ["curl", "-fSL", "-o", tgz, f"{ALPINE_MIRROR}/community/armv7/APKINDEX.tar.gz"],
            check=True,
        )
        subprocess.run(["bsdtar", "-xzf", tgz, "-C", CACHE_DIR, "APKINDEX"], check=True)
        os.replace(main_idx, comm_idx)
        subprocess.run(
            ["bsdtar", "-xzf", os.path.join(CACHE_DIR, "APKINDEX.tar.gz"), "-C", CACHE_DIR, "APKINDEX"],
            check=True,
        )
    return main_idx, comm_idx


def main():
    main_idx, comm_idx = ensure_indices()
    p1, pr1 = parse_index(main_idx, "main")
    p2, pr2 = parse_index(comm_idx, "community")
    pkgs = {**p2, **p1}
    provides = {**pr2, **pr1}

    need = []
    seen = set()
    q = list(ROOT_PKGS)
    while q:
        r = q.pop(0)
        if r.startswith("!"):
            continue
        k = r.split("=")[0].split("<")[0].split(">")[0].split("~")[0]
        pn = provides.get(k, k)
        if pn in seen:
            continue
        if pn not in pkgs:
            raise RuntimeError(f"Unresolved dependency {r} -> {pn}")
        seen.add(pn)
        need.append(pn)
        for dep in pkgs[pn].get("D", "").split():
            q.append(dep)

    print(f"[build-m3] Resolved {len(need)} Alpine v3.21 armv7 packages.")
    if os.path.exists(STAGE_DIR):
        shutil.rmtree(STAGE_DIR)
    os.makedirs(STAGE_DIR, exist_ok=True)

    for pn in need:
        meta = pkgs[pn]
        repo = meta["repo"]
        ver = meta["V"]
        fn = f"{pn}-{ver}.apk"
        apk_path = os.path.join(CACHE_DIR, fn)
        if not os.path.exists(apk_path):
            url = f"{ALPINE_MIRROR}/{repo}/armv7/{fn}"
            print(f"[build-m3] Downloading {fn}...")
            subprocess.run(["curl", "-fSL", "-o", apk_path, url], check=True)
        subprocess.run(
            [
                "bsdtar",
                "-xzpf",
                apk_path,
                "-C",
                STAGE_DIR,
                "--exclude=.PKGINFO",
                "--exclude=.SIGN*",
                "--exclude=.pre-*",
                "--exclude=.post-*",
                "--exclude=.trigger",
            ],
            check=True,
        )

    # Install musegadget Python package from ~/workspace/muse-gadget-sdk-latest/linux/src/musegadget
    if not os.path.isdir(MUSE_SDK_SRC):
        raise RuntimeError(f"Missing Muse Gadget SDK source at {MUSE_SDK_SRC}")
    site_pkgs = os.path.join(STAGE_DIR, "usr", "lib", "python3.12", "site-packages")
    os.makedirs(site_pkgs, exist_ok=True)
    shutil.copytree(MUSE_SDK_SRC, os.path.join(site_pkgs, "musegadget"), dirs_exist_ok=True)

    # Apply kernel 3.4.94 BLE adaptations to musegadget/ble_server.py:
    #   1. Explicit D-Bus signatures on RegisterApplication/RegisterAdvertisement
    #   2. HCI LE advertising fallback (hcitool cmd 0x08 0x0006/0x0008/0x0009/0x000a)
    #      since kernel 3.4 lacks MGMT_OP_READ_ADV_FEATURES (0x003d)
    #   3. _ASSUMED_MTU = 23 matching kernel 3.4 L2CAP_CID_ATT omtu=23
    custom_ble_srv = os.path.join(OVERLAY_DIR, "src", "ble_server.py")
    if os.path.isfile(custom_ble_srv):
        shutil.copy2(custom_ble_srv, os.path.join(site_pkgs, "musegadget", "ble_server.py"))

    bin_dir = os.path.join(STAGE_DIR, "usr", "bin")
    os.makedirs(bin_dir, exist_ok=True)
    cli_path = os.path.join(bin_dir, "musegadget")
    with open(cli_path, "w") as f:
        f.write("#!/usr/bin/python3\nfrom musegadget.cli import main\nif __name__ == '__main__':\n    raise SystemExit(main())\n")
    os.chmod(cli_path, 0o755)

    # Ensure messagebus user/group helper exists in /etc/passwd and /etc/group on install
    os.makedirs(os.path.dirname(OUT_TAR), exist_ok=True)
    subprocess.run(
        ["bsdtar", "-czf", OUT_TAR, "-C", STAGE_DIR, "."],
        check=True,
    )
    sz = os.path.getsize(OUT_TAR)
    print(f"[build-m3] Built {OUT_TAR} ({sz / 1024 / 1024:.2f} MiB)")


if __name__ == "__main__":
    main()
