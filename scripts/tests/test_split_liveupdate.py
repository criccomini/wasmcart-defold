import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from split_liveupdate import MANIFEST, split


class SplitTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.graph = self.root / "game.graph.json"
        self.archive = self.root / "all.zip"
        self.output = self.root / "parts"
        self.nodes = [
            {"path": "/a.collectionc", "hexDigest": "aaa", "children": ["/shared", "/base"]},
            {"path": "/b.collectionc", "hexDigest": "bbb", "children": ["/shared"]},
            {"path": "/shared", "hexDigest": "ccc", "children": ["/a.collectionc"]},
            {"path": "/base", "hexDigest": "ddd", "isInMainBundle": True},
        ]
        self.graph.write_text(json.dumps(self.nodes))
        with zipfile.ZipFile(self.archive, "w", compression=zipfile.ZIP_DEFLATED) as out:
            for name in ("aaa", "bbb", "ccc", MANIFEST):
                out.writestr(name, b"\x05\xed\xed\xed" + name.encode())

    def run_split(self, *parts):
        with contextlib.redirect_stdout(io.StringIO()):
            split(self.graph, self.archive, self.output, parts)

    def test_dependencies_manifest_headers_and_compression(self):
        self.run_split("a=/a.collectionc", "b=/b.collectionc")
        with zipfile.ZipFile(self.archive) as source:
            for name, entries in (("a", {"aaa", "ccc", MANIFEST}),
                                  ("b", {"aaa", "bbb", "ccc", MANIFEST})):
                with zipfile.ZipFile(self.output / f"{name}.zip") as part:
                    self.assertEqual(set(part.namelist()), entries)
                    for entry in entries:
                        self.assertEqual(part.read(entry), source.read(entry))
                        self.assertEqual(part.getinfo(entry).compress_type, zipfile.ZIP_DEFLATED)

    def test_invalid_root(self):
        for root in ("/absent", "/base"):
            with self.assertRaisesRegex(ValueError, "root is not"):
                self.run_split("bad=" + root)
        self.assertFalse(self.output.exists())

    def test_missing_excluded_dependency(self):
        self.nodes[-1]["isInMainBundle"] = False
        self.graph.write_text(json.dumps(self.nodes))
        with self.assertRaisesRegex(ValueError, "excluded resource missing"):
            self.run_split("a=/a.collectionc")
        self.assertFalse(self.output.exists())

    def test_missing_graph_dependency(self):
        self.graph.write_text(json.dumps(self.nodes[:-1]))
        with self.assertRaisesRegex(ValueError, "graph has no dependency"):
            self.run_split("a=/a.collectionc")

    def test_invalid_and_duplicate_names(self):
        for parts in (("../a=/a.collectionc",), ("a",),
                      ("a=/a.collectionc", "a=/b.collectionc")):
            with self.assertRaises(ValueError):
                self.run_split(*parts)
        self.assertFalse(self.output.exists())

    def test_manifest_required(self):
        with zipfile.ZipFile(self.archive, "w") as out:
            out.writestr("aaa", b"payload")
        with self.assertRaisesRegex(ValueError, "archive has no"):
            self.run_split("a=/a.collectionc")

    def test_source_overwrite_rejected(self):
        self.output = self.root
        with self.assertRaisesRegex(ValueError, "overwrite the source"):
            self.run_split("all=/a.collectionc")
        with zipfile.ZipFile(self.archive) as source:
            self.assertIn(MANIFEST, source.namelist())


if __name__ == "__main__":
    unittest.main()
