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
        channels.json, which the page reads to show the Stable / Beta picker, and notes.json, the
        release notes from CHANGELOG.md, which the display shows before installing an update.

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
import re
import shutil
import subprocess
import sys

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


def read_parts(src):
    """flash-parts.json, or for releases made before it existed (v1.0.0), the standard file names."""
    path = os.path.join(src, "flash-parts.json")
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    apps = [n for n in os.listdir(src) if n.startswith("weather_amoled-") and n.endswith(".bin")
            and not n.endswith("-full.bin")]
    if len(apps) != 1:
        sys.exit(f"{src}: no flash-parts.json and {len(apps)} app images")
    return {
        "chip": "esp32s3",
        "version": apps[0][len("weather_amoled-"):-len(".bin")],
        "built": datetime.datetime.fromtimestamp(os.path.getmtime(os.path.join(src, apps[0])),
                                                 datetime.timezone.utc).strftime("%Y-%m-%d"),
        "parts": [{"file": "bootloader.bin", "offset": 0x0},
                  {"file": "partition-table.bin", "offset": 0x8000},
                  {"file": apps[0], "offset": 0x10000}],
    }


def add_channel(out, name, src):
    info = read_parts(src)
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


def read_changelog(path, keep=15):
    """CHANGELOG.md -> [{"version", "date", "notes": [str]}], newest first.

    Sections start with "## vX.Y.Z" (optionally followed by " - YYYY-MM-DD" or " — YYYY-MM-DD"), items with
    "- ". Markdown emphasis and code marks are dropped: the display shows plain text."""
    out = []
    if not os.path.exists(path):
        return out
    for line in open(path, encoding="utf-8"):
        line = line.rstrip()
        m = re.match(r"^##\s+(v\d+\.\d+\.\d+\S*)(?:\s+[-\u2014\u2013]\s+(\d{4}-\d{2}-\d{2}))?", line)
        if m:
            out.append({"version": m.group(1), "date": m.group(2) or "", "notes": []})
        elif out and re.match(r"^\s*[-*]\s+", line):
            out[-1]["notes"].append(re.sub(r"^\s*[-*]\s+", "", line))
        elif out and out[-1]["notes"] and line.startswith("  ") and line.strip():
            out[-1]["notes"][-1] += " " + line.strip()          # continuation of the previous item
    for r in out:
        r["notes"] = [re.sub(r"[*_`#]", "", n).strip() for n in r["notes"]]
    return out[:keep]


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
    notes = read_changelog(a.changelog)
    with open(os.path.join(a.out, "notes.json"), "w", encoding="utf-8") as f:
        json.dump({"releases": notes}, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print(f"  notes.json: {len(notes)} releases from {os.path.relpath(a.changelog, ROOT)}")


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
s.add_argument("--changelog", default=os.path.join(ROOT, "CHANGELOG.md"), help="release notes source")
a = ap.parse_args()
{"dist": cmd_dist, "site": cmd_site}[a.cmd](a)
