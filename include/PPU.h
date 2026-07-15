// ── PPU.h ────────────────────────────────────────────────────────────────────
// D!NG SNES core — Picture Processing Unit.
// Ported from snes-core.js (PPU class). No GPL code.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ding::snes {

class Bus;

constexpr int kScreenW = 256, kScreenH = 224;
constexpr int kScanlines = 262, kDotsPerLine = 341;
// NTSC master clock = 21.477272 MHz; 1 scanline = 1364 master clocks.
constexpr int kLineMC = 1364, kHblankMC = 1096;

class PPU {
public:
    PPU();

    uint8_t regRead(uint16_t addr);
    void    regWrite(uint16_t addr, uint8_t val);

    // Advances the PPU by `masterClocks`; returns true when the frame is done.
    bool advance(int masterClocks);

    Bus* bus = nullptr;

    std::array<uint8_t, 0x10000>  vram{};
    std::array<uint16_t, 0x100>   cgram{};
    std::array<uint8_t, 0x220>    oam{};
    std::array<uint8_t, 0x40>     regs{};

    uint32_t clk = 0;
    int      scanline = 0;
    bool     vblank = false, hblank = false;
    uint32_t frame = 0;

    uint16_t vramAddr = 0;
    uint8_t  vramInc = 1;
    bool     vramIncOnHi = false;
    uint16_t vramRdBuf = 0;

    uint8_t cgramAddr = 0;
    uint8_t cgramBuf = 0;
    bool    cgramLatch = false;

    uint16_t oamByteAddr = 0;
    uint8_t  oamLow = 0;

    std::array<uint16_t, 4> bgH{}, bgV{};
    uint8_t m7prev = 0, bgPrev = 0;

    int16_t m7a = 0x0100, m7b = 0, m7c = 0, m7d = 0x0100;
    int16_t m7cx = 0, m7cy = 0;

    uint16_t fixedColor = 0;

    // ARGB8888 framebuffer, kScreenW * kScreenH
    std::vector<uint32_t> pixels;

private:
    uint32_t toARGB(uint16_t bgr15) const;
    void     prefetchVRAM();

    friend class Bus; // needs regRead/regWrite passthrough
};

} // namespace ding::snes
