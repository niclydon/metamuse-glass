#!/usr/bin/env python3
"""
Builds the Chibi Nova sprite packs for Google Glass (fb-hello Cards 4 & 5):
1. out/nova_sprites_full.bin: 105 frames @ 240x240 sampled directly from the native
   640x640 / 960x960 WaveSpeed renders in avatar-gen/nova/work/frames_*, with a
   soft radial edge vignette fading the outer starfield background to pure #000000
   (transparent on the Google Glass LCoS prism!).
2. out/nova_sprites_compact.bin.xz: compact 36-frame @ 128x128 pack (< 200 KB XZ) for initramfs.
"""
import glob
import json
import lzma
import math
import os
import struct
from PIL import Image

# Source art locations are machine-specific; set them in the environment.
# The sprite source renders are not distributed with this repo.
WORK_DIR = os.environ.get("NOVA_ART_WORK_DIR", "art/nova/work")
PACK_DIR = os.environ.get("NOVA_ART_PACK_DIR", "art/nova/pack")
GLANCE_FRAMES_DIR = os.environ.get("NOVA_ART_FRAMES_DIR", "art/frames")
GLANCE_GRADES_JSON = os.environ.get("NOVA_ART_GRADES_JSON", "art/speaking/grades.json")
OUT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "out")
STATES = ["idle", "listening", "working", "speaking", "happy", "sleepy", "error"]
CROP = (0.04, 0.00, 0.96, 0.92)
FULLBODY_CROP = (0.04, 0.01, 0.96, 0.97)


def crop_and_vignette(im: Image.Image, width: int, height: int) -> Image.Image:
    w, h = im.size
    if w > 128:
        box = (int(CROP[0] * w), int(CROP[1] * h), int(CROP[2] * w), int(CROP[3] * h))
        im = im.crop(box)
    im = im.convert("RGB").resize((width, height), Image.Resampling.LANCZOS)
    px = list(im.getdata())
    cx, cy = (width - 1) / 2.0, (height - 1) / 2.0
    max_r = min(cx, cy)
    inner_r = max_r * 0.80
    out_px = []
    for idx, (r, g, b) in enumerate(px):
        y = idx // width
        x = idx % width
        # Snap dark starfield background to pure #000000 (transparent on LCoS prism)
        if r < 24 and g < 24 and b < 34:
            out_px.append((0, 0, 0))
            continue
        dist = math.hypot(x - cx, y - cy)
        if dist > inner_r:
            t = max(0.0, min(1.0, (max_r - dist) / (max_r - inner_r)))
            r = int(r * t)
            g = int(g * t)
            b = int(b * t)
            if r < 16 and g < 16 and b < 22:
                r, g, b = 0, 0, 0
        out_px.append((r, g, b))
    im.putdata(out_px)
    return im


def load_native_state_frames(state: str, cnt: int, width: int, height: int) -> list[Image.Image]:
    if state == "idle" and os.path.isdir(os.path.join(WORK_DIR, "frames_idle_a")):
        files = sorted(glob.glob(os.path.join(WORK_DIR, "frames_idle_a", "f_*.png")))
        start, span = 20, 96
        picks = [files[min(len(files) - 1, start + round(i * span / cnt))] for i in range(cnt)]
        return [crop_and_vignette(Image.open(p), width, height) for p in picks]
    if state == "listening" and os.path.isdir(os.path.join(WORK_DIR, "frames_listening_b")):
        files = sorted(glob.glob(os.path.join(WORK_DIR, "frames_listening_b", "f_*.png")))
        start, span = 12, 108
        picks = [files[min(len(files) - 1, start + round(i * span / cnt))] for i in range(cnt)]
        return [crop_and_vignette(Image.open(p), width, height) for p in picks]
    if state == "working" and os.path.isdir(os.path.join(WORK_DIR, "frames_working")):
        files = sorted(glob.glob(os.path.join(WORK_DIR, "frames_working", "f_*.png")))
        start, span = 32, 84
        picks = [files[min(len(files) - 1, start + round(i * span / cnt))] for i in range(cnt)]
        return [crop_and_vignette(Image.open(p), width, height) for p in picks]
    if state == "speaking" and os.path.isdir(os.path.join(WORK_DIR, "frames_speaking")):
        grades = json.load(open(os.path.join(PACK_DIR, "speaking-grades.json")))
        files = sorted(glob.glob(os.path.join(WORK_DIR, "frames_speaking", "f_*.png")))
        idxs = [int(round(i * (len(grades) - 1) / max(1, cnt - 1))) for i in range(cnt)]
        picks = [files[grades[i]["source_frame"]] for i in idxs]
        return [crop_and_vignette(Image.open(p), width, height) for p in picks]

    # Fallback to pack/frames/<state>
    files = sorted(glob.glob(os.path.join(PACK_DIR, "frames", state, "*.png")))
    idxs = [int(round(i * (len(files) - 1) / max(1, cnt - 1))) for i in range(cnt)]
    return [crop_and_vignette(Image.open(files[i]), width, height) for i in idxs]


def crop_and_vignette_fullbody(im: Image.Image, width: int, height: int) -> Image.Image:
    w, h = im.size
    box = (
        int(FULLBODY_CROP[0] * w),
        int(FULLBODY_CROP[1] * h),
        int(FULLBODY_CROP[2] * w),
        int(FULLBODY_CROP[3] * h),
    )
    im = im.crop(box).convert("RGB").resize((width, height), Image.Resampling.LANCZOS)
    px = list(im.getdata())
    cx, cy = (width - 1) / 2.0, (height - 1) / 2.0
    out_px = []
    for idx, (r, g, b) in enumerate(px):
        y = idx // width
        x = idx % width
        dx = (x - cx) / max(1.0, cx)
        dy = (y - cy) / max(1.0, cy)
        er = math.hypot(dx, dy)
        # Snap dark starfield & blue nebula background to pure #000000 for transparent LCoS prism
        if r < 28 and g < 28 and b < 40:
            out_px.append((0, 0, 0))
            continue
        # Below y=42% (neck, jacket, arms, boots), Nova has only warm grey skin and black/gold trim;
        # any blue-tinted pixel (b - r >= 6 and r < 56) is background nebula
        if y >= int(height * 0.42) and r < 56 and g < 62 and b < 78 and (b - r) >= 6:
            out_px.append((0, 0, 0))
            continue
        if r < 42 and g < 48 and b < 68 and (b - r) >= 6 and er > 0.42:
            out_px.append((0, 0, 0))
            continue
        if er > 0.66 and r < 48 and g < 52 and b < 72:
            out_px.append((0, 0, 0))
            continue
        if er > 0.74:
            t = max(0.0, min(1.0, (1.0 - er) / 0.26))
            r = int(r * t)
            g = int(g * t)
            b = int(b * t)
            if r < 20 and g < 20 and b < 28:
                r, g, b = 0, 0, 0
        out_px.append((r, g, b))
    # Despeckle isolated 1-2px background stars outside character silhouette
    clean_px = list(out_px)
    for y in range(1, height - 1):
        row_off = y * width
        for x in range(1, width - 1):
            i = row_off + x
            if out_px[i] == (0, 0, 0):
                continue
            nz = 0
            if out_px[i - 1] != (0, 0, 0): nz += 1
            if out_px[i + 1] != (0, 0, 0): nz += 1
            if out_px[i - width] != (0, 0, 0): nz += 1
            if out_px[i + width] != (0, 0, 0): nz += 1
            if nz <= 1:
                clean_px[i] = (0, 0, 0)
    im.putdata(clean_px)
    return im


def load_fullbody_state_frames(state: str, cnt: int, width: int, height: int) -> list[Image.Image]:
    dir_map = {
        "idle": "full-idle",
        "listening": "full-listening",
        "working": "full-thinking",
        "speaking": "full-speaking",
        "happy": "full-happy",
        "sleepy": "full-sleepy",
        "error": "full-error",
    }
    sub = os.path.join(GLANCE_FRAMES_DIR, dir_map[state])
    files = sorted(glob.glob(os.path.join(sub, "f*.png")))
    if state == "speaking" and os.path.isfile(GLANCE_GRADES_JSON):
        gdata = json.load(open(GLANCE_GRADES_JSON))["rows"]
        # Prefer eyes-open rows sorted by mouth-open score (closed -> small -> mid -> wide)
        open_rows = [r for r in gdata if r.get("eyes_open", True)]
        if len(open_rows) < cnt:
            open_rows = list(gdata)
        open_rows.sort(key=lambda r: (r["score"], r["frame"]))
        idxs = [int(round(i * (len(open_rows) - 1) / max(1, cnt - 1))) for i in range(cnt)]
        picks = [files[min(len(files) - 1, max(0, open_rows[i]["source_frame"] - 1))] for i in idxs]
        return [crop_and_vignette_fullbody(Image.open(p), width, height) for p in picks]

    idxs = [int(round(i * (len(files) - 1) / max(1, cnt - 1))) for i in range(cnt)]
    return [crop_and_vignette_fullbody(Image.open(files[i]), width, height) for i in idxs]


def build_fullbody_pack(width: int, height: int, counts: list[int], n_colors: int) -> bytes:
    header = bytearray(b"NOVA" + struct.pack("<HHHHHHHHHH", width, height, *counts, 0))
    body = bytearray()
    for st, cnt in zip(STATES, counts):
        imgs = load_fullbody_state_frames(st, cnt, width, height)
        sheet = Image.new("RGB", (width * len(imgs), height))
        for i, im in enumerate(imgs):
            sheet.paste(im, (i * width, 0))
        pal_im = sheet.quantize(colors=n_colors, method=Image.Quantize.MEDIANCUT)
        pal = pal_im.getpalette()[: n_colors * 3] + [0] * ((256 - n_colors) * 3)
        for c in range(256):
            r, g, b = pal[c * 3 : c * 3 + 3]
            if r < 18 and g < 18 and b < 26:
                r, g, b = 0, 0, 0
            body += struct.pack("<BBBB", b, g, r, 255)
        for im in imgs:
            q = im.quantize(palette=pal_im, dither=Image.Dither.NONE).tobytes()
            body += q
    return bytes(header + body)


def build_pack(width: int, height: int, counts: list[int], n_colors: int, denoise_thresh: int) -> bytes:
    header = bytearray(b"NOVA" + struct.pack("<HHHHHHHHHH", width, height, *counts, 0))
    body = bytearray()
    for st, cnt in zip(STATES, counts):
        imgs = load_native_state_frames(st, cnt, width, height)
        if denoise_thresh > 0:
            raw_rgbs = [list(im.getdata()) for im in imgs]
            for f_idx in range(1, len(raw_rgbs)):
                cur = raw_rgbs[f_idx]
                prev = raw_rgbs[f_idx - 1]
                for p_idx in range(len(cur)):
                    r1, g1, b1 = cur[p_idx]
                    r0, g0, b0 = prev[p_idx]
                    if abs(r1 - r0) + abs(g1 - g0) + abs(b1 - b0) < denoise_thresh:
                        cur[p_idx] = prev[p_idx]
                imgs[f_idx].putdata(cur)

        sheet = Image.new("RGB", (width * len(imgs), height))
        for i, im in enumerate(imgs):
            sheet.paste(im, (i * width, 0))
        pal_im = sheet.quantize(colors=n_colors, method=Image.Quantize.MEDIANCUT)
        pal = pal_im.getpalette()[: n_colors * 3] + [0] * ((256 - n_colors) * 3)

        for c in range(256):
            r, g, b = pal[c * 3 : c * 3 + 3]
            if r < 18 and g < 18 and b < 26:
                r, g, b = 0, 0, 0
            body += struct.pack("<BBBB", b, g, r, 255)

        for im in imgs:
            q = im.quantize(palette=pal_im, dither=Image.Dither.NONE).tobytes()
            body += q

    return bytes(header + body)


def main() -> None:
    os.makedirs(OUT_DIR, exist_ok=True)
    full_blob = build_pack(240, 240, [24, 24, 24, 24, 3, 3, 3], 256, 0)
    full_path = os.path.join(OUT_DIR, "nova_sprites_full.bin")
    with open(full_path, "wb") as f:
        f.write(full_blob)
    print(f"[nova-sprites] Wrote {full_path} ({len(full_blob):,} bytes, 105 native frames @ 240x240)")

    compact_blob = build_pack(128, 128, [6, 6, 6, 12, 2, 2, 2], 64, 24)
    compact_xz = lzma.compress(compact_blob, preset=9 | lzma.PRESET_EXTREME)
    compact_path = os.path.join(OUT_DIR, "nova_sprites_compact.bin.xz")
    with open(compact_path, "wb") as f:
        f.write(compact_xz)
    print(
        f"[nova-sprites] Wrote {compact_path} ({len(compact_xz):,} bytes XZ / {len(compact_blob):,} bytes raw, 36 frames @ 128x128)"
    )

    if os.path.isdir(GLANCE_FRAMES_DIR):
        fb_blob = build_fullbody_pack(240, 320, [24, 24, 24, 24, 3, 3, 3], 256)
        fb_path = os.path.join(OUT_DIR, "nova_fullbody_full.bin")
        with open(fb_path, "wb") as f:
            f.write(fb_blob)
        print(f"[nova-sprites] Wrote {fb_path} ({len(fb_blob):,} bytes, 105 full-body frames @ 240x320)")


if __name__ == "__main__":
    main()
