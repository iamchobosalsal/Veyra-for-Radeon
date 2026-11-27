"""Isolated package boundary tests; no real runtime, model, key or network used."""
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

SPEC = importlib.util.spec_from_file_location("package_release", Path(__file__).resolve().parents[2] / "scripts/radeon/package_release.py")
pack = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pack)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="radeon-package-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stage, self.build, self.output = (self.root / x for x in ("stage", "build", "output"))
        self.stage.mkdir(); self.build.mkdir()
        self.source = self.root / "Veyra-Radeon-1.0.0-source.zip"
        self.source.write_bytes(b"test corresponding source")
        self.trust = dict(radeonFork=True, repository="example/radeon-fork", publicKey="ab" * 32, installEnabled=True, testBuild=False)
        self.write_trust()
        self.audit = dict(product=pack.PRODUCT, gpuPackage="AMD", version="1.0.0-radeon",
                          releaseReady=True, localOnly=False, radeonRedistributionReviewed=True,
                          correspondingSource=dict(asset=self.source.name, sha256=pack.digest(self.source)), files=[])
        for name in (pack.PLAYER, "Qt6Core.dll", "platforms/qwindows.dll", "qml/Veyra/Main.qml", "LICENSE", "THIRD_PARTY_NOTICES.md", *pack.FSR_RUNTIME_FILES):
            self.add_file(name)
        (self.build / pack.PLAYER).write_bytes((self.stage / pack.PLAYER).read_bytes())
        (self.build / pack.WORKER).write_bytes(b"test worker")
        (self.build / "veyra_portable_launcher.exe").write_bytes(b"test launcher")
        self.audit["buildArtifacts"] = {name: dict(sha256=pack.digest(self.build / actual), size=(self.build / actual).stat().st_size)
                                       for name, actual in (("Veyra.exe", "veyra_portable_launcher.exe"), (pack.WORKER, pack.WORKER))}
        self.write_audit()

    def add_file(self, name, contents=b"fixture"):
        path = self.stage / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        row = dict(path=name, size=len(contents), sha256=hashlib.sha256(contents).hexdigest(),
                   redistribution=dict(reviewed=True, license="test-fixture", origin="local test"))
        self.audit["files"].append(row)
        return row

    def write_trust(self):
        (self.build / "portable-update-trust.json").write_bytes(pack.encode(self.trust))

    def write_audit(self):
        (self.stage / "package-manifest.json").write_bytes(pack.encode(self.audit))

    def wrap(self, review=False, manual_prerelease=False):
        self.write_audit(); self.write_trust()
        return pack.wrap(self.stage, self.build, self.output, "1.0.0", self.source, review, manual_prerelease)

    def rewrite_zip(self, path, transform):
        with zipfile.ZipFile(path) as archive:
            rows = [(item.filename, archive.read(item)) for item in archive.infolist()]
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in rows:
                archive.writestr(name, transform(name, data))

    def test_valid_fork_layout_and_source(self):
        path = self.wrap()
        row = pack.inspect_bundle(path, "1.0.0", self.source, expected_repo="example/radeon-fork", expected_key="ab" * 32)
        self.assertEqual(row["flavor"], "AMD")
        self.assertEqual(row["sha256"], pack.digest(path))
        with zipfile.ZipFile(path) as archive:
            self.assertEqual(archive.read(pack.stem("1.0.0") + "/portable-package.txt"), pack.MARKER)
            prefix = pack.stem("1.0.0") + "/versions/1.0.0/"
            self.assertEqual({n for n in archive.namelist() if "/runtime_local/" in n},
                             {prefix + n for n in pack.FSR_RUNTIME_FILES})
            self.assertFalse(any("/logs/" in n for n in archive.namelist()))

    def test_each_fsr_dependency_is_required_in_audit(self):
        complete = self.audit["files"][:]
        for name in pack.FSR_RUNTIME_FILES:
            with self.subTest(missing=name):
                self.audit["files"] = [row for row in complete if row["path"] != name]
                for review in (False, True):
                    with self.assertRaisesRegex(ValueError, "missing audited"):
                        pack.validate_audit(self.audit, "1.0.0", self.source, review)
        self.audit["files"] = complete

    def test_each_fsr_dependency_must_exist_and_match_audit(self):
        for name in pack.FSR_RUNTIME_FILES:
            path = self.stage / name
            data = path.read_bytes()
            with self.subTest(missing=name):
                path.unlink()
                with self.assertRaises(FileNotFoundError):
                    self.wrap(review=True)
                path.write_bytes(data + b"modified")
                with self.assertRaisesRegex(ValueError, "stage file differs"):
                    self.wrap(review=True)
                path.write_bytes(data)

    def test_fsr_exception_is_limited_to_four_exact_paths(self):
        for name in pack.ALL_FSR_RUNTIME_FILES:
            pack.check_path(name)
        for name in ("runtime_local/amd/fidelityfx/unknown.dll", "runtime_local/amd/fidelityfx/settings.json",
                     "runtime_local/user-data-2.0.0/profile.json", "runtime_local/amd/fidelityfx/amd_fidelityfx_loader_dx12.dll.bak",
                     "extra/" + pack.FSR_RUNTIME_FILES[0], "runtime_local/intel/experimental/libxess_fg.dll",
                     pack.OPEN_FSR_RUNTIME_FILE + ".bak", "extra/" + pack.OPEN_FSR_RUNTIME_FILE):
            with self.subTest(path=name), self.assertRaises(ValueError):
                pack.check_path(name)

    def test_review_without_feed_is_not_signable(self):
        self.trust.update(repository="", publicKey="", installEnabled=False)
        self.audit.update(releaseReady=False, localOnly=True, radeonRedistributionReviewed=False)
        path = self.wrap(review=True)
        pack.inspect_bundle(path, "1.0.0", self.source, review=True)
        with self.assertRaisesRegex(ValueError, "dedicated repository"):
            pack.inspect_bundle(path, "1.0.0", self.source)

    def test_upstream_repository_rejected_even_in_review(self):
        self.trust["repository"] = "Likely7/Veyra-NRVideo"
        for review in (False, True):
            with self.subTest(review=review), self.assertRaisesRegex(ValueError, "separate Radeon"):
                self.wrap(review)

    def test_unreviewed_file_blocks_public_release(self):
        self.audit["files"][0].pop("redistribution")
        with self.assertRaisesRegex(ValueError, "no recorded redistribution"):
            self.wrap()

    def test_stale_executable_audit_rejected(self):
        (self.stage / pack.PLAYER).write_bytes(b"custom executable")
        with self.assertRaisesRegex(ValueError, "differs from build"):
            self.wrap()

    def test_changed_shader_and_source_rejected(self):
        path = self.stage / "qml/Veyra/Main.qml"
        path.write_bytes(b"modified")
        with self.assertRaisesRegex(ValueError, "differs from audit"):
            self.wrap()
        path.write_bytes(b"fixture")
        self.source.write_bytes(b"different source revision")
        with self.assertRaisesRegex(ValueError, "corresponding-source"):
            self.wrap()

    def test_private_and_vendor_paths_rejected(self):
        for name in ("runtime_local/radeon/nr-assets/model.f16", "logs/player.log", "runtime/amd-nr/assets/model.bin",
                     "runtime/amd-nr-rdna3/nvngx_dlssnr.dll", "runtime/nvidia-vfg/model.bin", "nvcuda.dll",
                     "runtime/experimental/anr/bootstrap/winmm.dll", "media/intro.wav", "private.pem", "../escape", "CON.txt", "COM¹"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                pack.check_path(name)

    def test_custom_rife_provider_is_not_treated_as_nvidia_binary(self):
        row = self.add_file(pack.PROVIDER, b"source-built-rife-fixture")
        with self.assertRaisesRegex(ValueError, "source-built Radeon provenance"):
            self.wrap()
        self.audit["radeonSourceBuiltProviderSha256"] = row["sha256"]
        path = self.wrap()
        pack.inspect_bundle(path, "1.0.0", self.source)

    def test_archive_tampering_and_foreign_marker_rejected(self):
        path = self.wrap()
        self.rewrite_zip(path, lambda name, data: b"tampered" if name.endswith("/" + pack.PLAYER) else data)
        with self.assertRaisesRegex(ValueError, "archived bytes"):
            pack.inspect_bundle(path, "1.0.0", self.source)
        self.rewrite_zip(path, lambda name, data: b"VEYRA_PORTABLE_V1\nAMD\n" if name.endswith("portable-package.txt") else data)
        with self.assertRaisesRegex(ValueError, "foreign package marker"):
            pack.inspect_bundle(path, "1.0.0", self.source)

    def test_signature_bound_to_repository_and_embedded_key(self):
        path = self.wrap()
        with self.assertRaisesRegex(ValueError, "repository differs"):
            pack.inspect_bundle(path, "1.0.0", self.source, expected_repo="another/fork")
        with self.assertRaisesRegex(ValueError, "signing key"):
            pack.inspect_bundle(path, "1.0.0", self.source, expected_key="cd" * 32)

    def test_update_worker_and_launcher_are_audited(self):
        (self.build / pack.WORKER).write_bytes(b"another worker")
        with self.assertRaisesRegex(ValueError, "build artifact differs"):
            self.wrap()
        (self.build / pack.WORKER).write_bytes(b"test worker")
        path = self.wrap()
        self.rewrite_zip(path, lambda name, data: b"other launcher" if name.endswith("/Veyra.exe") else data)
        with self.assertRaisesRegex(ValueError, "archived build artifact"):
            pack.inspect_bundle(path, "1.0.0", self.source)

    def test_sign_payload_identity_and_no_key_generation(self):
        path = self.wrap()
        key, openssl = self.root / "fixture.key", self.root / "fixture-openssl.exe"
        key.write_bytes(b"dummy"); openssl.write_bytes(b"dummy")
        public = bytes.fromhex("302a300506032b6570032100" + "ab" * 32)
        with patch.object(pack.subprocess, "check_output", side_effect=[public, b"s" * 64]) as command:
            index = pack.sign(path, self.source, self.root / "signed", "1.0.0", "example/radeon-fork", key, openssl)
        envelope = json.loads(index.read_bytes())
        payload = json.loads(base64.b64decode(envelope["payload"]))
        self.assertEqual((payload["product"], payload["channel"], payload["repository"]),
                         (pack.PRODUCT, pack.CHANNEL, "example/radeon-fork"))
        self.assertEqual(payload["correspondingSource"]["sha256"], pack.digest(self.source))
        self.assertEqual(len(command.call_args_list), 2)
        self.assertNotIn("genpkey", str(command.call_args_list))


    def test_public_manual_prerelease_is_explicit_and_has_no_private_marker(self):
        self.trust.update(repository="", publicKey="", installEnabled=False)
        path = self.wrap(manual_prerelease=True)
        pack.inspect_bundle(path, "1.0.0", self.source, manual_prerelease=True)
        with zipfile.ZipFile(path) as archive:
            prefix = pack.stem("1.0.0") + "/"
            self.assertEqual(archive.read(prefix + "MANUAL-PRERELEASE.txt"), pack.MANUAL_NOTICE)
            self.assertNotIn(prefix + "NOT-FOR-PUBLIC-RELEASE.txt", archive.namelist())
            audit = json.loads(archive.read(prefix + "versions/1.0.0/package-manifest.json"))
            self.assertTrue(audit["releaseReady"])
            self.assertFalse(audit["localOnly"])
        with self.assertRaisesRegex(ValueError, "dedicated repository"):
            pack.inspect_bundle(path, "1.0.0", self.source)
        self.assertFalse((self.output / pack.INDEX).exists())

    def test_manual_prerelease_requires_fully_disabled_fork_trust(self):
        disabled = dict(self.trust, repository="", publicKey="", installEnabled=False)
        for changes in ({"installEnabled": True}, {"installEnabled": None},
                        {"repository": "example/radeon-fork"}, {"publicKey": "ab" * 32},
                        {"radeonFork": False}, {"testBuild": True},
                        {"repository": "Likely7/Veyra-NRVideo"}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                pack.trust_policy(dict(disabled, **changes), False, manual_prerelease=True)
        with self.assertRaisesRegex(ValueError, "mutually exclusive"):
            pack.trust_policy(disabled, True, manual_prerelease=True)

    def test_manual_prerelease_still_requires_public_audit_and_exact_source(self):
        self.trust.update(repository="", publicKey="", installEnabled=False)
        for key, value in (("releaseReady", False), ("localOnly", True), ("radeonRedistributionReviewed", False)):
            old = self.audit[key]
            self.audit[key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "redistribution review"):
                self.wrap(manual_prerelease=True)
            self.audit[key] = old
        self.write_audit(); self.write_trust()
        with self.assertRaisesRegex(ValueError, "corresponding-source"):
            pack.wrap(self.stage, self.build, self.output, "1.0.0", manual_prerelease=True)
        self.source.write_bytes(b"another source snapshot")
        with self.assertRaisesRegex(ValueError, "corresponding-source"):
            self.wrap(manual_prerelease=True)

    def test_manual_prerelease_keeps_file_license_and_private_path_checks(self):
        self.trust.update(repository="", publicKey="", installEnabled=False)
        rights = self.audit["files"][0]["redistribution"]
        for key in ("reviewed", "license", "origin"):
            old = rights.pop(key)
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "redistribution"):
                self.wrap(manual_prerelease=True)
            rights[key] = old
        self.add_file("runtime_local/radeon/nr-assets/model.f16")
        with self.assertRaisesRegex(ValueError, "private or unapproved"):
            self.wrap(manual_prerelease=True)

    def test_manual_prerelease_keeps_binary_and_archive_integrity_checks(self):
        self.trust.update(repository="", publicKey="", installEnabled=False)
        (self.stage / pack.PLAYER).write_bytes(b"wrong player")
        with self.assertRaisesRegex(ValueError, "differs from build"):
            self.wrap(manual_prerelease=True)
        (self.stage / pack.PLAYER).write_bytes((self.build / pack.PLAYER).read_bytes())
        path = self.wrap(manual_prerelease=True)
        self.rewrite_zip(path, lambda name, data: b"tampered" if name.endswith("/" + pack.PLAYER) else data)
        with self.assertRaisesRegex(ValueError, "archived bytes"):
            pack.inspect_bundle(path, "1.0.0", self.source, manual_prerelease=True)

    def test_manual_prerelease_notice_tampering_is_rejected(self):
        self.trust.update(repository="", publicKey="", installEnabled=False)
        path = self.wrap(manual_prerelease=True)
        self.rewrite_zip(path, lambda name, data: b"stable automatic update" if name.endswith("/MANUAL-PRERELEASE.txt") else data)
        with self.assertRaisesRegex(ValueError, "invalid manual prerelease notice"):
            pack.inspect_bundle(path, "1.0.0", self.source, manual_prerelease=True)

    def test_manual_prerelease_cannot_be_signed_as_an_update(self):
        self.trust.update(repository="", publicKey="", installEnabled=False)
        path = self.wrap(manual_prerelease=True)
        (self.root / "fixture.key").write_bytes(b"dummy")
        (self.root / "fixture-openssl.exe").write_bytes(b"dummy")
        public = bytes.fromhex("302a300506032b6570032100" + "ab" * 32)
        with patch.object(pack.subprocess, "check_output", return_value=public) as command:
            with self.assertRaisesRegex(ValueError, "dedicated repository"):
                pack.sign(path, self.source, self.root / "signed", "1.0.0", "example/radeon-fork",
                          self.root / "fixture.key", self.root / "fixture-openssl.exe")
        self.assertEqual(len(command.call_args_list), 1)  # Public-key read only, never pkeyutl -sign.
        self.assertFalse((self.root / "signed" / pack.INDEX).exists())


    def core_audit(self):
        self.audit["packageVariant"] = "core"
        self.audit["optionalComponents"] = {"fidelityfx": {"bundled": False, "reason": "Runtime redistribution review incomplete; FSR unavailable."}}
        self.audit["files"] = [row for row in self.audit["files"] if row["path"] not in pack.FSR_RUNTIME_FILES]
        for name in pack.FSR_RUNTIME_FILES:
            (self.stage / name).unlink()

    def test_core_manual_prerelease_omits_fsr_and_declares_unavailable_features(self):
        self.core_audit()
        self.trust.update(repository="", publicKey="", installEnabled=False)
        path = self.wrap(manual_prerelease=True)
        pack.inspect_bundle(path, "1.0.0", self.source, manual_prerelease=True)
        with zipfile.ZipFile(path) as archive:
            prefix = pack.stem("1.0.0") + "/"
            self.assertEqual(archive.read(prefix + "CORE-PACKAGE.txt"), pack.CORE_NOTICE)
            self.assertFalse(any("/runtime_local/" in name for name in archive.namelist()))
            self.assertIn(prefix + "MANUAL-PRERELEASE.txt", archive.namelist())

    def test_core_rejects_partial_or_complete_bundled_fsr(self):
        self.core_audit()
        for name in pack.FSR_RUNTIME_FILES:
            self.add_file(name)
            with self.subTest(count=len([r for r in self.audit["files"] if r["path"] in pack.FSR_RUNTIME_FILES])):
                with self.assertRaisesRegex(ValueError, "omit all four"):
                    self.wrap()

    def test_core_requires_truthful_explicit_omission_metadata(self):
        self.core_audit()
        proper = self.audit["optionalComponents"]
        for value in ({}, {"fidelityfx": {"bundled": False}},
                      {"fidelityfx": {"bundled": True, "reason": "omitted"}},
                      {"fidelityfx": {"bundled": False, "reason": "  "}}):
            self.audit["optionalComponents"] = value
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "explicit FidelityFX"):
                self.wrap()
        self.audit["optionalComponents"] = proper
        self.audit.pop("packageVariant")
        with self.assertRaisesRegex(ValueError, "explicit Core"):
            self.wrap()

    def test_core_still_requires_file_review_and_corresponding_source(self):
        self.core_audit()
        rights = self.audit["files"][0].pop("redistribution")
        with self.assertRaisesRegex(ValueError, "redistribution"):
            self.wrap()
        self.audit["files"][0]["redistribution"] = rights
        self.source.write_bytes(b"wrong corresponding source")
        with self.assertRaisesRegex(ValueError, "corresponding-source"):
            self.wrap()

    def test_core_notice_tampering_is_rejected(self):
        self.core_audit()
        path = self.wrap()
        self.rewrite_zip(path, lambda name, data: b"Full FSR included" if name.endswith("/CORE-PACKAGE.txt") else data)
        with self.assertRaisesRegex(ValueError, "invalid Core package notice"):
            pack.inspect_bundle(path, "1.0.0", self.source)

    def open_fsr_audit(self):
        self.audit["files"] = [row for row in self.audit["files"] if row["path"] not in pack.FSR_RUNTIME_FILES]
        for name in pack.FSR_RUNTIME_FILES:
            (self.stage / name).unlink()
        row = self.add_file(pack.OPEN_FSR_RUNTIME_FILE, b"source-built-open-fsr-fixture")
        self.audit["optionalComponents"] = {"fidelityfx": {
            "bundled": True, "implementation": "open-fsr3", "sha256": row["sha256"]}}
        return row

    def test_open_fsr_full_package_round_trip_contains_only_monolith(self):
        self.open_fsr_audit()
        path = self.wrap()
        pack.inspect_bundle(path, "1.0.0", self.source)
        with zipfile.ZipFile(path) as archive:
            prefix = pack.stem("1.0.0") + "/versions/1.0.0/"
            self.assertEqual({n for n in archive.namelist() if "/runtime_local/" in n},
                             {prefix + pack.OPEN_FSR_RUNTIME_FILE})
            self.assertFalse(any(n.endswith("CORE-PACKAGE.txt") for n in archive.namelist()))

    def test_open_fsr_explicit_profile_and_bundled_declaration_are_required(self):
        self.open_fsr_audit()
        proper = self.audit["optionalComponents"]
        for value in ({}, {"fidelityfx": {"bundled": True}},
                      {"fidelityfx": {"bundled": False, "implementation": "open-fsr3"}},
                      {"fidelityfx": {"bundled": True, "implementation": "unknown"}}):
            self.audit["optionalComponents"] = value
            for review in (False, True):
                with self.subTest(value=value, review=review), self.assertRaises(ValueError):
                    pack.validate_audit(self.audit, "1.0.0", self.source, review)
        self.audit["optionalComponents"] = proper

    def test_open_fsr_requires_monolith_row_and_exact_source_built_provenance(self):
        row = self.open_fsr_audit()
        component = self.audit["optionalComponents"]["fidelityfx"]
        for value in (None, "", "not-a-hash", "f" * 64, 123):
            component["sha256"] = value
            for review in (False, True):
                with self.subTest(value=value, review=review), self.assertRaisesRegex(ValueError, "provenance"):
                    pack.validate_audit(self.audit, "1.0.0", self.source, review)
        component["sha256"] = row["sha256"]
        self.audit["files"].remove(row)
        with self.assertRaisesRegex(ValueError, "missing audited"):
            pack.validate_audit(self.audit, "1.0.0", self.source, False)

    def test_open_fsr_rejects_every_mixed_modular_runtime(self):
        self.open_fsr_audit()
        for name in pack.FSR_RUNTIME_FILES:
            row = self.add_file(name)
            for review in (False, True):
                with self.subTest(name=name, review=review), self.assertRaisesRegex(ValueError, "mixed runtimes"):
                    pack.validate_audit(self.audit, "1.0.0", self.source, review)
            self.audit["files"].remove(row)
            (self.stage / name).unlink()

    def test_modular_fsr_rejects_unannounced_open_monolith(self):
        self.add_file(pack.OPEN_FSR_RUNTIME_FILE)
        with self.assertRaisesRegex(ValueError, "mixed runtimes"):
            self.wrap()

    def test_core_rejects_open_monolith_even_with_omission_metadata(self):
        self.core_audit()
        self.add_file(pack.OPEN_FSR_RUNTIME_FILE)
        for review in (False, True):
            with self.subTest(review=review), self.assertRaisesRegex(ValueError, "omit all four"):
                pack.validate_audit(self.audit, "1.0.0", self.source, review)

    def test_open_fsr_keeps_stage_and_archive_byte_integrity(self):
        self.open_fsr_audit()
        dll = self.stage / pack.OPEN_FSR_RUNTIME_FILE
        data = dll.read_bytes()
        dll.unlink()
        with self.assertRaises(FileNotFoundError):
            self.wrap()
        dll.write_bytes(data + b"changed")
        with self.assertRaisesRegex(ValueError, "stage file differs"):
            self.wrap()
        dll.write_bytes(data)
        path = self.wrap()
        self.rewrite_zip(path, lambda name, data: b"changed" if name.endswith("/" + pack.OPEN_FSR_RUNTIME_FILE) else data)
        with self.assertRaisesRegex(ValueError, "archived bytes"):
            pack.inspect_bundle(path, "1.0.0", self.source)

    def test_open_fsr_manual_release_still_requires_file_rights_and_exact_source(self):
        row = self.open_fsr_audit()
        self.trust.update(repository="", publicKey="", installEnabled=False)
        rights = row.pop("redistribution")
        with self.assertRaisesRegex(ValueError, "redistribution"):
            self.wrap(manual_prerelease=True)
        row["redistribution"] = rights
        self.source.write_bytes(b"wrong source revision")
        with self.assertRaisesRegex(ValueError, "corresponding-source"):
            self.wrap(manual_prerelease=True)

    def test_open_fsr_archive_manifest_cannot_drop_or_forge_provenance(self):
        self.open_fsr_audit()
        path = self.wrap()
        def remove_provenance(name, data):
            if name.endswith("/package-manifest.json"):
                audit = json.loads(data)
                audit["optionalComponents"]["fidelityfx"].pop("sha256")
                return pack.encode(audit)
            return data
        self.rewrite_zip(path, remove_provenance)
        with self.assertRaisesRegex(ValueError, "provenance"):
            pack.inspect_bundle(path, "1.0.0", self.source)

    def test_existing_artifact_is_never_overwritten(self):
        path = self.wrap()
        before = pack.digest(path)
        with self.assertRaises(FileExistsError):
            self.wrap()
        self.assertEqual(pack.digest(path), before)


if __name__ == "__main__":
    unittest.main()
