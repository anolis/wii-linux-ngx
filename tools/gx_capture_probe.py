#!/usr/bin/env python3
#
# Probe HDMI/V4L2 capture modes for Wii GX diagnostics.
#
# The script intentionally uses only Python stdlib plus ffmpeg/v4l2-ctl so it
# can run on the host without installing OpenCV or image libraries.

import argparse
import csv
import glob
import os
import re
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_CANDIDATES = [
    ("mjpeg", 2560, 1600, "50"),
    ("mjpeg", 2560, 1600, "30"),
    ("mjpeg", 1920, 1080, "50"),
    ("mjpeg", 1920, 1080, "30"),
    ("mjpeg", 1280, 720, "60"),
    ("mjpeg", 720, 480, "60000/1001"),
    ("mjpeg", 720, 480, "60"),
    ("mjpeg", 640, 480, "60"),
    ("mjpeg", 640, 480, "30000/1001"),
    ("mjpeg", 640, 480, "30"),
    ("yuyv422", 720, 480, "60000/1001"),
    ("yuyv422", 720, 480, "60"),
    ("yuyv422", 640, 480, "60"),
    ("yuyv422", 640, 480, "30000/1001"),
    ("yuyv422", 640, 480, "30"),
]


def run(cmd, timeout=5):
    return subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )


def discover_devices():
    return sorted(glob.glob("/dev/video*"))


def v4l2_formats(device):
    proc = run(["v4l2-ctl", "--device", device, "--list-formats-ext"], timeout=5)
    text = (proc.stdout + proc.stderr).decode("utf-8", "replace")
    formats = []
    current_fmt = None
    current_size = None

    for line in text.splitlines():
        fmt = re.search(r"\[\d+\]: '([^']+)'", line)
        if fmt:
            fourcc = fmt.group(1).strip().lower()
            if fourcc == "mjpg":
                fourcc = "mjpeg"
            elif fourcc == "yuyv":
                fourcc = "yuyv422"
            current_fmt = fourcc
            current_size = None
            continue

        size = re.search(r"Size:\s+Discrete\s+(\d+)x(\d+)", line)
        if size and current_fmt:
            current_size = (int(size.group(1)), int(size.group(2)))
            continue

        fps = re.search(r"Interval:\s+Discrete\s+[^()]+ \(([\d.]+) fps\)", line)
        if fps and current_fmt and current_size:
            fps_float = float(fps.group(1))
            fr = "60000/1001" if abs(fps_float - 59.94) < 0.1 else str(int(round(fps_float)))
            formats.append((current_fmt, current_size[0], current_size[1], fr))

    return formats, text


def unique(seq):
    seen = set()
    out = []
    for item in seq:
        if item not in seen:
            seen.add(item)
            out.append(item)
    return out


def capture_raw(device, fmt, width, height, framerate, timeout_s):
    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-f",
        "v4l2",
        "-input_format",
        fmt,
        "-video_size",
        f"{width}x{height}",
        "-framerate",
        framerate,
        "-i",
        device,
        "-frames:v",
        "1",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "pipe:1",
    ]
    return run(cmd, timeout=timeout_s)


def score_rgb(raw, width, height):
    if len(raw) != width * height * 3:
        raise ValueError(f"short frame: got {len(raw)}, expected {width * height * 3}")

    n = width * height
    mean_y = 0.0
    m2_y = 0.0
    mean_r = 0.0
    mean_g = 0.0
    mean_b = 0.0
    diff_sum = 0.0
    diff_n = 0
    prev_y = None

    # Sample every fourth pixel.  Full-frame stats are unnecessary for mode
    # ranking and make slow USB capture cards more annoying to test.
    step_px = 4
    sample_n = 0
    for p in range(0, n, step_px):
        i = p * 3
        r = raw[i]
        g = raw[i + 1]
        b = raw[i + 2]
        y = 0.2126 * r + 0.7152 * g + 0.0722 * b
        sample_n += 1

        delta = y - mean_y
        mean_y += delta / sample_n
        m2_y += delta * (y - mean_y)
        mean_r += (r - mean_r) / sample_n
        mean_g += (g - mean_g) / sample_n
        mean_b += (b - mean_b) / sample_n

        if prev_y is not None:
            diff_sum += abs(y - prev_y)
            diff_n += 1
        prev_y = y

    var_y = m2_y / max(1, sample_n - 1)
    std_y = var_y ** 0.5
    mean_diff = diff_sum / max(1, diff_n)
    nonblank = 1.0 if 8.0 < mean_y < 247.0 else 0.0

    # Prefer modes that produce a nonblank, high-contrast, stable-looking
    # picture.  Current GX diagnostics may be mostly green/noisy, so do not
    # overfit to text edges or exact colours.
    quality = (nonblank * 1000.0) + std_y + (mean_diff * 0.25)

    return {
        "mean_y": mean_y,
        "std_y": std_y,
        "mean_diff": mean_diff,
        "mean_r": mean_r,
        "mean_g": mean_g,
        "mean_b": mean_b,
        "quality": quality,
    }


def write_ppm(path, raw, width, height):
    with open(path, "wb") as f:
        f.write(f"P6\n{width} {height}\n255\n".encode("ascii"))
        f.write(raw)


def probe(args):
    devices = [args.device] if args.device else discover_devices()
    if not devices:
        print("No /dev/video* devices found. Plug in the HDMI capture card and retry.", file=sys.stderr)
        return 2

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    rows = []

    for device in devices:
        fmts, listing = v4l2_formats(device)
        listing_path = out_dir / f"{Path(device).name}-formats.txt"
        listing_path.write_text(listing, encoding="utf-8")

        if args.mode:
            fmt, size, framerate = args.mode.split(":")
            width, height = size.lower().split("x")
            candidates = [(fmt, int(width), int(height), framerate)]
        else:
            candidates = unique(fmts + DEFAULT_CANDIDATES) if args.all_formats else unique(DEFAULT_CANDIDATES + fmts)
        print(f"{device}: trying {len(candidates)} candidate modes")

        for fmt, width, height, framerate in candidates:
            for frame_idx in range(args.frames):
                suffix = f"_{frame_idx:03d}" if args.frames > 1 else ""
                label = f"{Path(device).name}_{fmt}_{width}x{height}_{framerate.replace('/', '-')}{suffix}"
                started = time.time()
                try:
                    proc = capture_raw(device, fmt, width, height, framerate, args.timeout)
                except subprocess.TimeoutExpired:
                    row = {
                        "device": device,
                        "format": fmt,
                        "width": width,
                        "height": height,
                        "framerate": framerate,
                        "ok": 0,
                        "quality": 0,
                        "error": "timeout",
                    }
                    rows.append(row)
                    print(f"  FAIL {label}: timeout")
                    break

                elapsed = time.time() - started
                if proc.returncode != 0:
                    err = proc.stderr.decode("utf-8", "replace").strip().splitlines()[-1:]
                    row = {
                        "device": device,
                        "format": fmt,
                        "width": width,
                        "height": height,
                        "framerate": framerate,
                        "ok": 0,
                        "quality": 0,
                        "error": err[0] if err else f"ffmpeg exit {proc.returncode}",
                    }
                    rows.append(row)
                    print(f"  FAIL {label}: {row['error']}")
                    break

                try:
                    stats = score_rgb(proc.stdout, width, height)
                except ValueError as exc:
                    row = {
                        "device": device,
                        "format": fmt,
                        "width": width,
                        "height": height,
                        "framerate": framerate,
                        "ok": 0,
                        "quality": 0,
                        "error": str(exc),
                    }
                    rows.append(row)
                    print(f"  FAIL {label}: {exc}")
                    break

                frame_path = out_dir / f"{label}.ppm"
                write_ppm(frame_path, proc.stdout, width, height)
                row = {
                    "device": device,
                    "format": fmt,
                    "width": width,
                    "height": height,
                    "framerate": framerate,
                    "ok": 1,
                    "quality": round(stats["quality"], 3),
                    "mean_y": round(stats["mean_y"], 3),
                    "std_y": round(stats["std_y"], 3),
                    "mean_diff": round(stats["mean_diff"], 3),
                    "mean_r": round(stats["mean_r"], 3),
                    "mean_g": round(stats["mean_g"], 3),
                    "mean_b": round(stats["mean_b"], 3),
                    "elapsed_s": round(elapsed, 3),
                    "frame": str(frame_path),
                    "error": "",
                }
                rows.append(row)
                print(
                    "  OK   {label}: q={quality:.1f} y={mean_y:.1f} std={std_y:.1f} "
                    "rgb=({mean_r:.1f},{mean_g:.1f},{mean_b:.1f})".format(label=label, **stats)
                )
                if args.interval and frame_idx + 1 < args.frames:
                    time.sleep(args.interval)

    csv_path = out_dir / "capture-probe.csv"
    fields = [
        "device",
        "format",
        "width",
        "height",
        "framerate",
        "ok",
        "quality",
        "mean_y",
        "std_y",
        "mean_diff",
        "mean_r",
        "mean_g",
        "mean_b",
        "elapsed_s",
        "frame",
        "error",
    ]
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    good = [r for r in rows if r.get("ok") == 1]
    good.sort(key=lambda r: float(r["quality"]), reverse=True)
    print(f"\nWrote {csv_path}")
    if good:
        print("Best modes:")
        for row in good[:5]:
            print(
                "  {device} {format} {width}x{height}@{framerate} "
                "quality={quality} frame={frame}".format(**row)
            )
        return 0

    print("No working capture modes found.", file=sys.stderr)
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="V4L2 device, e.g. /dev/video0")
    parser.add_argument("--out-dir", default="/tmp/gx-capture-probe", help="output directory")
    parser.add_argument("--timeout", type=float, default=4.0, help="seconds per ffmpeg attempt")
    parser.add_argument("--all-formats", action="store_true", help="try listed formats before defaults")
    parser.add_argument("--mode", help="single mode as format:WIDTHxHEIGHT:fps, e.g. mjpeg:2560x1600:50")
    parser.add_argument("--frames", type=int, default=1, help="frames to capture per mode")
    parser.add_argument("--interval", type=float, default=0.0, help="seconds between repeated frames")
    args = parser.parse_args()
    return probe(args)


if __name__ == "__main__":
    sys.exit(main())
