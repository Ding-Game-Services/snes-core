// ── SNES.h ───────────────────────────────────────────────────────────────────
// D!NG SNES core — top-level system, wires Cartridge/Bus/CPU/PPU/SPC700 together.
// Ported from snes-core.js (SNES class). No GPL code.
//
// NOTE: memRead/memSnapshot/diagSnap below mirror the JS prototype's hooks for
// the Ding achievement engine. Reconcile against the real ding_core.h /
// ding-core-sdk interface (capability struct, DingBiosDescriptor, MD5 ROM ID)
// once that's pulled in as sdk/ — this header doesn't implement ding_core.h yet.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Bus.h"
#include "CPU65816.h"
#include "Cartridge.h"
#include "PPU.h"
#include "SPC700.h"

namespace ding::snes {

// One entry in a memory map used to build achievement/leaderboard snapshots
struct MemMapEntry {
    std::string key;
    uint32_t    addr;
    bool        u16 = false;
};

class SNES {
public:
    explicit SNES(Cartridge& cart);

    void runFrame();

    // ── WRAM helpers for Ding achievement engine ──────────────────────────
    uint8_t  memRead(uint32_t addr) const;
    uint16_t memReadU16(uint32_t addr) const;
    // Returns key -> value for each entry in memMap (u16 entries read as LE)
    // (concrete return type e.g. std::map<std::string,uint32_t> — left open
    // pending ding_core.h's expected snapshot shape)

    Cartridge& cart;
    Bus        bus;
    CPU65816   cpu;
    SPC700     spc;
    PPU        ppu;

private:
    double spcAcc = 0.0; // SPC700 clock accumulator (~0.04768 SPC cycles/master clock)
};

} // namespace ding::snes
