// ── Cartridge.h ──────────────────────────────────────────────────────────────
// D!NG SNES core — ROM/SRAM mapping.
// Supports LoROM, HiROM, ExHiROM (map mode $25). Recognises SA-1 / SDD-1 but
// maps them as plain HiROM for now — co-proc stubs are a later milestone.
// Ported from snes-core.js (Cartridge class). No GPL code.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ding::snes {

class Cartridge {
public:
    explicit Cartridge(const std::vector<uint8_t>& data);

    // Read/write via 24-bit SNES address (bank + addr)
    uint8_t read(uint8_t bank, uint16_t addr) const;
    void    write(uint8_t bank, uint16_t addr, uint8_t val);

    // Header-derived metadata
    std::string title;
    std::string mapLabel;   // "LoROM" | "HiROM" | "ExHiROM" (+ co-proc suffix)
    uint8_t mapMode  = 0;
    uint8_t cartType = 0;
    uint8_t romSize  = 0;
    uint8_t ramSize  = 0;
    uint8_t country  = 0;
    uint8_t devId    = 0;
    uint8_t version  = 0;

    bool hiROM   = false;
    bool exHiROM = false;

    // Post copier-header-strip ROM bytes — exactly what RetroAchievements
    // hashes for SNES (see header-detection rule in the .cpp). Use this,
    // not the original file buffer, when computing ROM identity.
    const std::vector<uint8_t>& romBytes() const { return rom; }

    // Battery SRAM — empty if the cartridge has none. Save-state and
    // battery-save (.srm) code both go through these.
    std::vector<uint8_t>&       sramBytes()       { return sram; }
    const std::vector<uint8_t>& sramBytes() const { return sram; }

private:
    // Score a candidate header at a ROM file offset. Higher = more likely real.
    int headerScore(uint32_t offset) const;

    std::vector<uint8_t> rom;
    std::vector<uint8_t> sram;
};

} // namespace ding::snes
