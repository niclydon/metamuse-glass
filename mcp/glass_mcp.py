#!/usr/bin/env python3
"""glass-mcp: Model Context Protocol (MCP) server for Google Glass (Vox / Nova).

Connects over Wi-Fi (set GLASS_WIFI_IP in the environment) or USB RNDIS (172.16.42.1:8080) to
`/usr/sbin/glass-agentd` (with Dropbear SSH fallback on port 22) to manage and
control the Google Glass Explorer Edition over the wire.
"""
from __future__ import annotations

import base64
import gzip
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

DEFAULT_USB_IP = os.environ.get("GLASS_USB_IP", "172.16.42.1")
DEFAULT_WIFI_IP = os.environ.get("GLASS_WIFI_IP", "")
DEFAULT_PORT = int(os.environ.get("GLASS_AGENT_PORT", "8080"))
_PRIVATE_SCREENSHOT_DIR = Path("/home/niclydon/projects/glass-os/muse-gadget-overlay/docs/screenshots")
SCREENSHOT_DIR = Path(
    os.environ.get(
        "GLASS_SCREENSHOT_DIR",
        _PRIVATE_SCREENSHOT_DIR if _PRIVATE_SCREENSHOT_DIR.is_dir() else (Path(__file__).resolve().parent.parent / "docs" / "screenshots"),
    )
)
TOKEN_PATH = Path.home() / ".config" / "glass-mcp" / "token"


def _auth_headers() -> dict:
    tok = os.environ.get("GLASS_AGENT_TOKEN", "").strip()
    if not tok and TOKEN_PATH.is_file():
        tok = TOKEN_PATH.read_text().strip()
    return {"Authorization": f"Bearer {tok}"} if tok else {}


def resolve_glass_endpoint() -> tuple[str, int]:
    env_ip = os.environ.get("GLASS_IP")
    if env_ip:
        return env_ip, DEFAULT_PORT
    candidates = [
        (DEFAULT_USB_IP, DEFAULT_PORT),
        (DEFAULT_WIFI_IP, DEFAULT_PORT),
    ]
    for ip, port in candidates:
        try:
            with urllib.request.urlopen(f"http://{ip}:{port}/health", timeout=1.5) as r:
                if r.status == 200:
                    return ip, port
        except Exception:
            continue
    return (DEFAULT_WIFI_IP or DEFAULT_USB_IP), DEFAULT_PORT


def resolve_glass_ip() -> str:
    ip, _ = resolve_glass_endpoint()
    return ip


def api_get(path: str, timeout: float = 5.0) -> bytes:
    ip, port = resolve_glass_endpoint()
    url = f"http://{ip}:{port}{path}"
    req = urllib.request.Request(url, headers=_auth_headers(), method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def api_post(path: str, payload: dict, timeout: float = 20.0) -> dict:
    ip, port = resolve_glass_endpoint()
    url = f"http://{ip}:{port}{path}"
    data = json.dumps(payload).encode("utf-8")
    headers = {"Content-Type": "application/json", **_auth_headers()}
    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


TOOLS = [
    {
        "name": "glass_status",
        "description": "Get live Google Glass telemetry: battery %, voltage, charging status, Wi-Fi/USB connectivity, audio mux route (bone-conduction vs mono earbud), active prism UI card, Chibi Nova state, transcripts, and saved recordings.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "glass_screenshot",
        "description": "Capture a live 640x360 PNG screenshot directly from the Google Glass LCoS prism framebuffer (/dev/fb0) over Wi-Fi.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "output_path": {
                    "type": "string",
                    "description": "Optional output PNG path. Defaults to docs/screenshots/live-capture-<timestamp>.png.",
                }
            },
        },
    },
    {
        "name": "glass_set_card",
        "description": "Switch the active UI mode on the Google Glass prism (7=Full-Body Chibi + HUD, 8=Full-Body Stand-Up, 9=Camera Snap + PiP, 5=Face Chibi + HUD, 6=Face Portrait, 4=Diagnostic, 0..3=Telemetry).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "card": {"type": "integer", "description": "Card index 0..9 (default 7: Full-Body Chibi + HUD)."}
            },
            "required": ["card"],
        },
    },
    {
        "name": "glass_set_hud",
        "description": "Update the text, status badge, or avatar state displayed on the Google Glass prism HUD in the wearer's field of view.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "state": {"type": "string", "description": "Status pill (e.g. READY, LISTENING, THINKING, SPEAKING)."},
                "user_text": {"type": "string", "description": "Top caller/prompt box text."},
                "reply_text": {"type": "string", "description": "Main Nova reply/notification box text."},
                "camera_active": {"type": "boolean", "description": "Show/hide the 144x81 cyan camera thumbnail inset."},
                "card": {"type": "integer", "description": "Optional card index 0..9 to switch to."},
            },
        },
    },
    {
        "name": "glass_set_brightness",
        "description": "Set the Google Glass LCoS prism display brightness (10..255, default 120).",
        "inputSchema": {
            "type": "object",
            "properties": {"brightness": {"type": "integer", "description": "Brightness value 10..255."}},
            "required": ["brightness"],
        },
    },
    {
        "name": "glass_set_audio_route",
        "description": "Switch Google Glass audio output routing between auto, bone (bone-conduction transducer), and earbud (micro-USB mono earbud + bone-conduction).",
        "inputSchema": {
            "type": "object",
            "properties": {"mode": {"type": "string", "enum": ["auto", "bone", "earbud"]}},
            "required": ["mode"],
        },
    },
    {
        "name": "glass_chime",
        "description": "Play a confirmation chime (start, think, done) on the Google Glass bone-conduction transducer / earbud.",
        "inputSchema": {
            "type": "object",
            "properties": {"kind": {"type": "string", "enum": ["start", "think", "done"]}},
        },
    },
    {
        "name": "glass_voice_turn",
        "description": "Trigger a tap-to-talk voice turn with Meta Muse (Nova) on the Google Glass (starts listening, sends if already listening, or stops playback if speaking).",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "glass_camera_snap",
        "description": "Snap a photo on the Google Glass, display it on the prism HUD alongside Chibi Nova, send it to the Meta Muse (Nova) chat, and speak Nova's description.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "prompt": {
                    "type": "string",
                    "description": "Optional custom question to ask Nova about the captured photo.",
                }
            },
        },
    },
    {
        "name": "glass_exec",
        "description": "Execute a shell command on the Google Glass over Wi-Fi and return stdout, stderr, and exit_code.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "command": {"type": "string", "description": "Shell command to run on the Glass."},
                "timeout_s": {"type": "integer", "description": "Timeout in seconds (1..60, default 15)."},
            },
            "required": ["command"],
        },
    },
    {
        "name": "glass_switch_os",
        "description": "Switch the active OS on Google Glass between GlassOS (Vox / Meta Muse Nova), NexusOS Glass (Prism / Ops Glance / ARIA), or the interactive Stage-2 Prism Boot Chooser without needing a kernel reboot.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "target": {
                    "type": "string",
                    "enum": ["vox", "nexus", "chooser"],
                    "description": "Target OS to run on the prism (`vox` = GlassOS / Nova, `nexus` = NexusOS Glass / Prism, `chooser` = interactive prism boot menu).",
                },
                "persist": {
                    "type": "boolean",
                    "description": "Persist this OS choice to `/var/lib/musegadget/default-os` on eMMC userdata across reboots.",
                },
            },
            "required": ["target"],
        },
    },
    {
        "name": "glass_ota_update",
        "description": "Push a remote OTA update over Wi-Fi/USB to Google Glass for GlassOS (`vox` fb-hello binary), NexusOS (`nexus` prismd binary), a rootfs `overlay` tarball, or a full `boot` partition image (verified against the stock XE24 3.4.94 kernel SHA-256 before writing).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "target": {
                    "type": "string",
                    "enum": ["vox", "nexus", "overlay", "boot"],
                    "description": "Component or image slot to update (`vox`, `nexus`, `overlay`, or `boot`).",
                },
                "artifact_path": {
                    "type": "string",
                    "description": "Local path on the host to the compiled binary, overlay .tar.gz, or Android boot.img.",
                },
                "reboot": {
                    "type": "boolean",
                    "description": "Whether to reboot after flashing a `boot` image (default false).",
                },
            },
            "required": ["target", "artifact_path"],
        },
    },
    {
        "name": "glass_camera_record",
        "description": "Record a multi-second video clip (1-15s, default 10s) on Google Glass, download it over Wi-Fi, mux to MP4 and GIF, and optionally send a keyframe to Meta Muse (Nova) for spoken commentary.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "duration_s": {
                    "type": "integer",
                    "description": "Clip duration in seconds (1 to 15, default 10).",
                },
                "fps": {
                    "type": "integer",
                    "description": "Frame rate (default 15 for Ducati H.264, 5 for BGRA fallback).",
                },
                "send_to_nova": {
                    "type": "boolean",
                    "description": "Whether to send a keyframe from the clip to Nova chat for spoken commentary.",
                },
                "prompt": {
                    "type": "string",
                    "description": "Optional prompt for Nova when send_to_nova is true.",
                },
            },
        },
    },
    {
        "name": "glass_prism_record",
        "description": "Record a live 640x360 video clip (.mp4 + .gif) of the Google Glass LCoS prism HUD over Wi-Fi.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "duration_s": {
                    "type": "integer",
                    "description": "Recording duration in seconds (1 to 15, default 6).",
                },
                "fps": {
                    "type": "integer",
                    "description": "Target frames per second (1 to 12, default 6).",
                },
            },
        },
    },
    {
        "name": "glass_ask_nova",
        "description": "Composite tool: Send a text prompt (and optionally snap a live camera photo first) directly into the active Meta Muse (Nova) conversation, display it on the prism HUD, stream Nova's spoken MP3 TTS reply out of the Bone-Conduction Transducer / Earbud with 24 fps Chibi lip-sync, and return Nova's reply text.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "prompt": {
                    "type": "string",
                    "description": "Question or instruction to send to Meta Muse (Nova).",
                },
                "include_camera": {
                    "type": "boolean",
                    "description": "Whether to snap a live photo from the Glass camera and attach it to the message.",
                },
            },
            "required": ["prompt"],
        },
    },
    {
        "name": "glass_notify_hud",
        "description": "Composite tool: Display a high-contrast notification banner alongside Chibi Nova on the Glass prism HUD and play an audio chime on the bone-conduction transducer / earbud.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "title": {"type": "string", "description": "Top header / sender label."},
                "message": {"type": "string", "description": "Main notification body text."},
                "chime": {"type": "string", "enum": ["start", "think", "none"], "description": "Audio chime to play (default start)."},
                "card": {"type": "integer", "description": "Optional card index to switch to (default 7: Full-Body + HUD)."},
            },
            "required": ["title", "message"],
        },
    },
    {
        "name": "glass_fetch_archives",
        "description": "List all untethered voice call turns (`turn-*`) and camera photos/clips stored on the Glass (`/var/lib/musegadget/`), and optionally pull the latest turn's audio + transcript to the host over Wi-Fi.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pull_latest": {
                    "type": "boolean",
                    "description": "Download the most recent turn directory (`question.wav`, `reply.wav`/`reply.mp3`, `transcript.txt`) to the host.",
                },
            },
        },
    },
    {
        "name": "glass_health_audit",
        "description": "Run a comprehensive battery, thermal, CPU frequency/governor, memory, and Wi-Fi/SSH security audit on Google Glass.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "glass_reboot",
        "description": "Sync filesystems and cleanly reboot the Google Glass over Wi-Fi/USB (optionally choosing which OS to boot into next: `vox`, `nexus`, or `chooser`).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "next_os": {
                    "type": "string",
                    "enum": ["vox", "nexus", "chooser"],
                    "description": "Optional OS to set as the boot target before rebooting.",
                },
            },
        },
    },
    {
        "name": "glass_settings",
        "description": "Read or update on-device Google Glass settings (volume_pct 10-100, brightness 1-255, audio_route bone/earbud, sleep_timeout_sec 0=never or 5-3600s) and persist them to /var/lib/musegadget/.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "volume_pct": {
                    "type": "integer",
                    "description": "Optional playback/chime volume percentage (10 to 100).",
                },
                "brightness": {
                    "type": "integer",
                    "description": "Optional LCoS prism backlight brightness (1 to 255).",
                },
                "audio_route": {
                    "type": "string",
                    "enum": ["bone", "earbud", "usb", "auto"],
                    "description": "Optional audio route (`bone` for bone-conduction transducer, `earbud` for micro-USB earbud).",
                },
                "sleep_timeout_sec": {
                    "type": "integer",
                    "description": "Optional off-head desk auto-sleep timeout in seconds (`0` = never sleep, `15` = 15s default).",
                },
            },
        },
    },
    {
        "name": "glass_sensors",
        "description": "Read live physical wearable sensors from Google Glass: inward-facing IR proximity & don/doff head detector (`glasshub` 4-0035: `on_head`, `proxraw`, `ambient_visible`, `ambient_ir`, `wink`), 9-axis IMU die temperature (`mpu9150`), PCB/battery thermals, and BQ27520 coulomb counter (`voltage_mv`, `current_ma`, `capacity_pct`).",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "glass_speak",
        "description": "Have Chibi Nova speak an exact text message aloud on Google Glass over the bone-conduction transducer / earbud via realtime VoiceCall TTS with live 24 fps Chibi lip-sync and prism HUD text.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "text": {
                    "type": "string",
                    "description": "Exact message for Chibi Nova to speak aloud on the Glasses.",
                },
            },
            "required": ["text"],
        },
    },
    {
        "name": "glass_wifi_heal",
        "description": "Check Google Glass Wi-Fi (`wlan0` SSID, signal dBm, gateway ping) and USB dock (`rndis0`) connectivity, and automatically reset the Broadcom BCM4330 SDIO bus (`glass-wifi down && glass-wifi up`) if `dhd_check_hang` or packet loss is detected (or if `force=true`).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "force": {
                    "type": "boolean",
                    "description": "Force a BCM4330 SDIO reset even if the gateway ping succeeds (default false).",
                },
            },
        },
    },
]


def handle_call(name: str, args: dict) -> dict:
    if name == "glass_status":
        data = json.loads(api_get("/status").decode("utf-8"))
        data["connected_ip"] = resolve_glass_ip()
        return data
    if name == "glass_screenshot":
        SCREENSHOT_DIR.mkdir(parents=True, exist_ok=True)
        out_path = args.get("output_path") or str(SCREENSHOT_DIR / f"live-capture-{int(time.time())}.png")
        png = api_get("/screenshot.png", timeout=10.0)
        Path(out_path).write_bytes(png)
        return {"ok": True, "path": out_path, "size_bytes": len(png), "connected_ip": resolve_glass_ip()}
    if name == "glass_set_card":
        return api_post("/card", {"card": int(args["card"])})
    if name == "glass_set_hud":
        return api_post("/hud", args)
    if name == "glass_set_brightness":
        return api_post("/brightness", {"brightness": int(args["brightness"])})
    if name == "glass_set_audio_route":
        return api_post("/audio-route", {"mode": args.get("mode", "auto")})
    if name == "glass_chime":
        return api_post("/chime", {"kind": args.get("kind", "start")})
    if name == "glass_voice_turn":
        return api_post("/voice-turn", {})
    if name == "glass_camera_snap":
        return api_post("/camera-snap", {"prompt": args.get("prompt", "")})
    if name == "glass_exec":
        return api_post("/exec", {"command": args["command"], "timeout_s": args.get("timeout_s", 15)})
    if name == "glass_switch_os":
        return api_post("/os/switch", {"target": args["target"], "persist": bool(args.get("persist", False))})
    if name == "glass_ota_update":
        target = args["target"]
        art = Path(args["artifact_path"]).expanduser().resolve()
        if not art.is_file():
            raise FileNotFoundError(f"Artifact not found: {art}")
        raw = art.read_bytes()
        sha = hashlib.sha256(raw).hexdigest()
        ip = resolve_glass_ip()
        if len(raw) <= 512 * 1024:
            return api_post(
                "/ota/apply",
                {
                    "target": target,
                    "sha256": sha,
                    "payload_b64": base64.b64encode(raw).decode("ascii"),
                    "reboot": bool(args.get("reboot", False)),
                },
                timeout=60.0,
            )
        remote_stage = f"/var/lib/musegadget/ota/staged-{target}.bin"
        subprocess.run(
            [
                "ssh",
                "-o",
                "StrictHostKeyChecking=no",
                "-o",
                "UserKnownHostsFile=/dev/null",
                f"root@{ip}",
                f"mkdir -p /var/lib/musegadget/ota && cat > {remote_stage}",
            ],
            input=raw,
            check=True,
        )
        return api_post(
            "/ota/apply",
            {
                "target": target,
                "sha256": sha,
                "staged_path": remote_stage,
                "reboot": bool(args.get("reboot", False)),
            },
            timeout=90.0,
        )
    if name == "glass_camera_record":
        duration_s = max(1, min(15, int(args.get("duration_s", 10))))
        fps = max(1, min(24, int(args.get("fps", 15))))
        res = api_post("/camera-record", {"duration_s": duration_s, "fps": fps}, timeout=duration_s + 30.0)
        if not res.get("ok") or not res.get("remote_path"):
            return res
        remote_path = res["remote_path"]
        raw_clip = api_get(f"/download?path={urllib.parse.quote(remote_path)}", timeout=30.0)
        SCREENSHOT_DIR.mkdir(parents=True, exist_ok=True)
        ts = int(time.time())
        mp4_out = SCREENSHOT_DIR / f"camera-clip-{ts}.mp4"
        gif_out = SCREENSHOT_DIR / f"camera-clip-{ts}.gif"
        tmp_in = Path(f"/tmp/glass-clip-{ts}.bin")
        if remote_path.endswith(".gz"):
            raw_clip = gzip.decompress(raw_clip)
        tmp_in.write_bytes(raw_clip)
        actual_fps = int(res.get("fps", 5))
        if res.get("mode") == "ducati-h264":
            subprocess.run(
                ["ffmpeg", "-y", "-f", "h264", "-r", str(actual_fps), "-i", str(tmp_in), "-pix_fmt", "yuv420p", str(mp4_out)],
                capture_output=True,
                check=True,
            )
        else:
            subprocess.run(
                [
                    "ffmpeg",
                    "-y",
                    "-f",
                    "rawvideo",
                    "-pix_fmt",
                    "bgra",
                    "-s",
                    "640x360",
                    "-r",
                    str(actual_fps),
                    "-i",
                    str(tmp_in),
                    "-c:v",
                    "libx264",
                    "-pix_fmt",
                    "yuv420p",
                    str(mp4_out),
                ],
                capture_output=True,
                check=True,
            )
        subprocess.run(
            ["ffmpeg", "-y", "-i", str(mp4_out), "-vf", f"fps={min(actual_fps, 10)},scale=640:-1:flags=lanczos", str(gif_out)],
            capture_output=True,
            check=True,
        )
        tmp_in.unlink(missing_ok=True)
        res["local_mp4"] = str(mp4_out)
        res["local_gif"] = str(gif_out)
        if args.get("send_to_nova"):
            prompt = args.get("prompt") or f"I just recorded a {duration_s}-second clip on my Google Glass. Tell me what you see!"
            res["nova_triggered"] = api_post("/camera-snap", {"prompt": prompt})
        return res
    if name == "glass_prism_record":
        duration_s = max(1, min(15, int(args.get("duration_s", 6))))
        fps = max(1, min(12, int(args.get("fps", 6))))
        total_frames = duration_s * fps
        interval = 1.0 / fps
        SCREENSHOT_DIR.mkdir(parents=True, exist_ok=True)
        ts = int(time.time())
        tmp_bgra = Path(f"/tmp/prism-rec-{ts}.bgra")
        with open(tmp_bgra, "wb") as f:
            for _ in range(total_frames):
                t0 = time.time()
                f.write(api_get("/screenshot.bgra", timeout=5.0))
                dt = time.time() - t0
                if dt < interval:
                    time.sleep(interval - dt)
        mp4_out = SCREENSHOT_DIR / f"prism-clip-{ts}.mp4"
        gif_out = SCREENSHOT_DIR / f"prism-clip-{ts}.gif"
        subprocess.run(
            [
                "ffmpeg",
                "-y",
                "-f",
                "rawvideo",
                "-pix_fmt",
                "bgra",
                "-s",
                "640x360",
                "-r",
                str(fps),
                "-i",
                str(tmp_bgra),
                "-c:v",
                "libx264",
                "-pix_fmt",
                "yuv420p",
                str(mp4_out),
            ],
            capture_output=True,
            check=True,
        )
        subprocess.run(
            ["ffmpeg", "-y", "-i", str(mp4_out), "-vf", f"fps={fps},scale=640:-1:flags=lanczos", str(gif_out)],
            capture_output=True,
            check=True,
        )
        tmp_bgra.unlink(missing_ok=True)
        return {"ok": True, "frames": total_frames, "fps": fps, "local_mp4": str(mp4_out), "local_gif": str(gif_out)}
    if name == "glass_ask_nova":
        return api_post(
            "/ask-nova",
            {"prompt": args["prompt"], "include_camera": bool(args.get("include_camera", False))},
            timeout=50.0,
        )
    if name == "glass_notify_hud":
        card = int(args.get("card", 7))
        chime = args.get("chime", "start")
        hud_res = api_post(
            "/hud",
            {
                "card": card,
                "state": "NOTIFY",
                "user_text": str(args["title"]),
                "reply_text": str(args["message"]),
            },
        )
        if chime and chime != "none":
            api_post("/chime", {"kind": chime})
        return hud_res
    if name == "glass_fetch_archives":
        st = json.loads(api_get("/status").decode("utf-8"))
        recs = st.get("recordings", [])
        photos = st.get("photos", [])
        pulled = {}
        if args.get("pull_latest", True) and recs:
            latest = recs[-1]
            out_dir = Path(os.environ.get("GLASS_ARCHIVE_DIR", Path(__file__).resolve().parent.parent / "docs" / "audio")) / latest
            out_dir.mkdir(parents=True, exist_ok=True)
            for fname in ("transcript.txt", "question.wav", "reply.wav", "reply.mp3"):
                try:
                    raw = api_get(f"/download?path={urllib.parse.quote(f'/var/lib/musegadget/recordings/{latest}/{fname}')}", timeout=10.0)
                    (out_dir / fname).write_bytes(raw)
                    pulled[fname] = str(out_dir / fname)
                except Exception:
                    pass
        return {"ok": True, "recordings": recs, "photos": photos, "pulled_latest": pulled}
    if name == "glass_health_audit":
        data = json.loads(api_get("/health-audit").decode("utf-8"))
        data["connected_ip"] = resolve_glass_ip()
        return data
    if name == "glass_reboot":
        next_os = args.get("next_os")
        if next_os in ("vox", "nexus", "chooser"):
            api_post("/os/switch", {"target": next_os, "persist": True})
        api_post("/exec", {"command": "nohup /bin/sh -c 'sync; sleep 0.3; /sbin/reboot -f' >/dev/null 2>&1 </dev/null &"})
        return {"ok": True, "rebooting": True, "next_os": next_os or "default"}
    if name == "glass_settings":
        if not args:
            return json.loads(api_get("/settings").decode("utf-8"))
        return api_post("/settings", args)
    if name == "glass_sensors":
        return json.loads(api_get("/sensors").decode("utf-8"))
    if name == "glass_speak":
        return api_post("/speak", {"text": args["text"]}, timeout=40.0)
    if name == "glass_wifi_heal":
        return api_post("/wifi/heal", {"force": bool(args.get("force", False))}, timeout=15.0)
    raise ValueError(f"Unknown tool: {name}")


def main() -> None:
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        msg = json.loads(line)
        mid = msg.get("id")
        method = msg.get("method")
        if method == "initialize":
            reply = {
                "jsonrpc": "2.0",
                "id": mid,
                "result": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "glass-mcp", "version": "0.1.0"},
                },
            }
        elif method == "notifications/initialized":
            continue
        elif method == "tools/list":
            reply = {"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS}}
        elif method == "tools/call":
            params = msg.get("params") or {}
            try:
                res = handle_call(params.get("name", ""), params.get("arguments") or {})
                reply = {
                    "jsonrpc": "2.0",
                    "id": mid,
                    "result": {"content": [{"type": "text", "text": json.dumps(res, indent=2)}]},
                }
            except Exception as exc:
                reply = {
                    "jsonrpc": "2.0",
                    "id": mid,
                    "result": {
                        "isError": True,
                        "content": [{"type": "text", "text": f"{type(exc).__name__}: {exc}"}],
                    },
                }
        else:
            reply = {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": f"Method not found: {method}"}}
        sys.stdout.write(json.dumps(reply) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
