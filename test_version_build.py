"""Exercise the real hook without generating firmware or spending revisions."""
import json
import runpy
import struct
import subprocess
import sys
import tempfile
import types
from pathlib import Path

hook = Path(__file__).with_name("version_build.py")
if len(sys.argv) > 1:
    root, mode = Path(sys.argv[1]), sys.argv[2]
    scons = types.ModuleType("SCons")
    script = types.ModuleType("SCons.Script")
    script.COMMAND_LINE_TARGETS = ["nobuild"] if mode == "nobuild" else []
    script.GetOption = lambda option: mode == "clean"
    sys.modules.update({"SCons": scons, "SCons.Script": script})
    class Env:
        def subst(self, text):
            for key, value in {"$PROJECT_DIR": str(root), "$BUILD_DIR": str(root / "build"),
                               "${PROGNAME}": "firmware", "$PIOENV": "pico"}.items():
                text = text.replace(key, value)
            return text
        def Append(self, **kwargs):
            pass
        def AddPostAction(self, target, action):
            self.action = action
    env = Env()
    runpy.run_path(str(hook), init_globals={"env": env, "Import": lambda name: None})
    if mode == "success":
        state = json.loads((root / "firmware_version.json").read_text())
        revision = state["last_generated"] + 1
        # Put the string across two UF2 payloads to check block reconstruction.
        payload = b"x" * 250 + ("CHRISCADE v%d" % revision).encode()
        data = bytearray()
        for offset in (0, 256):
            block = bytearray(512)
            block[:4] = b"UF2\x0a"
            struct.pack_into("<I", block, 16, 256)
            part = payload[offset:offset + 256].ljust(256, b"\0")
            block[32:288] = part
            data.extend(block)
        (root / "build").mkdir(exist_ok=True)
        (root / "build/firmware.uf2").write_bytes(data)
        env.action(None, None, env)
    sys.exit(0)

with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)
    state = root / "firmware_version.json"
    state.write_text('{"last_generated":264,"builds":[]}')
    for mode, expected in [("failed",264), ("clean",264), ("nobuild",264),
                           ("success",265), ("failed",265), ("success",266)]:
        subprocess.run([sys.executable, __file__, folder, mode], check=True)
        assert json.loads(state.read_text())["last_generated"] == expected
    assert len(json.loads(state.read_text())["builds"]) == 2
print("PASS: increments, failure retry, clean/nobuild, history, split UF2 payload")
