"""PlatformIO pre-build hook for RFNETHM.

For every `pio run` invocation:
  1. If the project is inside a git repo and the working tree has
     uncommitted changes, snapshot them (`git add -A; git commit`) so
     we can revert to any prior built state.  The commit message
     records the *previous* build number — i.e. the commit represents
     "what was about to be rebuilt".
  2. Increment `build_number.txt` (relative to firmware/).
  3. Regenerate `src/version.h` with `MAJOR.MINOR.BUILD` and timestamp.
     The header is included by main.c for the runtime banner.

Manual fields:
  - `version.txt`       — `MAJOR.MINOR` (one line, e.g. `0.1`).  Bump
    by hand when crossing a meaningful project phase.
  - `build_number.txt`  — auto-incremented; do not edit unless resetting.

The git repo for this project is at the parent directory
(/Public/CLAUDE/RFNETHM) — so the script discovers the actual repo
root via `git rev-parse --show-toplevel` and runs all git commands
from there.  This snapshots the whole RFNETHM tree (firmware + docs +
schematics), not just firmware/.
"""

import datetime
import os
import subprocess
import sys

Import("env")  # noqa: F821 — injected by PlatformIO

PROJECT_DIR  = env["PROJECT_DIR"]                                      # noqa: F821
VERSION_FILE = os.path.join(PROJECT_DIR, "version.txt")
BUILD_FILE   = os.path.join(PROJECT_DIR, "build_number.txt")
HEADER_FILE  = os.path.join(PROJECT_DIR, "src", "version.h")


def _read_version():
    with open(VERSION_FILE) as f:
        v = f.read().strip()
    parts = v.split(".")
    if len(parts) != 2:
        raise RuntimeError(f"version.txt must contain MAJOR.MINOR — got {v!r}")
    return int(parts[0]), int(parts[1])


def _read_build():
    if not os.path.exists(BUILD_FILE):
        return 0
    with open(BUILD_FILE) as f:
        return int((f.read().strip() or "0"))


def _atomic_write(path, content):
    """Truncate-and-write is unsafe on NFS — close may return before
    the data is durable, and a subsequent crash leaves the file
    truncated with all-zero blocks.  Write to a sibling tmp file,
    fsync, rename; rename is atomic on POSIX/NFS."""
    tmp = f"{path}.tmp.{os.getpid()}"
    with open(tmp, "w") as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
    os.rename(tmp, path)


def _write_build(n):
    _atomic_write(BUILD_FILE, f"{n}\n")


def _git_repo_root():
    r = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        cwd=PROJECT_DIR, capture_output=True, text=True, check=False,
    )
    if r.returncode != 0:
        return None
    return r.stdout.strip() or None


REPO_ROOT = _git_repo_root()


def _git_run(args, capture=False):
    return subprocess.run(
        ["git"] + args,
        cwd=REPO_ROOT or PROJECT_DIR,
        capture_output=capture,
        text=True,
        check=False,
    )


def _git_has_changes():
    r = _git_run(["status", "--porcelain"], capture=True)
    return bool(r.stdout.strip())


def _git_autocommit(prev_version_str):
    if not _git_has_changes():
        return False
    add = _git_run(["add", "-A"])
    if add.returncode != 0:
        print(f"[version_bump] git add failed (rc={add.returncode}); skip commit")
        return False
    msg = f"build snapshot v{prev_version_str}"
    cm = _git_run(["commit", "-m", msg], capture=True)
    if cm.returncode != 0:
        print(f"[version_bump] git commit skipped: {cm.stdout.strip() or cm.stderr.strip()}")
        return False
    return True


def _parse_release_tag(tag):
    """Parse a release tag like 'v0.14.138' or '0.14.138' into (MAJOR, MINOR, BUILD).

    Returns None if the tag doesn't match.  Used by release.sh to bake a
    tag-based version into the build (overriding the auto-counter), so
    factory.bin + manifest.json + firmware.bin all carry the same
    user-facing version string.
    """
    s = tag.strip().lstrip("v")
    parts = s.split(".")
    if len(parts) != 3:
        return None
    try:
        return int(parts[0]), int(parts[1]), int(parts[2])
    except ValueError:
        return None


def main():
    major, minor = _read_version()
    prev_build = _read_build()
    new_build  = prev_build + 1
    prev_version = f"{major}.{minor}.{prev_build}"

    # Release-Tag-Override (BOSE-Style):
    # Wenn RELEASE_TAG-env gesetzt ist, baut der Build mit diesem
    # Tag als FW_VERSION_STRING — counter bumpt trotzdem (für die
    # git-snapshot-Historie), aber das, was im Binary landet und
    # später in manifest.json + im /api/status erscheint, ist die
    # Tag-Version.  Damit sind alle Release-Artefakte versionsgleich.
    release_tag = os.environ.get("RELEASE_TAG", "").strip()
    parsed = _parse_release_tag(release_tag) if release_tag else None
    if release_tag and not parsed:
        raise RuntimeError(
            f"RELEASE_TAG={release_tag!r} kann ich nicht parsen — "
            "erwartet wird MAJOR.MINOR.BUILD (z.B. v0.14.138)."
        )
    if parsed:
        rt_major, rt_minor, rt_build = parsed
        fw_major, fw_minor, fw_build = rt_major, rt_minor, rt_build
        new_version = f"{rt_major}.{rt_minor}.{rt_build}"
        print(f"[version_bump] RELEASE_TAG=v{new_version} — overriding counter for FW_VERSION_*")
    else:
        fw_major, fw_minor, fw_build = major, minor, new_build
        new_version = f"{major}.{minor}.{new_build}"

    if REPO_ROOT:
        if _git_autocommit(prev_version):
            print(f"[version_bump] git snapshot committed @ v{prev_version}")
    else:
        print("[version_bump] not a git repo — skipping snapshot")

    _write_build(new_build)

    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    header = f"""// SPDX-License-Identifier: GPL-2.0-or-later
// AUTO-GENERATED by scripts/version_bump.py — do not edit by hand.
// Manual fields are version.txt (MAJOR.MINOR) and build_number.txt.
#ifndef VERSION_H
#define VERSION_H

#define FW_VERSION_MAJOR  {fw_major}
#define FW_VERSION_MINOR  {fw_minor}
#define FW_VERSION_BUILD  {fw_build}

#define FW_VERSION_STRING "{new_version}"
#define FW_BUILD_DATE     "{now}"

#endif // VERSION_H
"""
    os.makedirs(os.path.dirname(HEADER_FILE), exist_ok=True)
    _atomic_write(HEADER_FILE, header)

    print(f"[version_bump] FW v{new_version}  built {now}  (counter={new_build})")


main()
