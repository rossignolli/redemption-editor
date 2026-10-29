import importlib.util
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = (
    Path(__file__).resolve().parents[1]
    / "audit_retail_only_architecture.py"
)
SPEC = importlib.util.spec_from_file_location("retail_audit", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class RetailOnlyArchitectureAuditTests(unittest.TestCase):
    def _minimal_root(self) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / "src").mkdir()
        markers = "\n".join(MODULE.REQUIRED_PRODUCT_MARKERS)
        (root / "script.cpp").write_text(
            "    bool StartPlayback(ULONGLONG now)\n"
            "{\n"
            f"{markers}\n"
            "}\n"
            "    replay::editor::CameraState CurrentEditorCameraState()\n"
            "{\n}\n",
            encoding="utf-8",
        )
        (root / "VERSION").write_text(
            MODULE.ACCEPTED_VERSIONS[-1] + "\n", encoding="utf-8"
        )
        for project in ("ReplayProbe.vcxproj", "ReplayCoreTests.vcxproj"):
            (root / project).write_text(
                '<ClCompile Include="src\\pose_playback.cpp" />',
                encoding="utf-8",
            )
        self._write_pose_playback(root, "")
        return root

    def _write_pose_playback(self, root: Path, body: str) -> None:
        (root / "src" / "pose_playback.cpp").write_text(
            "void PlaceClone() {\n" + body + "\n}\n", encoding="utf-8"
        )
        (root / "src" / "pose_capture.cpp").write_text(
            "void ApplyOverrideFromDetour() { memcpy(a, b, c); }\n",
            encoding="utf-8",
        )

    def test_minimal_tree_passes(self):
        result = MODULE.audit(self._minimal_root())
        self.assertTrue(result["passed"], result["issues"])
        self.assertEqual(result["policy"], "pose-track-v2-inert-puppets")

    def test_synthetic_file_fails(self):
        root = self._minimal_root()
        (root / "src" / "node_tape.cpp").write_text("synthetic", encoding="utf-8")
        result = MODULE.audit(root)
        self.assertFalse(result["passed"])
        self.assertTrue(
            any("forbidden path remains" in item for item in result["issues"])
        )

    def test_removed_lane_file_fails(self):
        root = self._minimal_root()
        (root / "src" / "network_probe.cpp").write_text("", encoding="utf-8")
        result = MODULE.audit(root)
        self.assertTrue(
            any("network_probe" in item for item in result["issues"])
        )

    def test_removed_lane_include_fails(self):
        root = self._minimal_root()
        path = root / "script.cpp"
        path.write_text(
            '#include "src/ped_sync_node_codec.h"\n'
            + path.read_text(encoding="utf-8"),
            encoding="utf-8",
        )
        result = MODULE.audit(root)
        self.assertTrue(
            any("removed module" in item for item in result["issues"])
        )

    def test_removed_lab_token_fails(self):
        root = self._minimal_root()
        path = root / "script.cpp"
        path.write_text(
            path.read_text(encoding="utf-8")
            + "\nif (g_r1cCSourceCapture.attempted) return;\n",
            encoding="utf-8",
        )
        result = MODULE.audit(root)
        self.assertTrue(
            any("g_r1cCSourceCapture" in item for item in result["issues"])
        )

    def test_project_reference_to_removed_lane_fails(self):
        root = self._minimal_root()
        (root / "ReplayProbe.vcxproj").write_text(
            '<ClInclude Include="src\\task_context_interposer.h" />',
            encoding="utf-8",
        )
        result = MODULE.audit(root)
        self.assertTrue(
            any("forbidden path" in item for item in result["issues"])
        )

    def test_manual_actor_call_fails(self):
        root = self._minimal_root()
        path = root / "script.cpp"
        path.write_text(
            path.read_text(encoding="utf-8") + "\nTASK::TASK_JUMP(actor, true);\n",
            encoding="utf-8",
        )
        result = MODULE.audit(root)
        self.assertFalse(result["passed"])
        self.assertTrue(
            any("TASK::TASK_JUMP" in item for item in result["issues"])
        )

    def test_token_inside_comment_or_string_is_ignored(self):
        root = self._minimal_root()
        path = root / "script.cpp"
        path.write_text(
            path.read_text(encoding="utf-8")
            + '\n// NodeTape was removed\nlog << "SceneCensus gone";\n',
            encoding="utf-8",
        )
        result = MODULE.audit(root)
        self.assertTrue(result["passed"], result["issues"])

    def test_actor_namespace_in_transport_fails(self):
        root = self._minimal_root()
        path = root / "script.cpp"
        script = path.read_text(encoding="utf-8")
        script = script.replace(
            "    bool StartPlayback(ULONGLONG now)\n{\n",
            "    bool StartPlayback(ULONGLONG now)\n{\n    PED::SET_PED_INTO_VEHICLE(p, v, 0);\n",
        )
        path.write_text(script, encoding="utf-8")
        result = MODULE.audit(root)
        self.assertTrue(
            any("product playback: PED::" in item for item in result["issues"])
        )

    def test_playback_region_starts_at_definition_not_declaration(self):
        root = self._minimal_root()
        path = root / "script.cpp"
        prefix = (
            "    bool StartPlayback(ULONGLONG now);\n"
            "    Ped FindHorse() { return PED::GET_MOUNT(0); }\n"
        )
        path.write_text(prefix + path.read_text(encoding="utf-8"), encoding="utf-8")
        result = MODULE.audit(root)
        self.assertFalse(
            any("product playback: PED::" in item for item in result["issues"])
        )

    def test_pose_playback_inert_clone_passes(self):
        root = self._minimal_root()
        self._write_pose_playback(
            root, "ENTITY::SET_ENTITY_COORDS_NO_OFFSET(clone, 0, 0, 0, 0, 0, 0);"
        )
        result = MODULE.audit(root)
        self.assertTrue(result["pose_playback"]["found"])
        self.assertEqual(result["pose_playback"]["issues"], [])

    def test_pose_playback_task_native_fails(self):
        root = self._minimal_root()
        self._write_pose_playback(root, "TASK::TASK_JUMP(clone, 1);")
        result = MODULE.audit(root)
        self.assertFalse(result["passed"])
        self.assertTrue(
            any("forbidden actor native" in issue for issue in result["issues"])
        )

    def test_pose_playback_weapon_object_create_passes(self):
        root = self._minimal_root()
        self._write_pose_playback(
            root,
            "WEAPON::_REQUEST_WEAPON_ASSET(w, 31, 0);"
            " WEAPON::_CREATE_WEAPON_OBJECT(w, 0, 0, 0, 0, 1, 1.0f);",
        )
        result = MODULE.audit(root)
        self.assertEqual(result["pose_playback"]["issues"], [])

    def test_pose_playback_actor_weapon_native_fails(self):
        root = self._minimal_root()
        self._write_pose_playback(root, "WEAPON::GIVE_WEAPON_TO_PED(clone, w, 0, 1, 0);")
        result = MODULE.audit(root)
        self.assertFalse(result["passed"])
        self.assertTrue(
            any("forbidden actor native" in issue for issue in result["issues"])
        )

    def test_pose_capture_without_override_boundary_fails(self):
        root = self._minimal_root()
        (root / "src" / "pose_capture.cpp").write_text(
            "void Other() {}\n", encoding="utf-8"
        )
        result = MODULE.audit(root)
        self.assertFalse(result["passed"])
        self.assertTrue(
            any("override write boundary" in issue for issue in result["issues"])
        )

    def test_version_mutation_fails(self):
        root = self._minimal_root()
        (root / "VERSION").write_text("0.0.0-stray\n", encoding="utf-8")
        result = MODULE.audit(root)
        self.assertFalse(result["passed"])
        self.assertTrue(
            any("VERSION mismatch" in issue for issue in result["issues"])
        )


if __name__ == "__main__":
    unittest.main()
