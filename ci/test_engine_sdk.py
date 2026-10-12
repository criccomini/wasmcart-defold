"""Check actual CI command generation without invoking a platform build."""
import importlib.util
import os
from pathlib import Path
import shlex
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
spec = importlib.util.spec_from_file_location("defold_ci", HERE / "ci.py")
ci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci)


class EngineSDKTest(unittest.TestCase):
    def command(self, platform, packages_url):
        args = SimpleNamespace(verbose=False, archive=False, codesign=False,
            skip_docs=True, skip_builtins=True, skip_tests=False,
            skip_build_tests=False, with_valgrind=False, with_asan=False,
            with_ubsan=False, with_tsan=False, with_vanilla_lua=False)
        with patch.dict(os.environ, {"DM_PACKAGES_URL": packages_url}), patch.object(ci, "call") as call:
            ci.build_engine("dev", platform, args)
        call.assert_called_once()
        command = shlex.split(call.call_args.args[0])
        for operation in ("distclean", "install_ext", "check_sdk", "build_ext", "build_engine"):
            self.assertIn(operation, command)
        self.assertNotIn("--skip-tests", command)
        self.assertNotIn("--skip-build-tests", command)
        return command

    def test_windows_without_archive_uses_local_sdk_and_runs_tests(self):
        for url in ("", "  "):
            with self.subTest(url=url):
                self.assertNotIn("install_sdk", self.command("x86_64-win32", url))

    def test_windows_with_archive_preserves_packaged_sdk(self):
        self.assertIn("install_sdk", self.command("x86_64-win32", "https://packages.example.test"))

    def test_linux_local_sdk_behavior_is_unchanged(self):
        for url in ("", "https://packages.example.test"):
            with self.subTest(url=url):
                self.assertNotIn("install_sdk", self.command("x86_64-linux", url))

    def test_other_packaged_platforms_still_install_sdk(self):
        self.assertIn("install_sdk", self.command("arm64-nx64", ""))


if __name__ == "__main__":
    unittest.main()
