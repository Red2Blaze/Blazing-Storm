"""Package Blender 5.2 and Blender 3.6 versions from the same source.

Usage: python tools/blender-live-link/package_addons.py
No Blender installation required. Output ZIPs appear in dist/.
"""
from pathlib import Path
import re
import zipfile

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "blazing_live_link"
DIST = ROOT / "dist"

def build(version):
    DIST.mkdir(parents=True, exist_ok=True)
    label = "_".join(str(v) for v in version[:2])
    out = DIST / f"blazing_live_link_blender_{label}.zip"
    init_path = SOURCE / "__init__.py"
    init_text = init_path.read_text(encoding="utf-8")
    replacement = '"blender": (' + ", ".join(str(x) for x in version) + "),"
    init_text, count = re.subn(r'"blender":\s*\(\d+,\s*\d+,\s*\d+\),', replacement, init_text, count=1)
    if count != 1:
        raise RuntimeError("Blender compatibility declaration missing")
    with zipfile.ZipFile(out, mode="w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("blazing_live_link/__init__.py", init_text)
        zf.write(SOURCE / "protocol.py", "blazing_live_link/protocol.py")
    print(out)
    return out

if __name__ == "__main__":
    build((5, 2, 0))
    build((3, 6, 0))
