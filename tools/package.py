"""Build the release zip: dist/human_ai-<version>.zip

Layout inside the zip:
  Hearts of Iron IV (game folder)/version.dll
  Documents/Paradox Interactive/Hearts of Iron IV/mod/human_ai.mod
  Documents/Paradox Interactive/Hearts of Iron IV/mod/human_ai/...   (panel scripts)
  README.md, LICENSE

Needs native/build/version.dll (run native/build.bat first).
usage: py tools/package.py
"""
import os
import re
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD_DIRS = ["common", "interface", "localisation"]
GAME = "Hearts of Iron IV (game folder)"
DOCS_MOD = "Documents/Paradox Interactive/Hearts of Iron IV/mod"


def version():
    with open(os.path.join(ROOT, "native", "src", "hai.h"), encoding="utf-8") as f:
        return re.search(r'#define HAI_VERSION "([^"]+)"', f.read()).group(1)


def main():
    ver = version()
    dll = os.path.join(ROOT, "native", "build", "version.dll")
    if not os.path.exists(dll):
        raise SystemExit("native/build/version.dll missing: run native/build.bat first")
    with open(os.path.join(ROOT, "descriptor.mod"), encoding="utf-8") as f:
        descriptor = f.read().strip()

    os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
    out = os.path.join(ROOT, "dist", f"human_ai-{ver}.zip")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(dll, f"{GAME}/version.dll")
        # The launcher resolves a relative path against the Documents game folder.
        z.writestr(f"{DOCS_MOD}/human_ai.mod", descriptor + '\npath="mod/human_ai"\n')
        z.writestr(f"{DOCS_MOD}/human_ai/descriptor.mod", descriptor + "\n")
        for d in MOD_DIRS:
            for base, _, files in os.walk(os.path.join(ROOT, d)):
                for name in files:
                    full = os.path.join(base, name)
                    z.write(full, f"{DOCS_MOD}/human_ai/" + os.path.relpath(full, ROOT).replace(os.sep, "/"))
        z.write(os.path.join(ROOT, "README.md"), "README.md")
        z.write(os.path.join(ROOT, "LICENSE"), "LICENSE")
    print("wrote", os.path.relpath(out, ROOT))


if __name__ == "__main__":
    main()
