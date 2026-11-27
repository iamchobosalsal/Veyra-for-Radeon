"""Synthetic donor tests: required FSR files survive the private-data filter."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts/radeon"
spec = importlib.util.spec_from_file_location("package_release", SCRIPTS / "package_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
spec = importlib.util.spec_from_file_location("prepare_preview", SCRIPTS / "prepare_local_preview.py")
preview = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, {"package_release": release}):
    spec.loader.exec_module(preview)


class PreviewDependencyTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="radeon-preview-plan-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.donor, self.source, self.build = [self.root / name for name in ("donor", "source", "build")]
        for root in (self.donor, self.source, self.build):
            root.mkdir()
        for name in (*preview.ROOT_DLLS, "Qt6Core.dll", "qt.conf", "qml/Veyra/Main.qml",
                     "qml/Veyra/DonorExtra.qml", "assets/logo.png", "licenses/AMD_FIDELITYFX_LICENSE.txt",
                     *release.FSR_RUNTIME_FILES):
            self.put(self.donor, name)
        for directory in preview.PLUGIN_DIRS:
            self.put(self.donor, directory + "/plugin.dll")
        for name in preview.NR_FILES:
            self.put(self.donor, "runtime/amd-nr/" + name)
        for name in preview.RIFE_FILES:
            self.put(self.donor, "runtime/experimental/anr/" + name)
        for name in ("qml/Veyra/Main.qml", "LICENSE", "THIRD_PARTY_NOTICES.md", "README-RADEON.ko.md",
                     "RELEASE-RADEON.ko.md", "docs/radeon/PACKAGING.md", "i18n/catalog.json"):
            self.put(self.source, name, b"source wins")
        self.put(self.build, release.PLAYER)
        self.put(self.build, "shaders/PresentBlit_ps.dxil")
        self.put(self.donor, "runtime_local/user-data-2.0.0/profile.json", b"private")
        self.put(self.donor, "runtime_local/amd/fidelityfx/unknown.dll", b"unapproved")
        self.put(self.donor, "runtime/amd-nr/assets/model.f16", b"local model")
        self.patch = patch.object(preview, "PROVIDER_SHA256", release.digest(self.donor / release.PROVIDER))
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def put(self, root, relative, data=b"fixture"):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def test_plan_keeps_fsr_and_not_profiles_or_unknown_runtimes(self):
        plan = preview.build_plan(self.donor, self.source, self.build)
        self.assertEqual({name for name in plan if name.startswith("runtime_local/")}, set(release.FSR_RUNTIME_FILES))
        self.assertIn("licenses/AMD_FIDELITYFX_LICENSE.txt", plan)
        self.assertNotIn("runtime/amd-nr/assets/model.f16", plan)
        self.assertEqual(plan["qml/Veyra/Main.qml"], self.source / "qml/Veyra/Main.qml")
        self.assertIn("qml/Veyra/DonorExtra.qml", plan)

    def test_missing_each_fsr_dll_prevents_preparation(self):
        for name in release.FSR_RUNTIME_FILES:
            path = self.donor / name
            data = path.read_bytes()
            with self.subTest(missing=name):
                path.unlink()
                with self.assertRaisesRegex(ValueError, "required input missing"):
                    preview.build_plan(self.donor, self.source, self.build)
                path.write_bytes(data)


if __name__ == "__main__":
    unittest.main()
