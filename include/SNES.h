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

#include <array>
#include <cstdint>
#include <map>
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

// Mirrors the JS diagSnap() shape (called ~1x/sec from the frontend debug panel).
// Hex-string fields (vecReset etc.) and bytesAtPC are formatted the same way as
// the JS version so existing frontend debug-panel code needs minimal changes.
struct DiagSnapshot {
    uint16_t pc; uint8_t pbr; uint16_t a, x, y, sp, dp; uint8_t dbr, p; bool e;
    bool pendingNMI, pendingIRQ, stopped, waiting;
    uint64_t cycles;

    uint8_t inidisp, obsel, bgmode, mosaic, tm, ts, cgwsel, cgadsub;
    uint16_t coldata;
    uint16_t vramAddr; uint8_t cgramAddr; uint16_t oamAddr;
    int scanline; uint32_t frame; bool vblank;

    uint8_t nmitimen, nmiFlag, irqFlag;
    uint16_t htime, vtime;
    uint8_t memsel, mdmaen, hdmaen;

    std::array<uint8_t, 4> apuOut, apuIn;
    std::vector<ApuLogEntry> apuLog;

    bool hasSpc;
    uint16_t spcPc; uint8_t spcA, spcX, spcY, spcSp; uint64_t spcCycles;
    std::array<uint8_t, 4> spcOut, spcIn;

    std::string vecReset, vecNMI, vecIRQ, vecBRK; // "$XXXX" formatted
    std::vector<DmaChannel> dma;                  // ctrl/dest/src reformat left to caller

    std::vector<uint32_t> pcHistory;
    std::vector<std::string> bytesAtPC; // 8 bytes at PC, "XX" hex formatted
};

class SNES {
public:
    explicit SNES(Cartridge& cart);

    void runFrame();

    // ── WRAM helpers for Ding achievement engine ──────────────────────────
    uint8_t  memRead(uint32_t addr) const;
    uint16_t memReadU16(uint32_t addr) const;
    std::map<std::string, uint32_t> memSnapshot(const std::vector<MemMapEntry>& memMap) const;

    DiagSnapshot diagSnap() const;

    Cartridge& cart;
    Bus        bus;
    CPU65816   cpu;
    SPC700     spc;
    PPU        ppu;

private:
    double spcAcc = 0.0; // SPC700 clock accumulator (~0.04768 SPC cycles/master clock)
};

} // namespace ding::snes
