"""Empaqueta libSodiumLevi.so como .levipack (zip con manifest.json) para LeviLauncher."""
import argparse
import json
import zipfile
from pathlib import Path



def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--library", required=True, type=Path)
    p.add_argument("--icon", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--name", default="SodiumLevi")
    p.add_argument("--version", default="0.2.0")
    p.add_argument("--description", default="Optimizacion para Minecraft Bedrock: modos de rendimiento, control de CPU/termico y resolucion. Hecho por erickzgcs.")
    a = p.parse_args()

    for f in (a.library, a.icon):
        if not f.is_file():
            raise FileNotFoundError(f)

    manifest = {
        "type": "preload-native",
        "name": a.name,
        "author": "erickzgcs",
        "description": a.description,
        "version": a.version,
        "entry": a.library.name,
        "icon": "icon.png",
        "overwrite_files": ["icon.png"],
        "overwrite_folders": [],
    }

    a.output.parent.mkdir(parents=True, exist_ok=True)
    if a.output.exists():
        a.output.unlink()
    with zipfile.ZipFile(a.output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
        z.write(a.library, a.library.name)
        z.write(a.icon, "icon.png")
    print(a.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
