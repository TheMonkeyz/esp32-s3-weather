#!/usr/bin/env python3
"""Assemble the web-flasher site from a firmware build.

    python3 tools/make_flasher_site.py [--build build] [--out _site] [--version v1.2.3]

Copies web/flash/* to OUT, the three flash images from BUILD (offsets taken from
build/flasher_args.json) to OUT/firmware/, and writes OUT/manifest.json for ESP Web Tools.

The images are separate parts on purpose: a single merged image would also overwrite the
NVS partition (Wi-Fi credentials, location, settings) with 0xFF on every update.

Preview locally (Web Serial works on localhost):  python3 -m http.server -d _site 8000
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ap = argparse.ArgumentParser()
ap.add_argument("--build", default=os.path.join(ROOT, "build"))
ap.add_argument("--out", default=os.path.join(ROOT, "_site"))
ap.add_argument("--version", default=None, help="default: git describe")
args = ap.parse_args()

version = args.version
if not version:
    try:
        version = subprocess.check_output(["git", "describe", "--tags", "--always", "--dirty"],
                                          cwd=ROOT, text=True).strip()
    except Exception:
        version = "dev"

with open(os.path.join(args.build, "flasher_args.json")) as f:
    fa = json.load(f)
chip = fa["extra_esptool_args"]["chip"]            # esp32s3
family = {"esp32s3": "ESP32-S3", "esp32": "ESP32", "esp32c3": "ESP32-C3"}[chip]

if os.path.exists(args.out):
    shutil.rmtree(args.out)
shutil.copytree(os.path.join(ROOT, "web", "flash"), args.out)
os.makedirs(os.path.join(args.out, "firmware"), exist_ok=True)

parts = []
for offset, rel in sorted(fa["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
    name = os.path.basename(rel)
    shutil.copy2(os.path.join(args.build, rel), os.path.join(args.out, "firmware", name))
    parts.append({"path": f"firmware/{name}", "offset": int(offset, 16)})

manifest = {
    "name": "Weather Display",
    "version": version,
    "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
    "new_install_prompt_erase": True,
    "builds": [{"chipFamily": family, "parts": parts}],
}
with open(os.path.join(args.out, "manifest.json"), "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")

print(f"Site in {args.out}: version {version}, {family}, parts:")
for p in parts:
    size = os.path.getsize(os.path.join(args.out, p["path"]))
    print(f"  0x{p['offset']:06x}  {p['path']}  {size:,} bytes")
