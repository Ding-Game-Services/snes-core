// ── Cartridge.cpp ────────────────────────────────────────────────────────────
#include "Cartridge.h"

#include <algorithm>
#include <cctype>

namespace ding::snes {

namespace {
constexpr uint32_t kLoROMBase = 0x7FC0;
constexpr uint32_t kHiROMBase = 0xFFC0;
constexpr uint32_t kExHiROMBase = 0x40FFC0;
}

Cartridge::Cartridge(const std::vector<uint8_t>& data) {
    // Strip 512-byte copier header when present (size mod 1024 == 512)
    bool hasHeader = (data.size() & 0x3FF) == 0x200;
    rom.assign(data.begin() + (hasHeader ? 0x200 : 0), data.end());

    int loScore = headerScore(kLoROMBase);
    int hiScore = headerScore(kHiROMBase);
    int exScore = headerScore(kExHiROMBase);

    int best = std::max({loScore, hiScore, exScore});
    exHiROM = (exScore == best && exScore > 0);
    hiROM   = !exHiROM && (hiScore >= loScore);

    uint32_t base = exHiROM ? kExHiROMBase : hiROM ? kHiROMBase : kLoROMBase;

    // Title: 21 bytes, printable ASCII only, trimmed
    std::string rawTitle(rom.begin() + base, rom.begin() + base + 21);
    std::string cleaned;
    for (char c : rawTitle) {
        if (c >= 0x20 && c <= 0x7E) cleaned += c;
    }
    // trim trailing/leading whitespace
    size_t startPos = cleaned.find_first_not_of(' ');
    size_t endPos = cleaned.find_last_not_of(' ');
    title = (startPos == std::string::npos) ? "" : cleaned.substr(startPos, endPos - startPos + 1);
    if (title.empty()) title = "Unknown ROM";

    mapMode  = rom[base + 0x15];
    cartType = rom[base + 0x16];
    romSize  = rom[base + 0x17];
    ramSize  = rom[base + 0x18];
    country  = rom[base + 0x19];
    devId    = rom[base + 0x1A];
    version  = rom[base + 0x1B];

    mapLabel = exHiROM ? "ExHiROM" : hiROM ? "HiROM" : "LoROM";

    // Detect co-processors by cartType upper nibble
    uint8_t chipUpper = (cartType >> 4) & 0xF;
    if (chipUpper == 0x1 && devId == 0x33) mapLabel += "+SA-1";
    if (chipUpper == 0x4)                  mapLabel += "+SDD-1";
    if (chipUpper == 0x2)                  mapLabel += "+DSP";

    // SRAM — clamp to power-of-2 KB; min 0, max 512 KB
    uint32_t sramKB = ramSize > 0 ? std::min<uint32_t>(1u << ramSize, 512u) : 0u;
    sram.assign(sramKB * 1024, 0);
}

int Cartridge::headerScore(uint32_t offset) const {
    if (offset + 0x40 > rom.size()) return -100;
    int score = 0;

    // Checksum complement XOR checksum must equal 0xFFFF — strongest signal
    uint16_t comp = (rom[offset + 0x1D] << 8) | rom[offset + 0x1C];
    uint16_t csum = (rom[offset + 0x1F] << 8) | rom[offset + 0x1E];
    if ((comp ^ csum) == 0xFFFF) score += 6;

    // Map mode byte should be a known value
    uint8_t mm = rom[offset + 0x15];
    static constexpr uint8_t kKnownModes[] = {0x20,0x21,0x22,0x23,0x25,0x30,0x31,0x35,0x3A};
    if (std::find(std::begin(kKnownModes), std::end(kKnownModes), mm) != std::end(kKnownModes))
        score += 3;

    // Reset vector should point into ROM space
    uint16_t reset = (rom[offset + 0x3D] << 8) | rom[offset + 0x3C];
    if (reset >= 0x8000) score += 2;

    // ROM size byte should be plausible (64KB–64MB = codes 6–12)
    uint8_t sz = rom[offset + 0x17];
    if (sz >= 6 && sz <= 12) score += 1;

    // Most title characters should be printable ASCII or null padding
    int printable = 0;
    for (int i = 0; i < 21; i++) {
        uint8_t c = rom[offset + i];
        if ((c >= 0x20 && c <= 0x7E) || c == 0x00) printable++;
    }
    if (printable >= 15) score += 2;

    return score;
}

uint8_t Cartridge::read(uint8_t bank, uint16_t addr) const {
    uint8_t b = bank;
    uint16_t a = addr;

    // ── ExHiROM ────────────────────────────────────────────────────────────
    if (exHiROM) {
        if ((b >= 0x40 && b <= 0x7D) || b >= 0xC0)
            return rom[((b & 0x3F) * 0x10000u + a) % rom.size()];
        if (a >= 0x8000) {
            uint32_t offset = 0x200000u + ((b & 0x3F) * 0x8000u) + (a - 0x8000);
            return rom[offset % rom.size()];
        }
        if (!sram.empty() && a >= 0x6000 && a <= 0x7FFF) {
            uint32_t off = ((b & 0x1F) * 0x2000u) + (a - 0x6000);
            return sram[off % sram.size()];
        }
        return 0;
    }

    // ── HiROM ──────────────────────────────────────────────────────────────
    if (hiROM) {
        if ((b >= 0x40 && b <= 0x7D) || (b >= 0xC0 && b <= 0xFF))
            return rom[((b & 0x3F) * 0x10000u + a) % rom.size()];
        if (a >= 0x8000)
            return rom[((b & 0x3F) * 0x10000u + a) % rom.size()];
        if (!sram.empty() && a >= 0x6000 && a <= 0x7FFF) {
            int bankIdx = b >= 0xA0 ? b - 0xA0 : b - 0x20;
            uint32_t off = (bankIdx * 0x2000u) + (a - 0x6000);
            return sram[off % sram.size()];
        }
        return 0;
    }

    // ── LoROM ──────────────────────────────────────────────────────────────
    if (a >= 0x8000) {
        uint32_t off = ((b & 0x7F) * 0x8000u) + (a - 0x8000);
        return rom[off % rom.size()];
    }
    if (!sram.empty() && ((b >= 0x70 && b <= 0x7D) || (b >= 0xF0 && b <= 0xFF))) {
        int bankBase = b >= 0xF0 ? b - 0xF0 : b - 0x70;
        uint32_t off = (bankBase * 0x8000u) + a;
        return sram[off % sram.size()];
    }
    return 0;
}

void Cartridge::write(uint8_t bank, uint16_t addr, uint8_t val) {
    uint8_t b = bank;
    uint16_t a = addr;
    if (sram.empty()) return;

    if (hiROM || exHiROM) {
        if (a >= 0x6000 && a <= 0x7FFF) {
            int bankIdx = b >= 0xA0 ? b - 0xA0 : b >= 0x20 ? b - 0x20 : 0;
            uint32_t off = (bankIdx * 0x2000u) + (a - 0x6000);
            if (off < sram.size()) sram[off] = val;
        }
    } else {
        if ((b >= 0x70 && b <= 0x7D) || (b >= 0xF0 && b <= 0xFF)) {
            int bankBase = b >= 0xF0 ? b - 0xF0 : b - 0x70;
            uint32_t off = (bankBase * 0x8000u) + a;
            if (off < sram.size()) sram[off] = val;
        }
    }
}

} // namespace ding::snes
