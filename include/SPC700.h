// ── SPC700.h ─────────────────────────────────────────────────────────────────
// D!NG SNES core — SPC700 audio CPU + IPL ROM + DSP register file/timers.
// Ported from snes-core.js (SPC700 class). No GPL code.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ding::snes {

class SPC700 {
public:
    SPC700();

    int step(); // executes one instruction, returns cycles consumed

    std::array<uint8_t, 0x10000> ram{};

    uint8_t  A = 0, X = 0, Y = 0;
    uint8_t  SP = 0xFF;
    uint16_t PC = 0xFFC0;

    // Flags: Negative, oVerflow, zero-Page, Break, Half-carry, Interrupt, Zero, Carry
    uint8_t N = 0, V = 0, P = 0, B = 0, H = 0, I = 0, Z = 0, C = 0;

    std::array<uint8_t, 4> inPorts{}, outPorts{};

    std::array<bool, 3>     timerEn{};
    std::array<uint16_t, 3> timerDiv{}, timerTarget{}, timerCycles{};
    std::array<uint8_t, 3>  timerOut{};

std::array<uint8_t, 128> dspRegs{};
    uint8_t dspAddr = 0;
    uint8_t ctrlReg = 0xB0;
    uint64_t cycles = 0;

    std::vector<uint16_t> pcTrace; // ring buffer of recent PCs, for stall diagnosis

private:
    uint8_t rd(uint16_t addr);
    void    wr(uint16_t addr, uint8_t val);
    uint8_t ioRead(uint16_t addr);
    void    ioWrite(uint16_t addr, uint8_t val);
    uint8_t readTimer(int n);

    static constexpr std::array<uint8_t, 64> kIplRom = {
        0xCD,0xEF,0xBD,0xE8,0x00,0xC6,0x1D,0xD0,0xFC,0x8F,0xAA,0xF4,0x8F,0xBB,0xF5,0x78,
        0xCC,0xF4,0xD0,0xFB,0x2F,0x19,0xEB,0xF4,0xD0,0xFC,0x7E,0xF4,0xD0,0x0B,0xE4,0xF5,
        0xCB,0xF4,0xD7,0x00,0xFC,0xD0,0xF3,0xAB,0x01,0x10,0xEF,0x7E,0xF4,0x10,0xEB,0xBA,
        0xF6,0xDA,0x00,0xBA,0xF4,0xC4,0xF4,0xDD,0x5D,0xD0,0xDB,0x1F,0x00,0x00,0xC0,0xFF,
    };
};

} // namespace ding::snes
