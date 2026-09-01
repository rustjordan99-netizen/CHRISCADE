"""PlatformIO post script: embed the next revision; commit after UF2 generation.

The counter lives outside .pio so cleaning builds never resets it. Only the
settings translation unit includes the generated header (no whole-core rebuild).
"""
import atexit
import hashlib
import json
import os
import struct
from datetime import datetime, timezone
from pathlib import Path

Import("env")


def configure(env):
    from SCons.Script import COMMAND_LINE_TARGETS, GetOption
    if GetOption("clean") or any(t in COMMAND_LINE_TARGETS for t in
            ("nobuild", "buildfs", "uploadfs", "idedata", "compiledb")):
        return
    root = Path(env.subst("$PROJECT_DIR"))
    state_file = root / "firmware_version.json"
    # Refuse concurrent builds rather than allow two firmwares to share a version.
    lock = open(root / ".firmware-version.lock", "a+b")
    lock.seek(0)
    if os.name == "nt":
        import msvcrt
        try:
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            lock.close()
            raise RuntimeError("Another CHRISCADE build is using the version counter")
    else:
        import fcntl
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    atexit.register(lock.close)
    state = json.loads(state_file.read_text(encoding="utf-8"))
    revision = state["last_generated"] + 1
    version = "v%d" % revision
    generated = root / ".generated"
    generated.mkdir(parents=True, exist_ok=True)
    header = generated / "chriscade_build_version.h"
    content = '#pragma once\n#define CHRISCADE_BUILD_VERSION "%s"\n' % version
    if not header.exists() or header.read_text() != content:
        header.write_text(content, encoding="ascii")

    def record_version(source, target, env):
        uf2 = Path(env.subst("$BUILD_DIR/${PROGNAME}.uf2"))
        data = uf2.read_bytes()
        if len(data) < 512 or len(data) % 512 or data[:4] != b"UF2\x0a":
            raise RuntimeError("UF2 missing or invalid; version counter unchanged")
        payload = b"".join(data[i + 32:i + 32 + struct.unpack_from("<I", data, i + 16)[0]]
                           for i in range(0, len(data), 512))
        if ("CHRISCADE " + version).encode("ascii") not in payload:
            raise RuntimeError("UF2 version does not match; version counter unchanged")
        state["last_generated"] = revision
        state.setdefault("builds", []).append({
            "version": version,
            "environment": env.subst("$PIOENV"),
            "utc": datetime.now(timezone.utc).isoformat(),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
        temp = state_file.with_suffix(".json.tmp")
        temp.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
        temp.replace(state_file)
        print("CHRISCADE %s: UF2 generated and version recorded" % version)

    # The platform registers its UF2 conversion on ELF first; this post script
    # appends our action after that conversion, not before it.
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", record_version)


configure(env)
