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

    // STAT77 ($213E) sprite-limit flags, refreshed once per scanline in sprLine().
    bool spriteRangeOver = false; // >32 sprites intersected this line
    bool spriteTimeOver  = false; // 34-sliver budget was exceeded

    std::array<uint16_t, 4> bgH{}, bgV{};
    uint8_t m7prev = 0, bgPrev = 0;

    int16_t m7a = 0x0100, m7b = 0, m7c = 0, m7d = 0x0100;
    int16_t m7cx = 0, m7cy = 0;

    uint16_t fixedColor = 0;

    // ARGB8888 framebuffer, kScreenW * kScreenH
    std::vector<uint32_t> pixels;

    struct Snapshot {
        uint32_t frame; int scanline; bool vblank;
        uint8_t  mode; bool bg3hi; uint8_t tm; uint8_t inidisp;
    };
    Snapshot ppuSnapshot() const;

private:
    uint32_t toARGB(uint16_t bgr15) const;
    void     prefetchVRAM();

    // Re-latches oamByteAddr from the last-written $2102/$2103 values.
    // Hardware does this at the start of every vblank (and continuously
    // during forced blank) so games can stream OAM from a fixed address
    // each frame without rewriting $2102/$2103 every time.
    void     reloadOamAddr();

    // Real hardware ignores VRAM/OAM writes outside vblank+forceblank, and
    // CGRAM writes outside vblank+hblank+forceblank — writing at the wrong
    // time still advances the address/latch, but leaves RAM contents alone.
    // Not gating this let writes that spill past the blanking window (e.g.
    // from imprecise CPU cycle timing) corrupt whatever OAM/VRAM byte the
    // address counter was sitting on — the likely cause of "correct shape,
    // wrong content" sprite glitches (OAM entries ending up with stale/
    // wrong tile numbers, like font tiles instead of character frames).
    bool     forceBlank() const { return (regs[0x00] & 0x80) != 0; }
    bool     oamVramAccessOk() const { return vblank || forceBlank(); }
    bool     cgramAccessOk() const { return vblank || hblank || forceBlank(); }

    // One rendered background/sprite pixel: cgi = CGRAM color index, prio = priority bit.
    // `valid` mirrors the JS `out[x] === null` (no opaque pixel at this x).
    // attr: for Direct Color layers, the tilemap's 3-bit palette-selection
    // field is repurposed as extra r/g/b bits (bit0=r, bit1=g, bit2=b) — see
    // Tiles wiki "8bpp Direct Color". Unused/zero for non-direct-color pixels.
    struct Px { uint8_t cgi = 0; uint8_t prio = 0; bool valid = false; uint8_t attr = 0; };

    std::array<Px, kScreenW> bgLine(int bg, int y, int bpp);
    std::array<Px, kScreenW> m7Line(int y, std::array<Px, kScreenW>* bg2Out = nullptr);
    std::array<Px, kScreenW> sprLine(int y);

    // layers(): each entry is {isSprite(0/1), bgIndex, priority}
    struct LayerEntry { int isSprite; int idx; int prio; };
    std::vector<LayerEntry> layers(int mode) const;

    std::array<uint8_t, kScreenW> buildWinMask(int layer) const;

    // Offset-per-tile (mode 2/4/6): per-tile-column scroll overrides for
    // BG1/BG2, sourced from BG3's tilemap. -1 in a field means "no override".
    struct OptCol { int h = -1; int v = -1; };
    std::array<OptCol, 2> optOffset(int tileCol) const; // [0]=BG1, [1]=BG2
    uint16_t blendC(uint16_t main, uint16_t sub, int op, bool half) const;
    void     renderScanline(int y);

    friend class Bus; // needs regRead/regWrite passthrough
};

} // namespace ding::snes
