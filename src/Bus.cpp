// ── Bus.cpp ──────────────────────────────────────────────────────────────────
#include "Bus.h"

#include <algorithm>

#include "CPU65816.h"
#include "Cartridge.h"
#include "PPU.h"
#include "SPC700.h"

namespace ding::snes {

Bus::Bus(Cartridge& cart) : cart(cart) {}

uint8_t Bus::read(uint32_t addr24) {
    uint8_t  bank = (addr24 >> 16) & 0xFF;
    uint16_t addr = addr24 & 0xFFFF;
    uint8_t  b    = bank & 0x7F;

    uint8_t v;

    if (bank == 0x7E || bank == 0x7F) {
        v = wram[((bank - 0x7E) << 16) | addr];
    } else if (b <= 0x3F) {
        v = internalRead(bank, addr);
    } else {
        v = cart.read(bank, addr);
    }

    openBus = v & 0xFF;
    return openBus;
}

uint8_t Bus::internalRead(uint8_t bank, uint16_t addr) {
    if (addr <= 0x1FFF) return wram[addr & 0x1FFF];
    if (addr <= 0x20FF) return openBus;
    if (addr <= 0x213F) return ppu ? ppu->regRead(addr) : openBus;
    if (addr <= 0x2143) {
        int n = addr - 0x2140;
        if (spc) {
            const ApuLogEntry* last = apuLog.empty() ? nullptr : &apuLog.back();
            if (last && last->dir == 'R' && last->port == n) {
                uint8_t before = spc->outPorts[n];
                for (int i = 0; i < 512 && spc->outPorts[n] == before; i++) {
                    spc->step();
                }
            }
        }
        uint8_t v2 = spc ? spc->outPorts[n] : 0;
        uint8_t curA = cpu ? static_cast<uint8_t>(cpu->A & 0xFF) : 0;
        ApuLogEntry* last = apuLog.empty() ? nullptr : &apuLog.back();
        if (!last || last->dir != 'R' || last->port != n || last->val != v2 || last->a != curA) {
            apuLog.push_back({'R', static_cast<uint8_t>(n), v2, curA, 1});
            if (apuLog.size() > kApuLogMax) apuLog.erase(apuLog.begin());
        } else {
            last->rep = (last->rep > 0 ? last->rep : 1) + 1;
        }
        return v2;
    }
    if (addr == 0x2180) return wramPortRead();
    if (addr <= 0x21FF) return openBus;
    if (addr >= 0x2200 && addr <= 0x3FFF) return openBus;
    if (addr == 0x4016) {
        if (joyStrobe) return 1;
        uint8_t bit = joyBit1;
        joyBit1 = std::min<uint8_t>(bit + 1, 16);
        return bit >= 16 ? 1 : (joypad[0] >> (15 - bit)) & 1;
    }
    if (addr == 0x4017) {
        if (joyStrobe) return 1;
        uint8_t bit = joyBit2;
        joyBit2 = std::min<uint8_t>(bit + 1, 16);
        return bit >= 16 ? 1 : (joypad[1] >> (15 - bit)) & 1;
    }
    if (addr >= 0x4000 && addr <= 0x41FF) return openBus;
    if (addr >= 0x4200 && addr <= 0x44FF) return internalRegRead(addr);
    if (addr >= 0x4500 && addr <= 0x5FFF) return openBus;
    return cart.read(bank, addr); // $6000-$FFFF: cart SRAM/ROM
}

void Bus::write(uint32_t addr24, uint8_t val) {
    uint8_t  bank = (addr24 >> 16) & 0xFF;
    uint16_t addr = addr24 & 0xFFFF;
    uint8_t  b    = bank & 0x7F;
    val &= 0xFF;

    if (bank == 0x7E || bank == 0x7F) {
        wram[((bank - 0x7E) << 16) | addr] = val;
        return;
    }
    if (b <= 0x3F) {
        internalWrite(bank, addr, val);
        return;
    }
    cart.write(bank, addr, val);
}

void Bus::internalWrite(uint8_t bank, uint16_t addr, uint8_t val) {
    if (addr <= 0x1FFF) { wram[addr & 0x1FFF] = val; return; }
    if (addr <= 0x213F) { if (ppu) ppu->regWrite(addr, val); return; }
    if (addr == 0x2180) { wramPortWrite(val); return; }
    if (addr == 0x2181) { wmaddr = (wmaddr & 0x1FF00) | val; return; }
    if (addr == 0x2182) { wmaddr = (wmaddr & 0x100FF) | (val << 8); return; }
    if (addr == 0x2183) { wmaddr = (wmaddr & 0x0FFFF) | ((val & 1) << 16); return; }
    if (addr >= 0x2140 && addr <= 0x2143) {
        int n = addr - 0x2140;
        apuIn[n] = val;
        if (spc) spc->inPorts[n] = val;
        uint8_t curA = cpu ? static_cast<uint8_t>(cpu->A & 0xFF) : 0;
        apuLog.push_back({'W', static_cast<uint8_t>(n), val, curA, 1});
        if (apuLog.size() > kApuLogMax) apuLog.erase(apuLog.begin());
        return;
    }
    if (addr >= 0x2144 && addr <= 0x217F) return;
    if (addr == 0x4016) {
        if (val & 1) { joyStrobe = true; joyBit1 = 0; joyBit2 = 0; }
        else           joyStrobe = false;
        return;
    }
    if (addr >= 0x4200 && addr <= 0x44FF) { internalRegWrite(addr, val); return; }
    cart.write(bank, addr, val); // $6000-$FFFF: cart SRAM/ROM
}

uint8_t Bus::wramPortRead() {
    uint8_t v = wram[wmaddr & 0x1FFFF];
    wmaddr = (wmaddr + 1) & 0x1FFFF;
    return v;
}

void Bus::wramPortWrite(uint8_t val) {
    wram[wmaddr & 0x1FFFF] = val;
    wmaddr = (wmaddr + 1) & 0x1FFFF;
}

// ── Internal register reads ($4200–$44FF) ─────────────────────────────────
uint8_t Bus::internalRegRead(uint16_t addr) {
    switch (addr) {
        case 0x4210: { uint8_t v = nmiFlag | 0x02; nmiFlag = 0; return v; }
        case 0x4211: { uint8_t v = irqFlag; irqFlag = 0; return v; }
        case 0x4212: {
            uint8_t vbl = (ppu && ppu->vblank) ? 0x80 : 0;
            uint8_t hbl = (ppu && ppu->hblank) ? 0x40 : 0;
            return vbl | hbl | 0x01;
        }
        case 0x4213: return wrio;
        case 0x4214: return divResult & 0xFF;
        case 0x4215: return (divResult >> 8) & 0xFF;
        case 0x4216: return mpyResult & 0xFF;
        case 0x4217: return (mpyResult >> 8) & 0xFF;
        case 0x4218: return joypad[0] & 0xFF;
        case 0x4219: return (joypad[0] >> 8) & 0xFF;
        case 0x421A: return joypad[1] & 0xFF;
        case 0x421B: return (joypad[1] >> 8) & 0xFF;
        case 0x421C: case 0x421D: case 0x421E: case 0x421F: return 0;
        default:
            if (addr >= 0x4300 && addr <= 0x437F) {
                int ch = (addr >> 4) & 7, reg = addr & 0xF;
                const DmaChannel& c = dmaChannels[ch];
                switch (reg) {
                    case 0: return c.ctrl;
                    case 1: return c.destReg;
                    case 2: return c.srcAddr & 0xFF;
                    case 3: return (c.srcAddr >> 8) & 0xFF;
                    case 4: return c.srcBank;
                    case 5: return c.size & 0xFF;
                    case 6: return (c.size >> 8) & 0xFF;
                    case 7: return c.tableAddr & 0xFF;
                    case 8: return (c.tableAddr >> 8) & 0xFF;
                    case 9: return c.tableBank;
                    case 0xA: return c.lineCounter;
                    default: break;
                }
            }
            return openBus;
    }
}

// ── Internal register writes ($4200–$44FF) ────────────────────────────────
void Bus::internalRegWrite(uint16_t addr, uint8_t val) {
    switch (addr) {
        case 0x4200: nmitimen = val; return;
        case 0x4201: wrio     = val; return;
        case 0x4202: wrmpya   = val; return;
        case 0x4203:
            wrmpyb    = val;
            mpyResult = (wrmpya * wrmpyb) & 0xFFFF;
            divResult = wrmpyb;
            return;
        case 0x4204: wrdivl = val; return;
        case 0x4205: wrdivh = val; return;
        case 0x4206: {
            wrdivb = val;
            uint16_t dividend = (wrdivh << 8) | wrdivl;
            if (val == 0) {
                divResult = 0xFFFF;
                modResult = dividend;
            } else {
                divResult = dividend / val;
                modResult = dividend % val;
            }
            mpyResult = modResult;
            return;
        }
        case 0x4207: htime = (htime & 0x100) | val; return;
        case 0x4208: htime = (htime & 0x0FF) | ((val & 1) << 8); return;
        case 0x4209: vtime = (vtime & 0x100) | val; return;
        case 0x420A: vtime = (vtime & 0x0FF) | ((val & 1) << 8); return;
        case 0x420B: mdmaen = val; runDMA(); return;
        case 0x420C: hdmaen = val; return;
        case 0x420D: memsel = val; return;
        default:
            if (addr >= 0x4300 && addr <= 0x437F) {
                int ch = (addr >> 4) & 7, reg = addr & 0xF;
                DmaChannel& c = dmaChannels[ch];
                switch (reg) {
                    case 0: c.ctrl      = val; break;
                    case 1: c.destReg   = val; break;
                    case 2: c.srcAddr   = (c.srcAddr & 0xFF00) | val; break;
                    case 3: c.srcAddr   = (c.srcAddr & 0x00FF) | (val << 8); break;
                    case 4: c.srcBank   = val; break;
                    case 5: c.size      = (c.size & 0xFF00) | val; break;
                    case 6: c.size      = (c.size & 0x00FF) | (val << 8); break;
                    case 7: c.tableAddr = (c.tableAddr & 0xFF00) | val; break;
                    case 8: c.tableAddr = (c.tableAddr & 0x00FF) | (val << 8); break;
                    case 9: c.tableBank = val; break;
                    case 0xA: c.lineCounter = val; break;
                    default: break;
                }
            }
            return;
    }
}

// ── DMA execution ($420B write triggers this) ─────────────────────────────
void Bus::runDMA() {
    static constexpr int PATTERNS[8][4] = {
        {0,-1,-1,-1}, {0,1,-1,-1}, {0,0,-1,-1}, {0,0,1,1},
        {0,1,2,3},    {0,1,-1,-1}, {0,0,-1,-1}, {0,0,1,1},
    };
    static constexpr int PATTERN_LEN[8] = {1,2,2,4,4,2,2,4};

    for (int ch = 0; ch < 8; ch++) {
        if (!(mdmaen & (1 << ch))) continue;
        DmaChannel& c = dmaChannels[ch];
        const int* pattern = PATTERNS[c.ctrl & 0x7];
        int patLen = PATTERN_LEN[c.ctrl & 0x7];
        bool toWRAM = c.ctrl & 0x80;
        bool fixed  = c.ctrl & 0x08;
        bool decr   = c.ctrl & 0x10;

        uint32_t src = (c.srcBank << 16) | c.srcAddr;
        uint32_t count = c.size == 0 ? 0x10000 : c.size;
        uint32_t pi = 0;

        while (count-- > 0) {
            uint16_t destAddr = static_cast<uint16_t>(0x2100 | c.destReg | pattern[pi % patLen]);
            pi++;
            if (toWRAM) {
                write(src, read(destAddr));
            } else {
                write(destAddr, read(src));
            }
            if (!fixed) {
                if (decr) src = (src & 0xFF0000) | ((src - 1) & 0xFFFF);
                else      src = (src & 0xFF0000) | ((src + 1) & 0xFFFF);
            }
        }
    }
    mdmaen = 0;
}

void Bus::triggerNMI() {
    nmiFlag = 0x80;
    if (nmitimen & 0x80) { if (cpu) cpu->pendingNMI = true; }
}

void Bus::hdmaInit() {
    for (int ch = 0; ch < 8; ch++) {
        if (!(hdmaen & (1 << ch))) continue;
        DmaChannel& c = dmaChannels[ch];
        c.tableAddr    = c.srcAddr;
        c.tableBank    = c.srcBank;
        c.hdmaFinished = false;
        c.lineCounter  = read((c.tableBank << 16) | c.tableAddr);
        c.tableAddr    = (c.tableAddr + 1) & 0xFFFF;
        if (c.lineCounter == 0) { c.hdmaFinished = true; continue; }
        if (c.ctrl & 0x40) {
            uint8_t lo = read((c.tableBank << 16) | c.tableAddr);
            c.tableAddr = (c.tableAddr + 1) & 0xFFFF;
            uint8_t hi = read((c.tableBank << 16) | c.tableAddr);
            c.tableAddr = (c.tableAddr + 1) & 0xFFFF;
            c.indirectAddr = (hi << 8) | lo;
        }
    }
}

void Bus::hdmaRun() {
    static constexpr int PATTERNS[8][4] = {
        {0,-1,-1,-1}, {0,1,-1,-1}, {0,0,-1,-1}, {0,0,1,1},
        {0,1,2,3},    {0,1,-1,-1}, {0,0,-1,-1}, {0,0,1,1},
    };
    static constexpr int PATTERN_LEN[8] = {1,2,2,4,4,2,2,4};

    for (int ch = 0; ch < 8; ch++) {
        if (!(hdmaen & (1 << ch))) continue;
        DmaChannel& c = dmaChannels[ch];
        if (c.hdmaFinished) continue;

        const int* pattern = PATTERNS[c.ctrl & 7];
        int patLen = PATTERN_LEN[c.ctrl & 7];
        bool indirect = c.ctrl & 0x40;

        for (int i = 0; i < patLen; i++) {
            uint16_t dest = (0x2100 | c.destReg | pattern[i]) & 0xFFFF;
            uint8_t byte;
            if (indirect) {
                byte = read((c.tableBank << 16) | (c.indirectAddr & 0xFFFF));
                c.indirectAddr = (c.indirectAddr + 1) & 0xFFFF;
            } else {
                byte = read((c.tableBank << 16) | c.tableAddr);
                c.tableAddr = (c.tableAddr + 1) & 0xFFFF;
            }
            write(dest, byte);
        }

        int lo7 = (c.lineCounter & 0x7F) - 1;
        if (lo7 > 0) {
            c.lineCounter = static_cast<uint8_t>((c.lineCounter & 0x80) | lo7);
        } else {
            c.lineCounter = read((c.tableBank << 16) | c.tableAddr);
            c.tableAddr   = (c.tableAddr + 1) & 0xFFFF;
            if (c.lineCounter == 0) { c.hdmaFinished = true; continue; }
            if (indirect) {
                uint8_t lo = read((c.tableBank << 16) | c.tableAddr);
                c.tableAddr = (c.tableAddr + 1) & 0xFFFF;
                uint8_t hi = read((c.tableBank << 16) | c.tableAddr);
                c.tableAddr = (c.tableAddr + 1) & 0xFFFF;
                c.indirectAddr = (hi << 8) | lo;
            }
        }
    }
}

// ── H/V timer IRQ ─────────────────────────────────────────────────────────
void Bus::checkIRQ(int scanline) {
    int mode = (nmitimen >> 4) & 3;
    if (!mode) return;
    bool fire = false;
    if      (mode == 1) fire = true;
    else if (mode == 2) fire = (scanline == vtime);
    else if (mode == 3) fire = (scanline == vtime);
    if (fire) {
        irqFlag = 0x80;
        if (cpu) cpu->pendingIRQ = true;
    }
}

} // namespace ding::snes
