"""Private-file tests for reversible runtime deployment; no installed app touched."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[2]
WORK = Path(sys.argv.pop(1)).resolve()
WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location("manager", ROOT / "scripts/radeon/manage_open_nr_runtime.py")
manager = importlib.util.module_from_spec(spec);spec.loader.exec_module(manager)


class RuntimeInstallTests(unittest.TestCase):
    def setUp(self):
        self.root = WORK / uuid.uuid4().hex;self.root.mkdir()
        self.package = self.root / "package";self.app = self.root / "app";self.app.mkdir()
        (self.app / "veyra_qml_ui.exe").write_bytes(b"fixture")
        self.names = ["runtime/amd-nr/LmxxfNrRuntime.dll", "runtime/amd-nr/LmxxfNrRuntime-Original.dll",
                      "runtime/amd-nr/open-nr/shaders/ParityEncode.dxil"]
        files = {}
        for i, name in enumerate(self.names):
            path = self.package / name;path.parent.mkdir(parents=True, exist_ok=True);path.write_bytes(f"new-{i}".encode())
            files[name] = manager.sha(path)
            if i < 2:
                old = self.app / name;old.parent.mkdir(parents=True, exist_ok=True);old.write_bytes(f"old-{i}".encode())
        self.manifest = self.package / "open-nr-manifest.json"
        self.manifest.write_text(json.dumps({"provider": "OpenNR Vulkan", "files": files}))

    def test_apply_restore_preserves_profile_and_nvidia(self):
        sentinels = {self.app / "profile.json": b"user settings", self.app / "nvngx_dlssnr.dll": b"NVIDIA runtime"}
        for file, data in sentinels.items(): file.write_bytes(data)
        manager.apply(self.package, self.app, [])
        self.assertEqual((self.app / self.names[0]).read_bytes(), b"new-0")
        manager.restore(self.app, [])
        self.assertEqual((self.app / self.names[0]).read_bytes(), b"old-0")
        self.assertEqual((self.app / self.names[1]).read_bytes(), b"old-1")
        self.assertFalse((self.app / self.names[2]).exists())
        for file, data in sentinels.items(): self.assertEqual(file.read_bytes(), data)

    def test_running_app_refused_before_write(self):
        with self.assertRaisesRegex(RuntimeError, "running"):
            manager.apply(self.package, self.app, [(self.app / "veyra_qml_ui.exe").resolve()])
        self.assertEqual((self.app / self.names[0]).read_bytes(), b"old-0")

    def test_source_checksum_failure_before_write(self):
        (self.package / self.names[-1]).write_bytes(b"corrupted")
        with self.assertRaisesRegex(ValueError, "checksum"):
            manager.apply(self.package, self.app, [])
        self.assertEqual((self.app / self.names[0]).read_bytes(), b"old-0")

    def test_restore_does_not_overwrite_later_user_change(self):
        manager.apply(self.package, self.app, [])
        (self.app / self.names[0]).write_bytes(b"user replacement")
        with self.assertRaisesRegex(ValueError, "changed"):
            manager.restore(self.app, [])
        self.assertEqual((self.app / self.names[0]).read_bytes(), b"user replacement")
        self.assertEqual((self.app / self.names[1]).read_bytes(), b"new-1")

    def test_manifest_path_escape_rejected(self):
        manifest = json.loads(self.manifest.read_text());manifest["files"]["../outside.txt"] = "0" * 64
        self.manifest.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "Unsafe"):
            manager.apply(self.package, self.app, [])
        self.assertEqual((self.app / self.names[0]).read_bytes(), b"old-0")

    def test_restore_preserves_new_runtime_file(self):
        manager.apply(self.package, self.app, [])
        extra = self.app / "runtime/amd-nr/open-nr/user-notes.txt";extra.write_text("keep")
        manager.restore(self.app, [])
        self.assertEqual(extra.read_text(), "keep")

    def test_partial_copy_failure_is_recoverable(self):
        copy = manager.atomic_copy
        def fail_copy(source, target):
            if source == self.package / self.names[1]: raise OSError("injected copy failure")
            copy(source, target)
        manager.atomic_copy = fail_copy
        try:
            with self.assertRaisesRegex(OSError, "injected"):
                manager.apply(self.package, self.app, [])
        finally: manager.atomic_copy = copy
        self.assertEqual((self.app / self.names[0]).read_bytes(), b"old-0")
        manager.restore(self.app, [])
        self.assertEqual((self.app / self.names[1]).read_bytes(), b"old-1")


if __name__ == "__main__": unittest.main()
