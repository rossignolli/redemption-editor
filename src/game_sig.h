#pragma once

// The signatures of game_sig_core.h resolved in the running game.
//
// ResolveAll runs once at the start of ScriptMain, before any module reads
// game code or puts a detour in. On the 1491.50 exe (Steam: PE timestamp,
// image size and checksum) every signature is checked at its 1491.50
// address and nothing is searched, so the mod behaves exactly as with fixed
// addresses. On any other exe (another build, another store) every
// signature is searched in the code sections in one pass and is used only
// when it is found exactly once. Find then returns the address, or 0: the
// feature that needs it stays off and the log says which signature failed.

#include "game_sig_core.h"

#include <cstdint>
#include <ostream>
#include <string>

namespace replay::sig
{
    // SigForceScan (dev ini): search even on the known exe and log, for every
    // signature, whether the search landed on its 1491.50 address. Before
    // ResolveAll; ignored once the signatures are resolved.
    void Configure(bool forceScan);

    // Resolves every Signature of the module (once; a script restart does
    // not search again).
    void ResolveAll();

    // The address of `s` in the game, 0 when it is not usable.
    std::uintptr_t Find(const Signature& s);

    // A pattern checked at an address (a fragment inside a function found by
    // its signature); false on a malformed pattern or an unreadable address.
    bool MatchesAt(std::uintptr_t address, const char* pattern);

    // The code at `address` starts with another module's detour jump (E9 or
    // FF 25); false when unreadable.
    bool StartsWithJump(std::uintptr_t address);

    // The target of the rel32/disp32 at instruction + dispAt, for an
    // instruction `length` bytes long; 0 when `instruction` is 0 or the read
    // fails.
    std::uintptr_t RipTarget(std::uintptr_t instruction, std::uint32_t dispAt, std::uint32_t length);

    std::uintptr_t ImageBase();
    bool KnownExe();

    // SIG_SUMMARY, plus a SIG line per failed signature (every signature
    // under SigForceScan) and a SIG_HOOKED line per function the mod hooks
    // that another module had already hooked when it was resolved.
    void Report(std::ostream& log);

    // SigDumpCode (dev): the exe's code sections, .pdata, .rdata and .data written into
    // a folder of `parent` named after the exe (PE timestamp and checksum), as
    // the research dumps are (".text_0_.rva_00001000.size_032D7000.bin"...).
    // On a new game build, tools/sigs/sigtool.py check --dump <folder> tells
    // which signatures broke and gen/xref make new ones. `report` is the
    // SIG_DUMP line's fields.
    bool DumpCode(const std::wstring& parent, std::string& report);
}
