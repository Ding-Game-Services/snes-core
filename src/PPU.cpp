// ── PPU.cpp ──────────────────────────────────────────────────────────────────
#include "PPU.h"

#include <algorithm>

#include "Bus.h"

namespace ding::snes {

PPU::PPU() {
    regs[0x00] = 0x8F; // power-on: forced blank, brightness=15
    pixels.assign(kScreenW * kScreenH, 0xFF1A1428);
}

uint32_t PPU::toARGB(uint16_t c15) const {
    uint32_t r = static_cast<uint32_t>((c15 & 0x1F) * 255 / 31 + 0.5);
    uint32_t g = static_cast<uint32_t>(((c15 >> 5) & 0x1F) * 255 / 31 + 0.5);
    uint32_t b = static_cast<uint32_t>(((c15 >> 10) & 0x1F) * 255 / 31 + 0.5);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

void PPU::prefetchVRAM() {
    uint16_t a = (vramAddr << 1) & 0xFFFF;
    vramRdBuf = vram[a] | (vram[(a + 1) & 0xFFFF] << 8);
}

void PPU::reloadOamAddr() {
    oamByteAddr = (((regs[0x03] & 1) << 8) | regs[0x02]) << 1;
}

uint8_t PPU::regRead(uint16_t addr) {
    int r = addr - 0x2100;
    switch (r) {
        case 0x34: { uint32_t p = (m7a * (m7b >> 8)) & 0xFFFFFF; return p & 0xFF; }
        case 0x35: { uint32_t p = (m7a * (m7b >> 8)) & 0xFFFFFF; return (p >> 8) & 0xFF; }
        case 0x36: { uint32_t p = (m7a * (m7b >> 8)) & 0xFFFFFF; return (p >> 16) & 0xFF; }
        case 0x38: {
            uint8_t v = vramRdBuf & 0xFF;
            if (!vramIncOnHi) { vramAddr = (vramAddr + vramInc) & 0x7FFF; prefetchVRAM(); }
            return v;
        }
        case 0x39: {
            uint8_t v = (vramRdBuf >> 8) & 0xFF;
            if (vramIncOnHi) { vramAddr = (vramAddr + vramInc) & 0x7FFF; prefetchVRAM(); }
            return v;
        }
        case 0x3B: {
            uint8_t v;
            if (!cgramLatch) { v = cgram[cgramAddr & 0xFF] & 0xFF; cgramLatch = true; }
            else { v = (cgram[cgramAddr & 0xFF] >> 8) & 0x7F; cgramAddr = (cgramAddr + 1) & 0xFF; cgramLatch = false; }
            return v;
        }
case 0x3E:
            // bit7=time over, bit6=range over, bit5=master/slave (unused),
            // bits4-0=PPU1 open-bus/version (kept as the old hardcoded 0x01).
            return (spriteTimeOver ? 0x80 : 0) | (spriteRangeOver ? 0x40 : 0) | 0x01;
        case 0x3F: {
            // STAT78: bit7=interlace field (unused, always 0 - no interlace),
            // bit5=PAL/NTSC (0=NTSC, hardcoded until region switching exists),
            // bit4=ppu1/2 version marker, bits3-0=CPU version.
            // Real hardware also resets the Mode 7 h/v-counter latch on this
            // read; we don't latch counters yet so nothing to clear here.
            return 0x02;
        }
        default:
            return (r >= 0 && r < static_cast<int>(regs.size())) ? regs[r] : 0;
    }
}

void PPU::regWrite(uint16_t addr, uint8_t val) {
    int r = addr - 0x2100;
    if (r >= 0 && r < static_cast<int>(regs.size())) regs[r] = val;
    switch (r) {
        case 0x02: oamByteAddr = (((regs[0x03] & 1) << 8) | val) << 1; break;
        case 0x03: oamByteAddr = (((val & 1) << 8) | regs[0x02]) << 1; break;
case 0x04:
            if (oamByteAddr < 0x200) {
                if (oamByteAddr & 1) {
                    if (oamVramAccessOk()) { oam[oamByteAddr ^ 1] = oamLow; oam[oamByteAddr] = val; }
                } else {
                    oamLow = val;
                }
            } else if (oamByteAddr < 0x220) {
                if (oamVramAccessOk()) oam[oamByteAddr] = val;
            }
            oamByteAddr = (oamByteAddr + 1) & 0x3FF;
            break;
        case 0x0D: bgH[0] = ((val << 8) | (m7prev & 0xF8) | ((bgH[0] >> 8) & 0x07)) & 0x3FF; m7prev = val; break;
case 0x0E: bgV[0] = ((val << 8) | m7prev) & 0x3FF; m7prev = val; break;
case 0x0F: bgH[1] = ((val << 8) | (bgPrev & 0xF8) | ((bgH[1] >> 8) & 0x07)) & 0x3FF; bgPrev = val; break;
case 0x10: bgV[1] = ((val << 8) | bgPrev) & 0x3FF; bgPrev = val; break;
case 0x11: bgH[2] = ((val << 8) | (bgPrev & 0xF8) | ((bgH[2] >> 8) & 0x07)) & 0x3FF; bgPrev = val; break;
case 0x12: bgV[2] = ((val << 8) | bgPrev) & 0x3FF; bgPrev = val; break;
case 0x13: bgH[3] = ((val << 8) | (bgPrev & 0xF8) | ((bgH[3] >> 8) & 0x07)) & 0x3FF; bgPrev = val; break;
case 0x14: bgV[3] = ((val << 8) | bgPrev) & 0x3FF; bgPrev = val; break;
        case 0x15: {
            static constexpr uint8_t incs[4] = {1, 32, 128, 128};
            vramInc = incs[val & 3];
            vramIncOnHi = (val & 0x80) != 0;
            break;
        }
        case 0x16: vramAddr = (vramAddr & 0x7F00) | val; prefetchVRAM(); break;
        case 0x17: vramAddr = (vramAddr & 0x00FF) | ((val & 0x7F) << 8); prefetchVRAM(); break;
case 0x18:
            if (oamVramAccessOk()) vram[(vramAddr << 1) & 0xFFFF] = val;
            if (!vramIncOnHi) { vramAddr = (vramAddr + vramInc) & 0x7FFF; prefetchVRAM(); }
            break;
        case 0x19:
            if (oamVramAccessOk()) vram[((vramAddr << 1) + 1) & 0xFFFF] = val;
            if (vramIncOnHi) { vramAddr = (vramAddr + vramInc) & 0x7FFF; prefetchVRAM(); }
            break;
        case 0x1B: m7a = (val << 8) | m7prev; m7prev = val; break;
        case 0x1C: m7b = (val << 8) | m7prev; m7prev = val; break;
        case 0x1D: m7c = (val << 8) | m7prev; m7prev = val; break;
        case 0x1E: m7d = (val << 8) | m7prev; m7prev = val; break;
        case 0x1F: m7cx = (val << 8) | m7prev; m7prev = val; break;
        case 0x20: m7cy = (val << 8) | m7prev; m7prev = val; break;
        case 0x21: cgramAddr = val; cgramLatch = false; break;
case 0x22:
            if (!cgramLatch) { cgramBuf = val; cgramLatch = true; }
            else {
                if (cgramAccessOk())
                    cgram[cgramAddr & 0xFF] = ((val & 0x7F) << 8) | cgramBuf;
                cgramAddr = (cgramAddr + 1) & 0xFF;
                cgramLatch = false;
            }
            break;
        case 0x32: {
            uint8_t i = val & 0x1F;
            if (val & 0x20) fixedColor = (fixedColor & ~0x001F) | i;
            if (val & 0x40) fixedColor = (fixedColor & ~0x03E0) | (i << 5);
            if (val & 0x80) fixedColor = (fixedColor & ~0x7C00) | (i << 10);
            break;
        }
        default: break;
    }
}

std::array<PPU::Px, kScreenW> PPU::bgLine(int bg, int y, int bpp) {
    std::array<Px, kScreenW> out{};

    uint8_t mosaic = regs[0x06];
    int mSize = ((mosaic >> 4) & 0xF) + 1;
    bool mEn  = (mosaic & (1 << bg)) != 0;
    int mY = (mEn && mSize > 1) ? y - (y % mSize) : y;

    uint8_t bgSC    = regs[0x07 + bg];
    int mapBase = ((bgSC >> 2) & 0x3F) << 10;
    int mapSzX  = (bgSC & 1) ? 64 : 32;
    int mapSzY  = (bgSC & 2) ? 64 : 32;
uint8_t nba     = regs[0x0B + (bg >> 1)];
    int charBase= ((bg & 1) ? (nba >> 4) : (nba & 0xF)) << 12;
    int wpt     = bpp == 2 ? 8 : bpp == 4 ? 16 : 32;
    int pgsW    = mapSzX == 64 ? 2 : 1;
    int mode    = regs[0x05] & 7;
    bool optMode = (mode == 2 || mode == 4 || mode == 6) && (bg == 0 || bg == 1);

    int lastTC = -1;
    uint8_t p0 = 0, p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0, p6 = 0, p7 = 0;
    int palette = 0, prio = 0, xflip = 0;

    for (int x = 0; x < kScreenW; x++) {
        int mx = (mEn && mSize > 1) ? x - (x % mSize) : x;

        int hOfs = bgH[bg], vOfs = bgV[bg];
        if (optMode) {
            auto opt = optOffset(x >> 3);
            if (opt[bg].h >= 0) hOfs = (opt[bg].h & ~7) | (bgH[bg] & 7);
            if (opt[bg].v >= 0) vOfs = opt[bg].v;
        }

        int effX = (mx + hOfs) & ((mapSzX << 3) - 1);
        int effY = (mY + vOfs) & ((mapSzY << 3) - 1);
        int tileRow = effY >> 3;
        int pxRow   = effY & 7;
        int tc = effX >> 3;
        int pc = effX & 7;

        if (tc != lastTC || optMode) {
            lastTC = tc;
            int pgX = (tc >> 5) & 1, pgY = (tileRow >> 5) & 1;
            int page = pgY * pgsW + pgX;
            int ma = mapBase + page * 0x400 + ((tileRow & 31) << 5) + (tc & 31);
            uint8_t lo = vram[(ma << 1) & 0xFFFF], hi = vram[((ma << 1) + 1) & 0xFFFF];
            int e = (hi << 8) | lo;
            int tileNo = e & 0x3FF;
            palette = (e >> 10) & 7; prio = (e >> 13) & 1; xflip = (e >> 14) & 1;
            int yflip = (e >> 15) & 1;
            int row = yflip ? 7 - pxRow : pxRow;
            int wa = (charBase + tileNo * wpt + row) & 0x7FFF;
            p0 = vram[(wa << 1) & 0xFFFF]; p1 = vram[((wa << 1) + 1) & 0xFFFF];
            if (bpp >= 4) {
                p2 = vram[((wa + 8) << 1) & 0xFFFF]; p3 = vram[(((wa + 8) << 1) + 1) & 0xFFFF];
            }
            if (bpp == 8) {
                p4 = vram[((wa + 16) << 1) & 0xFFFF]; p5 = vram[(((wa + 16) << 1) + 1) & 0xFFFF];
                p6 = vram[((wa + 24) << 1) & 0xFFFF]; p7 = vram[(((wa + 24) << 1) + 1) & 0xFFFF];
            }
        }

        int col = xflip ? 7 - pc : pc, bit = 7 - col;
        int ci;
        if (bpp == 2) ci = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1);
        else if (bpp == 4) ci = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3);
        else ci = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3) |
                  (((p4 >> bit) & 1) << 4) | (((p5 >> bit) & 1) << 5) | (((p6 >> bit) & 1) << 6) | (((p7 >> bit) & 1) << 7);

        if (ci == 0) continue; // out[x] stays invalid (== JS null)

        int cgi;
        if (mode == 0) cgi = bg * 32 + palette * 4 + ci;
        else if (bpp == 8) cgi = ci;
        else cgi = palette * (1 << bpp) + ci;
out[x] = { static_cast<uint8_t>(cgi & 0xFF), static_cast<uint8_t>(prio), true, static_cast<uint8_t>(palette & 7) };
    }
    return out;
}

std::array<PPU::Px, kScreenW> PPU::m7Line(int y, std::array<Px, kScreenW>* bg2Out) {
    std::array<Px, kScreenW> out{};
    if (bg2Out) bg2Out->fill(Px{});
    // EXTBG (SETINI $2133 bit6): BG1 always renders the low 7 bits of the
    // tile pixel, regardless of bit7. When EXTBG is on, BG2 renders that
    // SAME low-7-bit color simultaneously (not exclusively) — bit7 is
    // purely BG2's priority bit (0=below, 1=above other layers per the
    // mode 7 priority table), not a BG1-vs-BG2 selector. BG1 typically
    // ends up obscuring low-priority BG2 pixels since it sits above them.
    bool extbg = (regs[0x33] & 0x40) != 0;
    uint8_t sel = regs[0x1A];
    int32_t sx = (static_cast<int32_t>(bgH[0]) << 19) >> 19;
    int32_t sy = (static_cast<int32_t>(bgV[0]) << 19) >> 19;
    int32_t cx = (static_cast<int32_t>(m7cx) << 16) >> 16;
    int32_t cy = (static_cast<int32_t>(m7cy) << 16) >> 16;
    int32_t a  = (static_cast<int32_t>(m7a) << 16) >> 16;
    int32_t b  = (static_cast<int32_t>(m7b) << 16) >> 16;
    int32_t c  = (static_cast<int32_t>(m7c) << 16) >> 16;
    int32_t d  = (static_cast<int32_t>(m7d) << 16) >> 16;
    int32_t ry = y + sy - cy;

 for (int x = 0; x < kScreenW; x++) {
        int32_t rx = x + sx - cx;
        int32_t tx = ((a * rx + b * ry) >> 8) + cx;
        int32_t ty = ((c * rx + d * ry) >> 8) + cy;
        bool outOfRange = (tx < 0 || tx > 1023 || ty < 0 || ty > 1023);
        bool forceTile0 = false;
        if (outOfRange) {
            if (sel & 0x80) {
                if (!(sel & 0x40)) continue; // bits 10: transparent, out[x] stays invalid
                forceTile0 = true;           // bits 11: fill with character 0's bitmap
            } else {
                tx &= 1023; ty &= 1023;       // bits 00/01: wrap
            }
        }
        int mapAddr = ((ty >> 3) & 0x7F) * 128 + ((tx >> 3) & 0x7F);
        uint8_t tileNo = forceTile0 ? 0 : vram[(mapAddr << 1) & 0xFFFF];
        int charAddr = (tileNo * 64 + ((ty & 7) << 3) + (tx & 7)) & 0x7FFF;
        uint8_t ci = vram[((charAddr << 1) + 1) & 0xFFFF];
if (extbg) {
            uint8_t color = ci & 0x7F;
            if (color != 0) {
                out[x] = { color, 0, true };
                if (bg2Out) (*bg2Out)[x] = { color, static_cast<uint8_t>((ci >> 7) & 1), true };
            }
        } else if (ci != 0) {
            out[x] = { ci, 0, true };
        }
    }
    return out;
}

std::array<PPU::Px, kScreenW> PPU::sprLine(int y) {
    std::array<Px, kScreenW> out{};
    uint8_t obsel = regs[0x01];
    static constexpr int SZ[8][4] = {
        {8,8,16,16}, {8,8,32,32}, {8,8,64,64}, {16,16,32,32},
        {16,16,64,64}, {32,32,64,64}, {16,32,32,64}, {16,32,32,64},
    };
    // OBJSEL layout is SSSN NbBB (bit7->bit0): size is bits 7-5, name base
    // is bits 2-0. These were swapped — size table was indexed by the name
    // base bits and vice versa, which silently pointed sprite tile fetches
    // at the wrong (but valid-looking, cleanly-decoded) VRAM region.
    const int* sz = SZ[(obsel >> 5) & 7];
    int nb0 = (obsel & 7) << 13;
    int nb1 = nb0 + ((((obsel >> 3) & 3) + 1) << 12);

    // Sprites that survived the 32-per-line limit, kept around so the
    // separate 34-sliver limit can be evaluated afterward.
    struct VisSprite {
        int xPos = 0, spW = 0, sprRow = 0, hflip = 0;
        uint8_t tileNo = 0, attr = 0;
        int chrBase = 0;
        int totalSlivers = 0;
        int acceptedSlivers = 0; // filled in by the 34-sliver pass below
    };
    std::vector<VisSprite> vis;
    vis.reserve(32);

    int count = 0;
    for (int s = 0; s < 128; s++) {
        int o4 = s << 2;
        int eb = (oam[0x200 + (s >> 2)] >> ((s & 3) << 1)) & 3;
        int large = (eb >> 1) & 1;
        int spW = large ? sz[2] : sz[0], spH = large ? sz[3] : sz[1];
        uint8_t yPos = oam[o4 + 1];
        int row = (y - yPos) & 0xFF;
        if (row >= spH) continue;

        uint8_t xLo = oam[o4];
        int xPos = (eb & 1) ? xLo - 256 : xLo;

        // Real hardware only counts onscreen sprites toward the 32-per-line
        // limit, EXCEPT a PPU bug that also counts sprites at exactly
        // X=-256 ($100) even though they're fully offscreen.
        bool onscreen = !(xPos + spW <= 0 || xPos >= kScreenW);
        bool xBugCase = (xPos == -256);
if (!onscreen && !xBugCase) continue;
        if (count >= 32) { spriteRangeOver = true; break; }
        count++;
        if (!onscreen) continue;

        uint8_t tileNo = oam[o4 + 2];
        uint8_t attr = oam[o4 + 3];
        int nameT = attr & 1;
        int hflip = (attr >> 6) & 1;
        int vflip = (attr >> 7) & 1;

        VisSprite v;
        v.xPos = xPos;
        v.spW = spW;
        v.sprRow = vflip ? spH - 1 - row : row;
        v.hflip = hflip;
        v.tileNo = tileNo;
        v.attr = attr;
        v.chrBase = nameT ? nb1 : nb0;
        v.totalSlivers = (spW + 7) / 8;
        vis.push_back(v);
    }

    // ── 34-sliver-per-line limit ──────────────────────────────────────────
    // A separate, stricter budget from the 32-sprite limit above: the
    // scanline is chopped into 8px slivers and only 34 of them can be drawn.
    // Per hardware, this is evaluated in REVERSE OAM order — the
    // highest-index sprite claims budget first, so low-index (highest
    // draw-priority) sprites are the ones that lose slivers once the
    // budget runs out. Within a single sprite, slivers are consumed
    // left-to-right in screen space, so an over-budget sprite loses its
    // rightmost slivers first.
    {
int budget = 34;
        for (int i = static_cast<int>(vis.size()) - 1; i >= 0; i--) {
            VisSprite& v = vis[i];
            int take = std::min(v.totalSlivers, budget);
            if (take < v.totalSlivers) spriteTimeOver = true;
            v.acceptedSlivers = take;
            budget -= take;
        }
    }

    // ── Draw pass — ascending OAM order so low index still wins overlaps ──
    for (auto& v : vis) {
        if (v.acceptedSlivers <= 0) continue;
        int visibleW = std::min(v.spW, v.acceptedSlivers * 8);
        int pal  = (v.attr >> 1) & 7;
        int prio = (v.attr >> 4) & 3;

        for (int px = 0; px < visibleW; px++) {
            int sx = v.xPos + px;
            if (sx < 0 || sx >= kScreenW || out[sx].valid) continue;
            int col = v.hflip ? v.spW - 1 - px : px;
            int stX = (col >> 3) & 0xF, stY = (v.sprRow >> 3) & 0xF;
            int ptX = col & 7, ptY = v.sprRow & 7;
            int sub = ((((v.tileNo >> 4) + stY) & 0xF) << 4) | ((v.tileNo + stX) & 0xF);
            int wa = (v.chrBase + (sub & 0xFF) * 16 + ptY) & 0x7FFF;
            uint8_t p0 = vram[(wa << 1) & 0xFFFF];
            uint8_t p1 = vram[((wa << 1) + 1) & 0xFFFF];
            uint8_t p2 = vram[((wa + 8) << 1) & 0xFFFF];
            uint8_t p3 = vram[(((wa + 8) << 1) + 1) & 0xFFFF];
            int bit = 7 - ptX;
            int ci = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3);
            if (ci == 0) continue;
            out[sx] = { static_cast<uint8_t>((128 + pal * 16 + ci) & 0xFF), static_cast<uint8_t>(prio), true };
        }
    }
    return out;
}

std::vector<PPU::LayerEntry> PPU::layers(int mode) const {
    bool bg3hi = (regs[0x05] & 8) != 0;
    if (mode == 0) return {
        {1,0,3},{0,0,1},{0,1,1},{1,0,2},{0,0,0},{0,1,0},
        {1,0,1},{0,2,1},{0,3,1},{1,0,0},{0,2,0},{0,3,0},
    };
    if (mode == 1 && bg3hi) return {
        {0,2,1},{1,0,3},{0,0,1},{0,1,1},{1,0,2},{0,0,0},{0,1,0},{1,0,1},{1,0,0},{0,2,0},
    };
    if (mode == 1) return {
        {1,0,3},{0,0,1},{0,1,1},{1,0,2},{0,0,0},{0,1,0},{1,0,1},{0,2,1},{1,0,0},{0,2,0},
    };
    return {
        {1,0,3},{0,0,1},{1,0,2},{0,1,1},{1,0,1},{0,0,0},{1,0,0},{0,1,0},
    };
}

std::array<PPU::OptCol, 2> PPU::optOffset(int tileCol) const {
    std::array<OptCol, 2> out{};
    int mode = regs[0x05] & 7;
    if (mode != 2 && mode != 4 && mode != 6) return out;
    if (tileCol == 0) return out; // leftmost tile is never affected

    uint8_t bg3SC = regs[0x09];
    int mapBase = ((bg3SC >> 2) & 0x3F) << 10;
    int mapSzX  = (bg3SC & 1) ? 64 : 32;
    int mapSzY  = (bg3SC & 2) ? 64 : 32;

    // BG3's own HOFS/VOFS pick which part of ITS tilemap the offset data
    // comes from — low 3 bits are ignored (matches real hardware: fine
    // scroll is meaningless for a whole-tile lookup). Previously this was
    // hardcoded to column (tileCol-1) and rows 0/1, so any game using
    // BG3HOFS/BG3VOFS to scroll or page-switch its offset data (including
    // via HDMA) was reading the wrong tiles entirely.
    int colBase = (tileCol - 1) + (bgH[2] >> 3);
    int col     = colBase & (mapSzX - 1);
    int rowBase = bgV[2] >> 3;
    int rowH    = rowBase & (mapSzY - 1);
    int rowV    = (rowBase + 1) & (mapSzY - 1); // spec: vertical row reads as if VOFS+8

    auto fetchEntry = [&](int row) -> uint16_t {
        int pgX = (col >> 5) & 1, pgY = (row >> 5) & 1;
        int page = pgY * (mapSzX == 64 ? 2 : 1) + pgX;
        int ma = mapBase + page * 0x400 + ((row & 31) << 5) + (col & 31);
        uint8_t lo = vram[(ma << 1) & 0xFFFF], hi = vram[((ma << 1) + 1) & 0xFFFF];
        return (hi << 8) | lo;
    };

    if (mode == 4) {
        uint16_t e = fetchEntry(rowH);
        int val = e & 0x1FFF;
        bool toBG1 = e & 0x2000, toBG2 = e & 0x4000, isV = e & 0x8000;
        if (isV) { if (toBG1) out[0].v = val; if (toBG2) out[1].v = val; }
        else     { if (toBG1) out[0].h = val; if (toBG2) out[1].h = val; }
    } else {
        uint16_t eh = fetchEntry(rowH), ev = fetchEntry(rowV);
        int hval = eh & 0x1FFF, vval = ev & 0x1FFF;
        if (eh & 0x2000) out[0].h = hval;
        if (eh & 0x4000) out[1].h = hval;
        if (ev & 0x2000) out[0].v = vval;
        if (ev & 0x4000) out[1].v = vval;
    }
    return out;
}

std::array<uint8_t, kScreenW> PPU::buildWinMask(int layer) const {
    uint8_t cfgReg; int shift;
    if (layer <= 1)      { cfgReg = regs[0x23]; shift = layer * 4; }
    else if (layer <= 3) { cfgReg = regs[0x24]; shift = (layer - 2) * 4; }
    else                 { cfgReg = regs[0x25]; shift = (layer - 4) * 4; }
    int cfg = (cfgReg >> shift) & 0xF;

    bool w1en = (cfg & 0x2) != 0, w1inv = (cfg & 0x1) != 0;
    bool w2en = (cfg & 0x8) != 0, w2inv = (cfg & 0x4) != 0;

    uint8_t w1l = regs[0x26], w1r = regs[0x27];
    uint8_t w2l = regs[0x28], w2r = regs[0x29];

    uint8_t logReg; int logShift;
    if (layer <= 3) { logReg = regs[0x2A]; logShift = layer * 2; }
    else            { logReg = regs[0x2B]; logShift = (layer - 4) * 2; }
    int logOp = (logReg >> logShift) & 0x3;

    std::array<uint8_t, kScreenW> mask{};
    for (int x = 0; x < kScreenW; x++) {
        bool in1 = w1en ? ((x >= w1l && x <= w1r) != w1inv) : false;
        bool in2 = w2en ? ((x >= w2l && x <= w2r) != w2inv) : false;
        if (!w1en && !w2en) { mask[x] = 1; continue; }
        if (w1en && !w2en)  { mask[x] = in1 ? 1 : 0; continue; }
        if (!w1en && w2en)  { mask[x] = in2 ? 1 : 0; continue; }
        bool v;
        switch (logOp) {
            case 0: v = in1 || in2;  break;
            case 1: v = in1 && in2;  break;
            case 2: v = in1 != in2;  break;
            default: v = in1 == in2; break;
        }
        mask[x] = v ? 1 : 0;
    }
    return mask;
}

uint16_t PPU::blendC(uint16_t main, uint16_t sub, int op, bool half) const {
    int r = main & 0x1F, g = (main >> 5) & 0x1F, b = (main >> 10) & 0x1F;
    int sr = sub & 0x1F, sg = (sub >> 5) & 0x1F, sb = (sub >> 10) & 0x1F;
    if (op == 0) { r += sr; g += sg; b += sb; }
    else         { r -= sr; g -= sg; b -= sb; }
    if (half) { r >>= 1; g >>= 1; b >>= 1; }
    r = std::clamp(r, 0, 31);
    g = std::clamp(g, 0, 31);
    b = std::clamp(b, 0, 31);
    return static_cast<uint16_t>(r | (g << 5) | (b << 10));
}

void PPU::renderScanline(int y) {
    int base = y * kScreenW;
    uint8_t inidisp = regs[0x00];
    if (inidisp & 0x80) {
        std::fill(pixels.begin() + base, pixels.begin() + base + kScreenW, 0xFF000000u);
        return;
    }

    int brightness = inidisp & 0xF;
    uint8_t tm = regs[0x2C];
    uint8_t ts = regs[0x2D];
    int mode = regs[0x05] & 7;
    static constexpr int bppT[8][4] = {
        {2,2,2,2}, {4,4,2,0}, {4,4,0,0}, {8,4,0,0},
        {8,2,0,0}, {4,2,0,0}, {4,0,0,0}, {0,0,0,0},
    };

uint8_t cgwsel  = regs[0x30];
    uint8_t cgadsub = regs[0x31];
    bool cmAdd   = !(cgadsub & 0x80);
    bool cmHalf  = (cgadsub & 0x40) != 0;
    int  cmEn    = cgadsub & 0x3F;

    // CGWSEL $2130: MMSS ..AD
    //   bit0 (D) = direct color enable
    //   bit1 (A) = Addend source: 0 = fixed color, 1 = sub screen
    //   bits4-5 (SS) = sub screen "force transparent" region (color window)
    //   bits6-7 (MM) = main screen "force black" region (color window)
    // Region encoding for both MM and SS: 0=nowhere, 1=outside window,
    // 2=inside window, 3=everywhere.
    bool addendIsSubscreen = (cgwsel & 0x02) != 0;
    int  mainBlackSel      = (cgwsel >> 6) & 0x3;
    int  subTransSel       = (cgwsel >> 4) & 0x3;

    bool anyWin = (regs[0x23] | regs[0x24] | regs[0x25]) != 0;
    uint8_t tmWin = regs[0x2E];
    uint8_t tsWin = regs[0x2F];
    bool needWin = anyWin && (tmWin || tsWin || cmEn);

    std::array<bool, 6> haveWin{};
    std::array<std::array<uint8_t, kScreenW>, 6> layerWin{};
auto getWin = [&](int li) -> const std::array<uint8_t, kScreenW>& {
        if (!haveWin[li]) { layerWin[li] = buildWinMask(li); haveWin[li] = true; }
        return layerWin[li];
    };

    // Evaluates one of CGWSEL's MM/SS region selectors against the color
    // window (window index 5) at column x. sel: 0=nowhere,1=outside,
    // 2=inside,3=everywhere.
    auto regionActive = [&](int sel, int x) -> bool {
        if (sel == 0) return false;
        if (sel == 3) return true;
        const auto& cwm = getWin(5);
        bool inside = cwm[x] != 0;
        return sel == 2 ? inside : !inside;
    };

    std::array<std::array<Px, kScreenW>, 4> bgL{};
    std::array<bool, 4> haveBg{};
    std::array<Px, kScreenW> sprL{};
    bool haveSpr = false;

    if (mode == 7) {
        bool extbg = (regs[0x33] & 0x40) != 0;
        bool need0 = ((tm | ts) & 0x01) != 0;
        bool need1 = extbg && ((tm | ts) & 0x02) != 0;
        if (need0 || need1) {
            std::array<Px, kScreenW> bg2{};
            std::array<Px, kScreenW> bg1 = m7Line(y, need1 ? &bg2 : nullptr);
            if (need0) { bgL[0] = bg1; haveBg[0] = true; }
            if (need1) { bgL[1] = bg2; haveBg[1] = true; }
        }
    } else {
        for (int bg = 0; bg < 4; bg++) {
            int bpp = bppT[mode][bg];
            if (bpp && ((tm | ts) >> bg) & 1) { bgL[bg] = bgLine(bg, y, bpp); haveBg[bg] = true; }
        }
    }
    if ((tm | ts) & 0x10) { sprL = sprLine(y); haveSpr = true; }

auto layerList = layers(mode);
    // Default sub-screen source: backdrop when Addend=subscreen (normal
    // sub-screen compositing falls through to CGRAM 0 when nothing draws),
    // or the fixed color when Addend=fixed color.
    uint16_t subBD = addendIsSubscreen ? cgram[0] : fixedColor;

    // Direct Color mode (CGWSEL bit0): bypasses CGRAM for 8bpp BG1 in modes
    // 3/4 and for Mode 7 BG1/EXTBG-BG2 — those layers never use a CGRAM
    // sub-palette on real hardware, so `cgi` there is already the raw
    // 3/3/2-bit-packed index; just spread it into a 5/5/5 color directly.
bool directColorEn = (cgwsel & 0x01) != 0;
    // attr bit0=r, bit1=g, bit2=b — the tilemap's palette-selection field,
    // repurposed as extra low-order color bits in Direct Color mode (see
    // Tiles wiki: "RRRr0 GGGg0 BBb00").
    auto directColor = [](uint8_t idx, uint8_t attr) -> uint16_t {
        int r = ((idx & 0x07) << 2) | (((attr >> 0) & 1) << 1);
        int g = (((idx >> 3) & 0x07) << 2) | (((attr >> 1) & 1) << 1);
        int b = (((idx >> 6) & 0x03) << 3) | (((attr >> 2) & 1) << 2);
        return static_cast<uint16_t>(r | (g << 5) | (b << 10));
    };
    auto isDirectLayer = [&](int layerIdx) {
        return directColorEn && layerIdx == 0 && (mode == 3 || mode == 4 || mode == 7);
    };

    for (int x = 0; x < kScreenW; x++) {
        int mainCGI = 0, mainLayer = -1; uint8_t mainAttr = 0;
        for (auto& layer : layerList) {
            int lIdx = layer.isSprite == 1 ? 4 : layer.idx;
            int tmBit = layer.isSprite == 1 ? 0x10 : (1 << layer.idx);
            if (!(tm & tmBit)) continue;
if (needWin && (tmWin & tmBit)) {
                const auto& wm = getWin(lIdx);
                // wm[x]==1 means "window is hiding this layer here" (per spec:
                // "0 when showing, 1 when hiding") — skip when true, not false.
                if (wm[x]) continue;
            }
            if (layer.isSprite == 1) {
                if (!haveSpr) continue;
                const Px& sp = sprL[x];
                if (sp.valid && sp.prio == layer.prio) { mainCGI = sp.cgi; mainLayer = 4; break; }
            } else {
                int bg = layer.idx;
                if (!haveBg[bg]) continue;
                const Px& px = bgL[bg][x];
                if (px.valid && px.prio == layer.prio) { mainCGI = px.cgi; mainLayer = bg; mainAttr = px.attr; break; }
            }
        }

uint16_t subC15 = subBD;
        if (addendIsSubscreen && cmEn) {
            for (auto& layer : layerList) {
                int tsBit = layer.isSprite == 1 ? 0x10 : (1 << layer.idx);
                if (!(ts & tsBit)) continue;
                int lIdx = layer.isSprite == 1 ? 4 : layer.idx;
if (needWin && (tsWin & tsBit)) {
                    const auto& wm = getWin(lIdx);
                    if (wm[x]) continue;
                }
                if (layer.isSprite == 1) {
                    if (!haveSpr) continue;
                    const Px& sp = sprL[x];
                    if (sp.valid && sp.prio == layer.prio) { subC15 = cgram[sp.cgi]; break; }
                } else {
                    int bg = layer.idx;
                    if (!haveBg[bg]) continue;
                    const Px& px = bgL[bg][x];
                    if (px.valid && px.prio == layer.prio) {
                        subC15 = isDirectLayer(bg) ? directColor(px.cgi, px.attr) : cgram[px.cgi];
                        break;
                    }
                }
            }
        }

// Sub screen "force transparent" region (CGWSEL SS bits): forces the
        // math operand back to backdrop within the selected region. Only
        // meaningful when Addend is actually the sub screen — fixed color
        // has no "layers" for this to mask.
        if (addendIsSubscreen && regionActive(subTransSel, x)) subC15 = cgram[0];

        uint16_t c15 = isDirectLayer(mainLayer) ? directColor(static_cast<uint8_t>(mainCGI), mainAttr) : cgram[mainCGI];

        // Main screen "force black" region (CGWSEL MM bits): replaces the
        // main screen color with black within the selected region, BEFORE
        // color math runs — math still applies against the forced-black
        // value (this is the correct behavior; some older emulators like
        // ZSNES got this wrong and skipped math entirely here).
        if (regionActive(mainBlackSel, x)) c15 = 0;

        bool doCM = false;
        // Sprites using palettes 0-3 always reject color math, regardless of
        // the OBJ bit in CGADSUB — this lets a game mix opaque and
        // color-math'd sprites on the main screen at the same time.
        bool spriteRejectsCM = (mainLayer == 4 && mainCGI < 192);
        if (cmEn && !spriteRejectsCM) {
            int lbit = mainLayer == -1 ? 0x20 : mainLayer == 4 ? 0x10 : (1 << mainLayer);
            if (cmEn & lbit) doCM = true;
        }
        if (doCM) c15 = blendC(c15, subC15, cmAdd ? 0 : 1, cmHalf);

        uint32_t rgba = toARGB(c15);
        if (brightness < 15) {
            double sc = brightness / 15.0;
            uint32_t r = static_cast<uint32_t>((rgba & 0xFF) * sc + 0.5);
            uint32_t g = static_cast<uint32_t>(((rgba >> 8) & 0xFF) * sc + 0.5);
            uint32_t b = static_cast<uint32_t>(((rgba >> 16) & 0xFF) * sc + 0.5);
            rgba = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
        pixels[base + x] = rgba;
    }
}

PPU::Snapshot PPU::ppuSnapshot() const {
    return {
        frame, scanline, vblank,
        static_cast<uint8_t>(regs[0x05] & 7),
        (regs[0x05] & 8) != 0,
        regs[0x2C], regs[0x00],
    };
}

bool PPU::advance(int masterClocks) {
    clk += masterClocks;
    bool done = false;
    while (clk >= kLineMC) {
        clk -= kLineMC;
        int sl = scanline;
        if (sl < kScreenH) renderScanline(sl);
        scanline++;
        if (scanline == kScreenH) {
            vblank = true;
            reloadOamAddr();
            if (bus) bus->triggerNMI();
        }
        if (scanline >= kScanlines) {
            scanline = 0; vblank = false; frame++; done = true;
        }
    }
    hblank = clk >= kHblankMC;
    return done;
}

} // namespace ding::snes
