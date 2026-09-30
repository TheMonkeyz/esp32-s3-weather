#!/usr/bin/env python3
"""Release files and the web-flasher site.

Two steps, used by CI (.github/workflows/firmware.yml) and for local previews:

  1. dist: turn a firmware build into release files
       python3 tools/make_flasher_site.py dist [--build build] [--out dist] [--version v1.2.3]
     -> bootloader.bin, partition-table.bin, weather_amoled-<version>.bin and flash-parts.json
        (chip, version, build time and the flash offset of each file)

  2. site: assemble the web flasher from one or two release folders
       python3 tools/make_flasher_site.py site --stable dist [--beta dist-beta] [--out _site]
     -> web/flash/* + stable/ (and beta/) each with its images and an ESP Web Tools manifest.json,
        and channels.json, which the page reads to show the Stable / Beta picker.

The images stay separate parts on purpose: a single merged image would also overwrite the NVS
partition (Wi-Fi credentials, location, settings, the TLS certificate) with 0xFF on every update.

Local preview (Web Serial works on localhost):
  python3 tools/make_flasher_site.py dist && python3 tools/make_flasher_site.py site --stable dist
  python3 -m http.server -d _site 8000
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAMILY = {"esp32s3": "ESP32-S3", "esp32": "ESP32", "esp32c3": "ESP32-C3"}


def git_version():
    try:
        return subprocess.check_output(["git", "describe", "--tags", "--always", "--dirty"],
                                       cwd=ROOT, text=True).strip()
    except Exception:
        return "dev"


def cmd_dist(a):
    version = a.version or git_version()
    with open(os.path.join(a.build, "flasher_args.json")) as f:
        fa = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    parts = []
    for offset, rel in sorted(fa["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
        name = os.path.basename(rel)
        if rel == fa["app"]["file"]:                       # the app gets the version in its name
            stem, ext = os.path.splitext(name)
            name = f"{stem}-{version}{ext}"
        shutil.copy2(os.path.join(a.build, rel), os.path.join(a.out, name))
        parts.append({"file": name, "offset": int(offset, 16)})
    info = {
        "chip": fa["extra_esptool_args"]["chip"],
        "version": version,
        "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "flash_settings": fa["flash_settings"],
        "parts": parts,
    }
    with open(os.path.join(a.out, "flash-parts.json"), "w") as f:
        json.dump(info, f, indent=2)
        f.write("\n")
    print(f"Release files in {a.out} ({version}):")
    for p in parts:
        print(f"  0x{p['offset']:06x}  {p['file']}")


def add_channel(out, name, src):
    with open(os.path.join(src, "flash-parts.json")) as f:
        info = json.load(f)
    d = os.path.join(out, name)
    os.makedirs(d, exist_ok=True)
    for p in info["parts"]:
        shutil.copy2(os.path.join(src, p["file"]), os.path.join(d, p["file"]))
    manifest = {
        "name": "Weather Display" + (" (beta)" if name == "beta" else ""),
        "version": info["version"],
        "new_install_prompt_erase": True,
        "builds": [{
            "chipFamily": FAMILY[info["chip"]],
            "parts": [{"path": p["file"], "offset": p["offset"]} for p in info["parts"]],  # relative to the manifest
        }],
    }
    with open(os.path.join(d, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"  {name}: {info['version']} (built {info['built']})")
    return {"version": info["version"], "built": info["built"], "manifest": f"{name}/manifest.json"}


def cmd_site(a):
    if os.path.exists(a.out):
        shutil.rmtree(a.out)
    shutil.copytree(os.path.join(ROOT, "web", "flash"), a.out)
    print(f"Site in {a.out}:")
    channels = {"stable": add_channel(a.out, "stable", a.stable), "beta": None}
    if a.beta:
        channels["beta"] = add_channel(a.out, "beta", a.beta)
    with open(os.path.join(a.out, "channels.json"), "w") as f:
        json.dump(channels, f, indent=2)
        f.write("\n")


ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
sub = ap.add_subparsers(dest="cmd", required=True)
d = sub.add_parser("dist", help="release files from a build")
d.add_argument("--build", default=os.path.join(ROOT, "build"))
d.add_argument("--out", default=os.path.join(ROOT, "dist"))
d.add_argument("--version", default=None, help="default: git describe")
s = sub.add_parser("site", help="web flasher from release folders")
s.add_argument("--stable", required=True, help="folder with flash-parts.json (a dist folder or a downloaded release)")
s.add_argument("--beta", default=None, help="same, for the beta channel (optional)")
s.add_argument("--out", default=os.path.join(ROOT, "_site"))
a = ap.parse_args()
{"dist": cmd_dist, "site": cmd_site}[a.cmd](a)
