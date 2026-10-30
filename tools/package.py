"""Build the release zip: dist/human_ai-<version>.zip

Layout inside the zip:
  Hearts of Iron IV (game folder)/version.dll    the mod itself (loaded by the game)
  Hearts of Iron IV (game folder)/HumanAI.exe    settings window (started by version.dll)
  Documents/Paradox Interactive/Hearts of Iron IV/mod/human_ai.mod   optional in-game panel,
  Documents/Paradox Interactive/Hearts of Iron IV/mod/human_ai/...   for players not using the Workshop
  README.md, LICENSE

Needs native/build/version.dll and HumanAI.exe (run native/build.bat) and workshop/
(run tools/gen_panel.py).
usage: py tools/package.py
"""
import os
import re
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = "Hearts of Iron IV (game folder)"
DOCS_MOD = "Documents/Paradox Interactive/Hearts of Iron IV/mod"


def version():
    with open(os.path.join(ROOT, "native", "src", "hai.h"), encoding="utf-8") as f:
        return re.search(r'#define HAI_VERSION "([^"]+)"', f.read()).group(1)


def main():
    ver = version()
    build = os.path.join(ROOT, "native", "build")
    workshop = os.path.join(ROOT, "workshop")
    need = [os.path.join(build, "version.dll"), os.path.join(build, "HumanAI.exe"),
            os.path.join(workshop, "descriptor.mod")]
    missing = [f for f in need if not os.path.exists(f)]
    if missing:
        raise SystemExit("missing: " + ", ".join(missing) + " (run native/build.bat and tools/gen_panel.py)")
    with open(os.path.join(workshop, "descriptor.mod"), encoding="utf-8") as f:
        descriptor = f.read().strip()

    os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
    out = os.path.join(ROOT, "dist", f"human_ai-{ver}.zip")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(need[0], f"{GAME}/version.dll")
        z.write(need[1], f"{GAME}/HumanAI.exe")
        # The launcher resolves a relative path against the Documents game folder.
        z.writestr(f"{DOCS_MOD}/human_ai.mod", descriptor + '\npath="mod/human_ai"\n')
        for base, _, files in os.walk(workshop):
            for name in files:
                full = os.path.join(base, name)
                z.write(full, f"{DOCS_MOD}/human_ai/" + os.path.relpath(full, workshop).replace(os.sep, "/"))
        z.write(os.path.join(ROOT, "README.md"), "README.md")
        z.write(os.path.join(ROOT, "LICENSE"), "LICENSE")
    print("wrote", os.path.relpath(out, ROOT))


if __name__ == "__main__":
    main()
