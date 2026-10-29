#!/usr/bin/env python3
"""Fail closed if a synthetic or manual actor path returns to the replay.

The product is the PoseTrack v2 replay: recorded skeleton poses written into
inert puppets through the pose detour, timed events replayed by the engine's
own systems, and a playhead transport (play, pause, scrub). The removed
network research modules (network probe, sync codecs, task context, story
task labs) must not come back.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


# Files of removed modules: the synthetic actor backends (NodeTape, forward
# executors, rigid tapes) and the network research modules.
FORBIDDEN_PATH_FRAGMENTS = (
    "node_tape",
    "ped_node_tape",
    "forward_action",
    "mixed_forward",
    "ped_forward",
    "player_forward",
    "rigid_forward",
    "rigid_body_tape",
    "scene_forward",
    "scene_archive",
    "scene_event_tape",
    "shot_event_detector",
    "story_task_phase_tape",
    "clip_sector_position",
    "aim_direction_payload",
    "network_probe",
    "ped_sync_node_codec",
    "task_context_interposer",
    "task_context_override_route",
    "task_binding_projection",
    "post_sync_context_zero_route",
    "story_jump_playback_session",
    "story_task_lifetime_tape",
    "story_task_wire_codec",
    "msb_bit_stream",
    "movement_payload_codec",
    "movement_codec_roundtrip_classifier",
    "movement_multisample_window",
    "orientation_apply_spy",
    "orientation_payload_codec",
    "sector_position_payload_codec",
    "physical_velocity_payload_codec",
    "ped_facial_appearance_payload_codec",
    "recording_sample_cadence",
)

# Tokens that mean a manual actor executor or a removed lab is back in the
# script.
FORBIDDEN_ACTIVE_TOKENS = (
    # The old diagnostic .rdrclip and its 30 Hz frame clock.
    "SaveClip",
    "RecordFrameEvents",
    "g_recordedFrames",
    "NodeTape",
    "NODE_TAPE",
    ".rntp",
    "RNTP",
    "TASK::TASK_GO_STRAIGHT_TO_COORD",
    "TASK::TASK_JUMP",
    "TASK::TASK_SHOOT_AT_ENTITY",
    "TASK::TASK_SHOOT_AT_COORD",
    "TASK::TASK_RELOAD_WEAPON",
    "TASK::TASK_CLIMB",
    "TASK::TASK_AIM_GUN",
    "ENTITY::SET_ENTITY_COORDS_NO_OFFSET",
    "ENTITY::SET_ENTITY_ROTATION",
    "ENTITY::SET_ENTITY_VELOCITY",
    "ENTITY::APPLY_FORCE_TO_ENTITY",
    "replay::network::",
    "replay::sync::",
    "replay::story::",
    "g_r1cCSourceCapture",
    "RunLocalFilmSandboxObserver",
    "SceneCensus",
)

# The product actor backend is PoseTrack v2: recorded skeleton poses written
# into inert puppets. Authored tasks and weapon natives stay forbidden.
REQUIRED_PRODUCT_MARKERS = (
    "actor_backend=pose_track_v2",
    "task_native_actor_calls=0",
    "weapon_actor_calls=0",
)

POSE_PLAYBACK_FILE = "pose_playback.cpp"
POSE_CAPTURE_FILE = "pose_capture.cpp"
POSE_OVERRIDE_FUNCTION = "ApplyOverrideFromDetour"
POSE_PLAYBACK_FORBIDDEN_CALL_PATTERNS = (
    re.compile(r"\bTASK::\s*[A-Za-z_]\w*\s*\("),
    re.compile(r"\bWEAPON::\s*[A-Za-z_]\w*\s*\("),
    re.compile(
        r"\bENTITY::\s*(?:SET_ENTITY_VELOCITY|APPLY_FORCE_TO_ENTITY|"
        r"SET_ENTITY_ANGULAR_VELOCITY)\s*\("
    ),
)
# Weapon object construction (the reference weapon object create): streams
# the weapon asset and builds an unowned object. None of these reach a ped,
# so actor weapon natives (give, equip, fire) stay forbidden.
POSE_PLAYBACK_ALLOWED_WEAPON_OBJECT_CALLS = frozenset(
    (
        "IS_WEAPON_VALID",
        "_REQUEST_WEAPON_ASSET",
        "_HAS_WEAPON_ASSET_LOADED",
        "_REMOVE_WEAPON_ASSET",
        "_CREATE_WEAPON_OBJECT",
        # Clears the puppet's default model loadout at creation, so only the
        # recorded weapon objects are drawn. It removes, never gives, equips
        # or fires.
        "REMOVE_ALL_PED_WEAPONS",
        # The recorded components of a weapon object (straps, barrels,
        # finishes) put back on the object puppet; none reaches a ped.
        "_GET_WEAPON_COMPONENT_TYPE_MODEL",
        "HAS_WEAPON_GOT_WEAPON_COMPONENT",
        "_GIVE_WEAPON_COMPONENT_TO_ENTITY",
        "REMOVE_WEAPON_COMPONENT_FROM_WEAPON_OBJECT",
    )
)

# Every build offered for a test is listed here, so a stray VERSION fails.
ACCEPTED_VERSIONS: tuple[str, ...] = ("1.0.0",)

# Builds made before 1.0.0 are listed, when the file is present, in
# audit_accepted_versions.txt next to this script (one VERSION per line, text
# after "#" is a note). The public repository does not carry it.
_HISTORY = Path(__file__).with_name("audit_accepted_versions.txt")
if _HISTORY.is_file():
    ACCEPTED_VERSIONS = ACCEPTED_VERSIONS + tuple(
        entry
        for entry in (
            line.split("#", 1)[0].strip()
            for line in _HISTORY.read_text(encoding="utf-8").splitlines()
        )
        if entry
    )


def _strip_cpp_comments_and_strings(text: str) -> str:
    """Return a code-ish view for static pattern checks.

    This is deliberately small rather than a C++ parser.  Removing comments
    and string contents avoids false positives from log messages while
    preserving line breaks and enough punctuation for call/assignment checks.
    """

    output: list[str] = []
    index = 0
    state = "code"
    quote = ""
    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if char == "/" and next_char == "/":
                output.extend((" ", " "))
                index += 2
                state = "line_comment"
                continue
            if char == "/" and next_char == "*":
                output.extend((" ", " "))
                index += 2
                state = "block_comment"
                continue
            if char in ('\"', "'"):
                quote = char
                output.append(" ")
                index += 1
                state = "string"
                continue
            output.append(char)
            index += 1
            continue
        if state == "line_comment":
            if char in "\r\n":
                output.append(char)
                state = "code"
            else:
                output.append(" ")
            index += 1
            continue
        if state == "block_comment":
            if char == "*" and next_char == "/":
                output.extend((" ", " "))
                index += 2
                state = "code"
            else:
                output.append(char if char in "\r\n" else " ")
                index += 1
            continue
        # C/C++ string or character literal.  Preserve newlines so reported
        # locations remain useful if an issue is printed by a caller.
        if char == "\\":
            output.append(" ")
            if next_char:
                output.append(next_char if next_char in "\r\n" else " ")
                index += 2
            else:
                index += 1
            continue
        if char == quote:
            output.append(" ")
            index += 1
            state = "code"
            continue
        output.append(char if char in "\r\n" else " ")
        index += 1
    return "".join(output)


def _normalize(path: str) -> str:
    return path.replace("\\", "/").lower()


def _compiled_paths(project_text: str) -> list[str]:
    return re.findall(r'<Cl(?:Compile|Include)\s+Include="([^"]+)"', project_text)


def _is_allowed_weapon_object_call(call: str) -> bool:
    match = re.match(r"WEAPON::\s*(\w+)", call.strip())
    return bool(match) and match.group(1) in POSE_PLAYBACK_ALLOWED_WEAPON_OBJECT_CALLS


def _pose_playback_audit(root: Path) -> dict[str, object]:
    """PoseTrack v2 contract: puppet natives stay inert, pose writes stay in the detour."""

    issues: list[str] = []
    playback_path = root / "src" / POSE_PLAYBACK_FILE
    capture_path = root / "src" / POSE_CAPTURE_FILE
    if not playback_path.is_file():
        return {"found": False, "issues": ["src/pose_playback.cpp is missing"]}

    playback = _strip_cpp_comments_and_strings(
        playback_path.read_text(encoding="utf-8")
    )
    forbidden = sorted(
        {
            match.group(0).strip()
            for pattern in POSE_PLAYBACK_FORBIDDEN_CALL_PATTERNS
            for match in pattern.finditer(playback)
            if not _is_allowed_weapon_object_call(match.group(0))
        }
    )
    if forbidden:
        issues.append(
            "pose playback uses forbidden actor native(s): " + "; ".join(forbidden)
        )

    override_found = False
    if capture_path.is_file():
        capture = capture_path.read_text(encoding="utf-8")
        override_found = POSE_OVERRIDE_FUNCTION in capture
        capture_code = _strip_cpp_comments_and_strings(capture)
        if re.search(r"\bTASK::|\bWEAPON::|\bENTITY::|\bPED::", capture_code):
            issues.append("pose capture must not call natives")
    if not override_found:
        issues.append(
            f"pose override write boundary {POSE_OVERRIDE_FUNCTION} is missing"
        )
    return {
        "found": True,
        "forbidden_calls": forbidden,
        "override_boundary": override_found,
        "issues": issues,
    }


def _script_audit(script: str) -> list[str]:
    issues: list[str] = []
    code = _strip_cpp_comments_and_strings(script)
    for token in FORBIDDEN_ACTIVE_TOKENS:
        if token in code:
            issues.append(f"manual/synthetic actor or removed lab token in script.cpp: {token}")
    for marker in REQUIRED_PRODUCT_MARKERS:
        if marker not in script:
            issues.append(f"missing fail-closed product marker: {marker}")
    for fragment in FORBIDDEN_PATH_FRAGMENTS:
        if re.search(r'#include\s*"[^"]*' + re.escape(fragment), script):
            issues.append(f"script.cpp includes a removed module: {fragment}")

    # The transport (StartPlayback .. CurrentEditorCameraState) drives the
    # playhead only; actor natives never appear there. The definition, not
    # the forward declaration earlier in the file.
    playback_match = re.search(
        r"    bool StartPlayback\(ULONGLONG now\)\s*\{", script
    )
    playback_start = playback_match.start() if playback_match else -1
    playback_end = script.find(
        "    replay::editor::CameraState CurrentEditorCameraState()"
    )
    if playback_start < 0 or playback_end <= playback_start:
        issues.append("playback transport boundary was not found")
    else:
        playback = script[playback_start:playback_end]
        for namespace in ("PED::", "TASK::", "WEAPON::"):
            if namespace in playback:
                issues.append(
                    f"actor native namespace in product playback: {namespace}"
                )
    return issues


def audit(root: Path) -> dict[str, object]:
    issues: list[str] = []
    for relative in (
        "script.cpp",
        "VERSION",
        "ReplayProbe.vcxproj",
        "ReplayCoreTests.vcxproj",
    ):
        if not (root / relative).is_file():
            issues.append(f"missing active file: {relative}")

    source_root = root / "src"
    if source_root.is_dir():
        for path in sorted(source_root.rglob("*")):
            if not path.is_file():
                continue
            normalized = _normalize(str(path.relative_to(root)))
            for fragment in FORBIDDEN_PATH_FRAGMENTS:
                if fragment in normalized:
                    issues.append(f"forbidden path remains: {normalized}")
                    break

    for project_name in ("ReplayProbe.vcxproj", "ReplayCoreTests.vcxproj"):
        path = root / project_name
        if not path.is_file():
            continue
        for compiled in _compiled_paths(path.read_text(encoding="utf-8")):
            normalized = _normalize(compiled)
            for fragment in FORBIDDEN_PATH_FRAGMENTS:
                if fragment in normalized:
                    issues.append(
                        f"{project_name} references a forbidden path: {compiled}"
                    )
                    break

    version_path = root / "VERSION"
    version = ""
    if version_path.is_file():
        version = version_path.read_text(encoding="utf-8").strip()
        if version not in ACCEPTED_VERSIONS:
            issues.append(f"VERSION mismatch: {version} is not an accepted build")

    script_path = root / "script.cpp"
    if script_path.is_file():
        issues.extend(_script_audit(script_path.read_text(encoding="utf-8")))

    pose_playback = _pose_playback_audit(root)
    issues.extend(str(issue) for issue in pose_playback.get("issues", []))

    return {
        "passed": not issues,
        "issues": issues,
        "version": version,
        "pose_playback": pose_playback,
        "policy": "pose-track-v2-inert-puppets",
        "synthetic_actor_backend": False,
        "manual_actor_backend": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
    )
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    result = audit(args.root.resolve())
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print("PASS" if result["passed"] else "FAIL")
        for issue in result["issues"]:
            print(f"- {issue}")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
