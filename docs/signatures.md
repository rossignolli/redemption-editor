# Game signatures

How the mod finds the game's functions and data, and what to do when the game updates.

## What a signature is

The mod never stores the address of a game function or global. It stores the function's own bytes, with the bytes
that hold an address left out (`??`): the target of a `call`, `jmp` or `jcc`, and the displacement of a RIP-relative
operand. When the code moves between builds, those are the only bytes that change, so the signature still matches.
When the function itself changes (another layout, other offsets), it does not, and the feature that needs it turns
itself off instead of running on a wrong assumption.

```cpp
// src/metaped_reader.cpp
const sig::Signature kGetter{"char_expression_getter",
    "40 53 48 83 EC 20 83 64 24 30 00 8B DA 48 85 C9 75 05 0F 57 C0 EB 2A 33 D2 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? "
    "?? ?? 48 85 C0 74 10 4C 8D 44 24 30 0F B7 D3 48 8B C8 E8 ?? ?? ?? ?? F3 0F 10 44 24 30 48 83 C4 20 5B C3",
    0xB44250};
```

The last number is where build 1.0.1491.50 has it. A signature declares itself in one of five forms
(`src/game_sig_core.h`):

| Form | Found as | Example |
|---|---|---|
| anchor | its own pattern, searched in the code | the expression getter above |
| `Rel32{dispAt, end}` | the target of a call, a jump or a RIP-relative operand of its parent | the frame container the getter calls; a global a function reads |
| `Offset{n}` | a place `n` bytes inside its parent, checked with its own pattern | an instruction a detour compares its return address with |
| `Pointer{n}` | the pointer at `n` bytes inside its parent | a method in a vtable slot |
| `Instance{}` | the one static object whose first 8 bytes are its parent | an object found by its vtable |

A function whose bytes appear more than once is reached from a unique caller instead of being searched; a global is
reached from the instruction that reads it; a method from its class's vtable. Every byte the mod relies on (the
offsets it reads or writes, the row sizes, the masks) is inside a signature's bytes, so a build that changed one of
them fails the signature rather than the feature.

Shared signatures (code more than one module imitates or names) live in `src/engine_sigs.h`, declared once; the
others sit next to the code that uses them.

### Functions the mod hooks

Another mod may have hooked the same function first (the Pose Editor loads before this one), and its detour writes a
jump over the function's first bytes. The signature of every function the mod detours leaves its first 14 bytes out
(`??`, `kHookBytes`: a 14-byte `jmp [rip]`, MinHook's is 5) and proves the function with the bytes after them, grown
until they are unique. MinHook installs over the other jump (its trampoline calls the other detour, which calls the
game). A function too short to be told apart after its first 14 bytes is reached from a unique caller instead. The log
names each one that was already hooked when it was resolved:

```text
SIG_HOOKED,name=pose_finalizer,rva=0x266C294,note=hooked by another module
```

### Calling game functions

A signature proves a function's bytes, not what it takes. Every game function the mod calls is declared once in
`src/engine_calls.h`, with its full type and the name of its signature, and called through `engine::Call`: one
guarded call that catches a fault, counts it per function and logs the first one (`ENGINE_CALL_FAULT`), so the caller
gets `false` instead of a crash. The trampolines of the mod's own detours are not called this way: they pass the
game's own call through. `sigtool.py calls` checks that each declared function's signature covers the first
instruction that reads each parameter (a register, or a stack slot read far in, then by a site signature of its own).

### Addresses a clip keeps

A clip never stores a game address. The places the playback compares (the code that made a sound, an effect or a
blood decal; a static audio object) are stored as a site: the hash of the signature's name and the distance from its
start (`src/game_sig_site.h`, `src/clip_sites.h`). On another build the same name is found again and the place moves
with it. So a clip opens on any build: its META only says whether it was saved on this one. An address no signature
names is kept only on its own build and dropped on another, and an offset past the end of its function (the function
shrank) is dropped too. The site names are part of the clip format: `kFrozenSiteNames` in
`tests/game_sig_site_tests.cpp` lists them, and the tests fail when one is renamed or removed.

## At start

`ScriptMain` resolves every signature once, before any module reads game code or installs a detour
(`replay::sig::ResolveAll`, `src/game_sig.h`):

- **Build 1.0.1491.50 (Steam):** recognised by its PE timestamp, image size and checksum. Each anchor is checked at its
  known address; nothing is searched, so the mod behaves as it did with fixed addresses.
- **Any other exe** (another build, another store): every anchor is searched in the code sections in one pass and used
  only when found exactly once. Derived signatures follow from their parents and are checked where they land.

The log's header has one line for it, and one line per signature that was not found:

```text
SIG_SUMMARY,exe_timestamp=0x65EEC92E,exe_image=0x7397600,exe_checksum=0x5576F9C,known=1,force_scan=0,signatures=...,hint=...,scan=0,derived=...,missing=0,ambiguous=0,...
SIG,name=wind_gate,via=search,result=missing,rva=0x0,rva_1491=0x2B98AF8,moved=0
```

A missing or ambiguous signature turns off only the features that need it, with the error each feature already logs.

## Developer keys

In the developer build (`REDEMPTION_DEV`), under `[RedemptionEditor]`:

- `SigForceScan=1` searches every anchor even on 1.0.1491.50 and logs a `SIG` line for each, with `moved=1` when the
  search landed elsewhere than the known address. It proves the search in the running game.
- `SigDumpCode=1` writes the exe's code, `.pdata`, `.rdata` and `.data` to
  `%LOCALAPPDATA%\RedemptionEditor\code\<timestamp>-<checksum>\` (`SIG_DUMP` line), the input of the tool below. The exe
  on disk has its code encrypted; only a dump of the running game can be read.

## The tool

`tools/sigs/sigtool.py` (Python 3 with `capstone` and `numpy`) works on a dump folder (`--dump`):

- `check` verifies every `sig::Signature` in `src/`: an anchor is unique and on its declared address, a derived one
  lands on its declared address from its parent's, every pattern matches, no pattern keeps an address byte fixed,
  and none runs past the end of its function (the next function is there only by link order).
  It is the gate for any change to a signature.
- `gen RVA --keep N` makes a unique pattern of at least `N` bytes at an address, address bytes masked.
- `xref TARGET` lists the code that references a global or a function, with a unique pattern for each.
- `func RVA` gives the function that holds an address and the offset in it (for `Offset` sites).
- `list` prints every signature with its address and name; `convert FILE` drafts signatures from old byte arrays.
- `calls` checks the functions of `src/engine_calls.h` against the code that reads their parameters.

## When the game updates

1. Run the developer build with `SigDumpCode=1` once; note the `SIG_SUMMARY` and the `SIG` lines.
2. `python tools/sigs/sigtool.py check --dump <the new folder>` lists the signatures the new build broke. Each failure
   is one of: not found (the function changed), more than one match (the pattern got too short for the new code), or a
   derived signature whose parent moved its call.
3. For each, look at the new code (`func`, `xref`, a disassembler) and decide: the same function with other bytes gets
   a new pattern (`gen`); a function that really changed needs its module reviewed, since the offsets and the
   behaviour the mod relies on may be gone.
4. A pattern that must change for the new build no longer matches 1.0.1491.50. Prefer bytes both builds share (check
   the pattern against both dumps); the `rva` field keeps the 1.0.1491.50 address of the code it describes.
