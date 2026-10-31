# Bind pose, the pose finalizer and T-pose detection

Reverse engineering notes behind the T-pose check and the other code that reads the pose finalizer and the skeleton's bind pose (RDR2 build 1491.50, offline dump). The code keeps the contract and a link here.

## The pose finalizer's bind pose loop (src/tpose_check.h)

2. The bind pose, from the engine: the pose finalizer (kPoseFinalizer) takes
the model's own rows where the animation left a lane unwritten. Its loop:

```text
     mov r10,[rcx]          the pose buffer: locals, then objects (kFinalizerLocals)
     mov r14d,[rcx+0D0h]    groups of the skeleton (rcx) (kPoseFinalizer)
     mov eax,[rcx+108h]     bones of the skeleton (kFinalizerBones)
     lea rsi,[rcx+10h]      one skeleton data pointer per group
     mov rax,[rsi]          the group's skeleton data (kFinalizerBindLoad)
     mov rbx,[rax+28h]      its bone records
     movzx edi,word [rax+5Eh]  its bone count
     movups [rbx], [rbx+10h], [rbx+20h]: rotation, translation and scale
                            of the bone (paired with the rotation, translation
                            and scale lanes of the source, pcmpeqd against
                            all ones)
     add r13,50h            next bone record (0x50 bytes) (kFinalizerBindStride)
     add rsi,8              next group; locals advance by its bones
                            (kFinalizerGroupStep)
```

ReadBindRotations walks the same chain through a guarded read. A pose
compared with it bone by bone (rotation only: translations are the
model's proportions) is the bind pose when at most a handful of bones
turned past kPosedRad (IsBindPose).

## The animation LOD setter (src/anim_lod_hook.h)

The LOD setter (anim_lod_hook.cpp kSetLod: fwAnimDirector* rcx, u8 lod dl,
bool visible r8b) is where the engine hands a ped's animation LOD to its
anim director: the visibility test (anim_visible.h) calls it every frame
for each pre-rendered ped with lod = the distance LOD of the graphics
component (+0x4A0, from the LOD multiplier) and visible = seen this frame
(or the anim-visible byte). Its tail stores the creature component's
requested LOD (+0xAE) and effective LOD (+0xAF = visible ? lod : 3); the
expressions keep only the ones whose LOD range holds it. The anim-visible
byte fixes the off-screen case only: a ped seen at 25 m still gets the
distance LOD (1 or 2: frozen face, measured +280 to +610 bind rows). The
detour passes lod 0 and visible 1 for the directors of the recorded peds:
the animation of a ped near and on screen, with nothing else changed (the
draw LOD, the fade and the draw distance come from the graphics component,
which is not touched, unlike SET_PED_LOD_MULTIPLIER).

Callers (rel32 scan of both text sections): the visibility test (per
frame), the model change (visible 1) and the spawn (lod 0, visible 1).
Every call goes to the original.

The director of a ped, as the creature lookup finds it (engine_sigs.h kCreatureLookup; pure reads):

```text
  slot = (u32 [ped+0x9C] & 0x1FFFF) - u32 [base index];
  entry = u64 [u64 [table] + slot * 16] & ~1 (0: none);
  director = u64 [entry+0x28].
```

## Validation band bounds (src/pose_skeleton_layout.h)

ComposeSimd is the scalar Compose bit for bit in the tests (/fp:precise), but
the ASI builds with /fp:fast, where the compiler may reorder the float
operations of either: the two expected objects can then differ by a few ulps,
and a bone right at a tolerance could be decided either way. The band moves
both thresholds away by more than that difference:

```text
- translation: with u = 2^-24, either computation of the expected
  translation is within u (|parent t| + |offset|) + 14 u |offset| of
  the exact composition per axis for any order of its four-term sums
  (offset: the local translation rotated by the parent), so the two
  are less than 51 u (3.0e-6) of the extent apart, the extent being
  |parent t| + |offset| in L1 norms (the bound of a fully expanded
  rotation is 7.2e-6). The margin is kBandTranslationRelative times
  the extent plus kBandTranslationAbsolute, which also covers the
  rounding of the baseline's and the band's squared distances (a few
  ulps of 2 mm);
- rotation: either normalized product is within 18 u (rad) of the
  exact rotation (16 u from the four-term sums of the product, 2 u
  from the normalization's roundings), so the two are less than 36 u
  (2.1e-6 rad) apart, and AngleBetween's own error is about 18 u
  (1.1e-6 rad) more: the margin kBandAngle is 6 times their sum. Both
  moved thresholds are tested with the cheap angle test (AngleExceeds'
  band covers its own rounding).
```

The band leaves a test undecided when its inputs hold NaN or infinity,
a quaternion's squared norm is outside (0.5, 2) or the extent is
kBandExtentCap or more; the bone still fails when the other test
clearly exceeds its threshold (the baseline's result either way).

## Skeleton shrunk by a gore head shot (src/prefix_hold.h)

A head shot that blows the head off (gore) makes the engine rebuild the
ped's skeleton in place: same pointer, the head's layers gone, then the
stump's layers on. Example: 998 bones (the record before the shot), 705 for
two or three frames (the body group alone), then 857 (the record after
it). No record has 705 bones, so the segment
switch released the arm (segment_switch.h: no_record_for_bones), the detour
wrote nothing (bone_mismatch: the bind pose) and the unposed hide hid the
body until the 857 record armed: the puppet blinked at the shot.

