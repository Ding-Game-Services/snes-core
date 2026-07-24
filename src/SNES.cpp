// ── SNES.cpp ─────────────────────────────────────────────────────────────────
#include "SNES.h"

#include <cstdio>

namespace ding::snes {

namespace {
std::string h4(uint16_t v) { char buf[8]; std::snprintf(buf, sizeof(buf), "$%04X", v); return buf; }
}

SNES::SNES(Cartridge& cart) : cart(cart), bus(cart), cpu(bus), spc(), ppu() {
    bus.ppu = &ppu;
    bus.cpu = &cpu;
    bus.spc = &spc;
    ppu.bus = &bus;
    cpu.reset();
    // SPC700 clock ratio: ~1.024 MHz / 21.477272 MHz ≈ 0.04768 SPC cycles per master clock
    spcAcc = 0.0;
}

uint8_t SNES::memRead(uint32_t addr) const {
    return const_cast<Bus&>(bus).wram[addr & 0x1FFFF];
}
uint16_t SNES::memReadU16(uint32_t addr) const {
    return memRead(addr) | (memRead(addr + 1) << 8);
}

std::map<std::string, uint32_t> SNES::memSnapshot(const std::vector<MemMapEntry>& memMap) const {
    std::map<std::string, uint32_t> s;
    for (const auto& e : memMap) {
        s[e.key] = e.u16 ? memReadU16(e.addr) : memRead(e.addr);
    }
    return s;
}

DiagSnapshot SNES::diagSnap() const {
    Bus& b = const_cast<Bus&>(bus);
    CPU65816& c = const_cast<CPU65816&>(cpu);
    PPU& p = const_cast<PPU&>(ppu);
    SPC700& s = const_cast<SPC700&>(spc);

    uint16_t vecReset = b.read(0xFFFC) | (b.read(0xFFFD) << 8);
    uint16_t vecNMI   = b.read(0xFFEA) | (b.read(0xFFEB) << 8);
    uint16_t vecIRQ   = b.read(0xFFEE) | (b.read(0xFFEF) << 8);
    uint16_t vecBRK   = b.read(0xFFE6) | (b.read(0xFFE7) << 8);

    DiagSnapshot d{};
    d.pc = c.PC; d.pbr = c.PBR; d.a = c.A; d.x = c.X; d.y = c.Y;
    d.sp = c.SP; d.dp = c.DP; d.dbr = c.DBR; d.p = c.P; d.e = c.E;
    d.pendingNMI = c.pendingNMI; d.pendingIRQ = c.pendingIRQ;
    d.stopped = c.stopped; d.waiting = c.waiting; d.cycles = c.cycles;

    d.inidisp = p.regs[0x00]; d.obsel = p.regs[0x01];
    d.bgmode  = p.regs[0x05]; d.mosaic = p.regs[0x06];
    d.tm = p.regs[0x2C]; d.ts = p.regs[0x2D];
    d.cgwsel = p.regs[0x30]; d.cgadsub = p.regs[0x31];
    d.coldata = p.fixedColor;
    d.vramAddr = p.vramAddr; d.cgramAddr = p.cgramAddr; d.oamAddr = p.oamByteAddr;
    d.scanline = p.scanline; d.frame = p.frame; d.vblank = p.vblank;

    d.nmitimen = b.nmitimen; d.nmiFlag = b.nmiFlag; d.irqFlag = b.irqFlag;
    d.htime = b.htime; d.vtime = b.vtime; d.memsel = b.memsel;
    d.mdmaen = b.mdmaen; d.hdmaen = b.hdmaen;
    d.apuOut = b.apuOut; d.apuIn = b.apuIn;
    d.apuLog.assign(b.apuLog.begin(), b.apuLog.end());

    d.hasSpc = true;
    d.spcPc = s.PC; d.spcA = s.A; d.spcX = s.X; d.spcY = s.Y; d.spcSp = s.SP;
    d.spcCycles = s.cycles;
    d.spcOut = s.outPorts; d.spcIn = s.inPorts;

    d.vecReset = h4(vecReset); d.vecNMI = h4(vecNMI); d.vecIRQ = h4(vecIRQ); d.vecBRK = h4(vecBRK);
    d.dma.assign(b.dmaChannels.begin(), b.dmaChannels.end());

    d.pcHistory.assign(c.pcHistory.begin(), c.pcHistory.end());
    d.bytesAtPC.reserve(8);
    for (int i = 0; i < 8; i++) {
        uint8_t byte = b.read((static_cast<uint32_t>(c.PBR) << 16) | ((c.PC + i) & 0xFFFF));
        char buf[4]; std::snprintf(buf, sizeof(buf), "%02X", byte);
        d.bytesAtPC.emplace_back(buf);
    }
    return d;
}

void SNES::runFrame() {
    if (bus.hdmaen) bus.hdmaInit();

    int lastScanline = ppu.scanline;
    bool ppuDone = false;
    int guard = 0;

 while (!ppuDone && guard++ < 750000) {
        int cyc = cpu.step();
        int mc = cyc * ((bus.memsel & 1) ? 6 : 8);

 spcAcc += mc * 0.04768;
        while (spcAcc >= 1.0) {
            spc.step();
            bus.apuOut[0] = spc.outPorts[0];
            bus.apuOut[1] = spc.outPorts[1];
            bus.apuOut[2] = spc.outPorts[2];
            bus.apuOut[3] = spc.outPorts[3];
            spcAcc -= 1.0;
        }
        spc.genAudio(mc); // sample clock runs off real elapsed master cycles, same as spcAcc
        // bus.spcSyncRequested is diagnostic-only at this point:
        // already fully caught up to its correct ratio-derived position by
        // the loop above, every single instruction. No extra action needed —
        // clearing it here just keeps the flag meaningful for the next read.
        bus.spcSyncRequested = false;

        ppuDone = ppu.advance(mc);

        int sl = ppu.scanline;
        if (sl != lastScanline) {
            if (lastScanline < kScreenH && bus.hdmaen) bus.hdmaRun();
            bus.checkIRQ(lastScanline);
            lastScanline = sl;
        }
    }
}

} // namespace ding::snes
