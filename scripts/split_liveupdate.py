#!/usr/bin/env python3
"""Split bob's Live Update ZIP by compiled collection roots, using game.graph.json."""

import argparse
from copy import copy
import json
from pathlib import Path
import re
import zipfile

MANIFEST = "liveupdate.game.dmanifest"


def split(graph_path, archive_path, output_dir, parts):
    graph = {node["path"]: node for node in json.loads(Path(graph_path).read_text())}
    plans = []
    names = set()
    with zipfile.ZipFile(archive_path) as source:
        available = set(source.namelist())
        if MANIFEST not in available:
            raise ValueError(f"archive has no {MANIFEST}")
        for spec in parts:
            name, separator, root = spec.partition("=")
            if not separator or not re.fullmatch(r"[A-Za-z0-9_-]+", name):
                raise ValueError(f"expected NAME=/path/to/area.collectionc: {spec}")
            if name in names:
                raise ValueError(f"duplicate part name: {name}")
            names.add(name)
            if root not in graph or graph[root].get("hexDigest") not in available:
                raise ValueError(f"root is not in the Live Update ZIP: {root}")
            pending = [root]
            seen = set()
            digests = set()
            while pending:
                path = pending.pop()
                if path in seen:
                    continue
                seen.add(path)
                if path not in graph:
                    raise ValueError(f"resource graph has no dependency: {path}")
                node = graph[path]
                digest = node.get("hexDigest")
                if digest in available:
                    digests.add(digest)
                elif digest and node.get("isInMainBundle") is False:
                    raise ValueError(f"excluded resource missing from ZIP: {path} ({digest})")
                pending.extend(node.get("children", []))
            if (Path(output_dir) / f"{name}.zip").resolve() == Path(archive_path).resolve():
                raise ValueError("output would overwrite the source ZIP")
            plans.append((name, digests))

        output_dir = Path(output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)
        for name, digests in plans:
            destination = output_dir / f"{name}.zip"
            with zipfile.ZipFile(destination, "w") as output:
                for entry in sorted(digests) + [MANIFEST]:
                    # Preserve bob's resource headers, manifest and compression.
                    output.writestr(copy(source.getinfo(entry)), source.read(entry))
            print(f"{destination}: {len(digests)} resources, {destination.stat().st_size} bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("graph", help="build/default/game.graph.json")
    parser.add_argument("archive", help="bob's Live Update ZIP")
    parser.add_argument("output", help="cart staging directory, e.g. staged/parts")
    parser.add_argument("parts", nargs="+", help="NAME=/compiled/collection.collectionc")
    args = parser.parse_args()
    try:
        split(args.graph, args.archive, args.output, args.parts)
    except (ValueError, KeyError, OSError, zipfile.BadZipFile) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
