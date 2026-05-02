// ── snes-core.js  ────────────────────────────────────────────────────────────
// D!NG SNES emulation core — zero DOM, no React, no Babel.
// Exposes globals: Cartridge, Bus, CPU65816, PPU, SPC700, SNES
// Used by SNESFrontend.html; will eventually pull ding-engine.js for
// achievement / leaderboard evaluation.
// ─────────────────────────────────────────────────────────────────────────────

const W = 256, H = 224, SCALE = 2;
const SCANLINES = 262, DOTS_PER_LINE = 341;
// NTSC master clock = 21.477272 MHz; 1 scanline = 1364 master clocks.
// Fast-ROM CPU cycle = 6 master clocks; Slow-ROM = 8.
// HBlank starts at master-clock 1096 within the scanline (≈ dot 274).
const LINE_MC = 1364, HBLANK_MC = 1096;

// ── CARTRIDGE ────────────────────────────────────────────────────────────────
// Supports LoROM, HiROM, ExHiROM (map mode $25). Recognises SA-1 / SDD-1 but
// maps them as plain HiROM for now — they'll need co-proc stubs later.
class Cartridge {
  constructor(data) {
    // Strip 512-byte copier header when present (size mod 1024 == 512)
    const hasHeader = (data.length & 0x3FF) === 0x200;
    this.rom = new Uint8Array(hasHeader ? data.slice(0x200) : data);

    // Score all three candidate header locations
    const loScore  = this._headerScore(0x7FC0);   // LoROM
    const hiScore  = this._headerScore(0xFFC0);   // HiROM
    const exScore  = this._headerScore(0x40FFC0); // ExHiROM (>32 Mbit)

    const best = Math.max(loScore, hiScore, exScore);
    this.exHiROM = (exScore === best && exScore > 0);
    this.hiROM   = !this.exHiROM && (hiScore >= loScore);

    const base = this.exHiROM ? 0x40FFC0 : this.hiROM ? 0xFFC0 : 0x7FC0;

    this.title    = String.fromCharCode(...this.rom.slice(base, base+21))
                      .replace(/[^\x20-\x7E]/g,'').trim() || 'Unknown ROM';
    this.mapMode  = this.rom[base+0x15];
    this.cartType = this.rom[base+0x16];
    this.romSize  = this.rom[base+0x17];
    this.ramSize  = this.rom[base+0x18];
    this.country  = this.rom[base+0x19];
    this.devId    = this.rom[base+0x1A];
    this.version  = this.rom[base+0x1B];

    // Human-readable mapper label
    this.mapLabel = this.exHiROM ? 'ExHiROM'
                  : this.hiROM   ? 'HiROM'
                  :                'LoROM';

    // Detect co-processors by cartType upper nibble
    const chipUpper = (this.cartType >> 4) & 0xF;
    if (chipUpper === 0x1 && this.devId === 0x33) this.mapLabel += '+SA-1';
    if (chipUpper === 0x4)                         this.mapLabel += '+SDD-1';
    if (chipUpper === 0x2)                         this.mapLabel += '+DSP';

    // SRAM — clamp to power-of-2 KB; min 0, max 512 KB
    const sramKB = this.ramSize > 0 ? Math.min(1 << this.ramSize, 512) : 0;
    this.sram = new Uint8Array(sramKB * 1024);
  }

  // Score a candidate header at a ROM file offset.
  // Higher is more likely to be the real header.
  _headerScore(offset) {
    if (offset + 0x40 > this.rom.length) return -100;
    let score = 0;

    // Checksum complement XOR checksum must equal 0xFFFF — strongest signal
    const comp = (this.rom[offset+0x1D]<<8) | this.rom[offset+0x1C];
    const csum = (this.rom[offset+0x1F]<<8) | this.rom[offset+0x1E];
    if ((comp ^ csum) === 0xFFFF) score += 6;

    // Map mode byte should be a known value
    const mm = this.rom[offset+0x15];
    if ([0x20,0x21,0x22,0x23,0x25,0x30,0x31,0x35,0x3A].includes(mm)) score += 3;

    // Reset vector should point into ROM space
    const reset = (this.rom[offset+0x3D]<<8) | this.rom[offset+0x3C];
    if (reset >= 0x8000) score += 2;

    // ROM size byte should be plausible (64KB–64MB = codes 6–12)
    const sz = this.rom[offset+0x17];
    if (sz >= 6 && sz <= 12) score += 1;

    // Most title characters should be printable ASCII or null padding
    let printable = 0;
    for (let i = 0; i < 21; i++) {
      const c = this.rom[offset+i];
      if ((c >= 0x20 && c <= 0x7E) || c === 0x00) printable++;
    }
    if (printable >= 15) score += 2;

    return score;
  }

  // Read one byte via 24-bit SNES address (bank + addr)
  read(bank, addr) {
    const b = bank & 0xFF, a = addr & 0xFFFF;

    // ── ExHiROM ────────────────────────────────────────────────────────────
    if (this.exHiROM) {
      if ((b >= 0x40 && b <= 0x7D) || b >= 0xC0)
        return this.rom[((b & 0x3F) * 0x10000 + a) % this.rom.length] ?? 0;
      if (a >= 0x8000) {
        const offset = 0x200000 + ((b & 0x3F) * 0x8000) + (a - 0x8000);
        return this.rom[offset % this.rom.length] ?? 0;
      }
      if (this.sram.length > 0 && a >= 0x6000 && a <= 0x7FFF) {
        const off = ((b & 0x1F) * 0x2000) + (a - 0x6000);
        return this.sram[off % this.sram.length];
      }
      return 0;
    }

    // ── HiROM ──────────────────────────────────────────────────────────────
    if (this.hiROM) {
      if ((b >= 0x40 && b <= 0x7D) || (b >= 0xC0 && b <= 0xFF))
        return this.rom[((b & 0x3F) * 0x10000 + a) % this.rom.length] ?? 0;
      if (a >= 0x8000)
        return this.rom[((b & 0x3F) * 0x10000 + a) % this.rom.length] ?? 0;
      if (this.sram.length > 0 && a >= 0x6000 && a <= 0x7FFF) {
        const bankIdx = b >= 0xA0 ? b - 0xA0 : b - 0x20;
        const off = (bankIdx * 0x2000) + (a - 0x6000);
        return this.sram[off % this.sram.length];
      }
      return 0;
    }

    // ── LoROM ──────────────────────────────────────────────────────────────
    if (a >= 0x8000) {
      const off = ((b & 0x7F) * 0x8000) + (a - 0x8000);
      return this.rom[off % this.rom.length] ?? 0;
    }
    if (this.sram.length > 0 && ((b >= 0x70 && b <= 0x7D) || (b >= 0xF0 && b <= 0xFF))) {
      const bankBase = b >= 0xF0 ? b - 0xF0 : b - 0x70;
      const off = (bankBase * 0x8000) + a;
      return this.sram[off % this.sram.length];
    }
    return 0;
  }

  write(bank, addr, val) {
    const b = bank & 0xFF, a = addr & 0xFFFF;
    if (!this.sram.length) return;

    if (this.hiROM || this.exHiROM) {
      if (a >= 0x6000 && a <= 0x7FFF) {
        const bankIdx = b >= 0xA0 ? b - 0xA0 : b >= 0x20 ? b - 0x20 : 0;
        const off = (bankIdx * 0x2000) + (a - 0x6000);
        if (off < this.sram.length) this.sram[off] = val;
      }
    } else {
      if ((b >= 0x70 && b <= 0x7D) || (b >= 0xF0 && b <= 0xFF)) {
        const bankBase = b >= 0xF0 ? b - 0xF0 : b - 0x70;
        const off = (bankBase * 0x8000) + a;
        if (off < this.sram.length) this.sram[off] = val;
      }
    }
  }
}

// ── BUS ──────────────────────────────────────────────────────────────────────
// Full 24-bit address decoder.
// $00-$3F / $80-$BF  → system area (WRAM mirror, PPU, APU, CPU I/O, cart)
// $40-$7D / $C0-$FF  → cartridge ROM/SRAM
// $7E-$7F            → WRAM (128 KB)
class Bus {
  constructor(cart) {
    this.cart   = cart;
    this.wram   = new Uint8Array(0x20000); // 128 KB
    this.ppu    = null;  // wired after PPU is created
    this.cpu    = null;  // wired for NMI/IRQ signalling

    // Joypad auto-read results (written by SNES hardware each frame)
    this.joypad = [0, 0];

    // ── Internal CPU registers ($4200–$421F) ──────────────────────────────
    this.nmitimen  = 0x00;  // $4200: NMI/IRQ/auto-joypad enable
    this.wrio      = 0xFF;  // $4201: programmable I/O
    this.memsel    = 0x00;  // $420D: ROM speed select

    // NMI / IRQ flags (set by PPU timing, cleared on read)
    this.nmiFlag   = 0x00;  // $4210 bit 7: vblank NMI pending
    this.irqFlag   = 0x00;  // $4211 bit 7: H/V timer IRQ pending

    // Hardware multiply ($4202/$4203 → $4216/$4217)
    this.wrmpya    = 0xFF;
    this.wrmpyb    = 0xFF;
    this.mpyResult = 0x0000;

    // Hardware divide ($4204-$4206 → $4214/$4215)
    this.wrdivl    = 0xFF;
    this.wrdivh    = 0xFF;
    this.wrdivb    = 0xFF;
    this.divResult = 0x0000;
    this.modResult = 0x0000;

    // H/V timer ($4207-$420A)
    this.htime     = 0x01FF;
    this.vtime     = 0x01FF;

    // WRAM port ($2180–$2183)
    this.wmaddr    = 0;      // 17-bit WRAM address

    // DMA channels (8 × general-purpose)
    this.mdmaen    = 0;
    this.hdmaen    = 0;
    this.dmaChannels = Array.from({length:8}, () => ({
      ctrl:0, destReg:0, srcAddr:0, srcBank:0,
      size:0, tableAddr:0, tableBank:0, lineCounter:0,
      indirectAddr:0, hdmaFinished:false,
    }));

    // APU I/O ports $2140–$2143 — wired to real SPC700 instance.
    this.spc    = null;
    this.apuOut = new Uint8Array(4);
    this.apuIn  = new Uint8Array(4);
    // Ring buffer of last 128 APU port accesses for diagnostics
    this.apuLog   = [];
    this._apuLogMax = 128;

    // Open-bus: last value put on the data bus (returned for unmapped reads)
    this.openBus = 0;

    // Serial joypad latch ($4016 write strobe, $4016/$4017 serial reads)
    this._joyStrobe = false;
    this._joyBit1   = 0;
    this._joyBit2   = 0;
  }

  // ── Main read entry-point ─────────────────────────────────────────────────
  read(addr24) {
    const bank = (addr24 >> 16) & 0xFF;
    const addr =  addr24        & 0xFFFF;
    const b    = bank & 0x7F;

    let v;

    if (bank === 0x7E || bank === 0x7F) {
      v = this.wram[((bank - 0x7E) << 16) | addr];
    }
    else if (b <= 0x3F) {
      if      (addr <= 0x1FFF)               v = this.wram[addr & 0x1FFF];
      else if (addr <= 0x20FF)               v = this.openBus;
      else if (addr <= 0x213F)               v = this.ppu?.regRead(addr) ?? this.openBus;
      else if (addr <= 0x2143) {
        const n = addr - 0x2140;
        if (this.spc) {
          const last = this.apuLog[this.apuLog.length-1];
          if (last && last.dir==='R' && last.port===n) {
            const before = this.spc.outPorts[n];
            for (let i=0; i<512 && this.spc.outPorts[n]===before; i++) {
              this.spc.step();
            }
          }
        }
        const v2 = this.spc ? this.spc.outPorts[n] : 0;
        const last = this.apuLog[this.apuLog.length-1];
        if (!last || last.dir!=='R' || last.port!==n || last.val!==v2 || last.a!==(this.cpu?.A??0)) {
          this.apuLog.push({dir:'R',port:n,val:v2,a:this.cpu?.A??0,rep:1});
          if (this.apuLog.length > this._apuLogMax) this.apuLog.shift();
        } else {
          last.rep = (last.rep||1) + 1;
        }
        v = v2;
      }
      else if (addr === 0x2180)              v = this._wramPortRead();
      else if (addr <= 0x21FF)               v = this.openBus;
      else if (addr >= 0x2200 && addr <= 0x3FFF) v = this.openBus;
      else if (addr === 0x4016) {
        if (this._joyStrobe) { v = 1; }
        else { const bit = this._joyBit1; this._joyBit1 = Math.min(bit+1,16); v = bit >= 16 ? 1 : (this.joypad[0] >> (15-bit)) & 1; }
      }
      else if (addr === 0x4017) {
        if (this._joyStrobe) { v = 1; }
        else { const bit = this._joyBit2; this._joyBit2 = Math.min(bit+1,16); v = bit >= 16 ? 1 : (this.joypad[1] >> (15-bit)) & 1; }
      }
      else if (addr >= 0x4000 && addr <= 0x41FF) v = this.openBus;
      else if (addr >= 0x4200 && addr <= 0x44FF) v = this._ri(addr);
      else if (addr >= 0x4500 && addr <= 0x5FFF) v = this.openBus;
      else                                    v = this.cart.read(bank, addr);
    }
    else {
      v = this.cart.read(bank, addr);
    }

    this.openBus = v & 0xFF;
    return this.openBus;
  }

  // ── Main write entry-point ────────────────────────────────────────────────
  write(addr24, val) {
    const bank = (addr24 >> 16) & 0xFF;
    const addr =  addr24        & 0xFFFF;
    const b    = bank & 0x7F;
    val &= 0xFF;

    if (bank === 0x7E || bank === 0x7F) {
      this.wram[((bank - 0x7E) << 16) | addr] = val; return;
    }
    if (b <= 0x3F) {
      if      (addr <= 0x1FFF)  { this.wram[addr & 0x1FFF] = val; return; }
      else if (addr <= 0x213F)  { this.ppu?.regWrite(addr, val); return; }
      else if (addr === 0x2180) { this._wramPortWrite(val); return; }
      else if (addr === 0x2181) { this.wmaddr = (this.wmaddr & 0x1FF00) | val; return; }
      else if (addr === 0x2182) { this.wmaddr = (this.wmaddr & 0x100FF) | (val << 8); return; }
      else if (addr === 0x2183) { this.wmaddr = (this.wmaddr & 0x0FFFF) | ((val & 1) << 16); return; }
      else if (addr >= 0x2140 && addr <= 0x2143) {
        const n = addr - 0x2140;
        this.apuIn[n] = val;
        if (this.spc) this.spc.inPorts[n] = val;
        this.apuLog.push({dir:'W',port:n,val,a:this.cpu?.A??0});
        if (this.apuLog.length > this._apuLogMax) this.apuLog.shift();
        return;
      }
      else if (addr >= 0x2144 && addr <= 0x217F) return;
      else if (addr === 0x4016) {
        if (val & 1) { this._joyStrobe = true;  this._joyBit1 = 0; this._joyBit2 = 0; }
        else         { this._joyStrobe = false; }
        return;
      }
      else if (addr >= 0x4200 && addr <= 0x44FF) { this._wi(addr, val); return; }
      else { this.cart.write(bank, addr, val); return; }
    }
    this.cart.write(bank, addr, val);
  }

  // ── WRAM port ($2180) ─────────────────────────────────────────────────────
  _wramPortRead() {
    const v = this.wram[this.wmaddr & 0x1FFFF];
    this.wmaddr = (this.wmaddr + 1) & 0x1FFFF;
    return v;
  }
  _wramPortWrite(val) {
    this.wram[this.wmaddr & 0x1FFFF] = val;
    this.wmaddr = (this.wmaddr + 1) & 0x1FFFF;
  }

  // ── Internal register reads ($4200–$44FF) ─────────────────────────────────
  _ri(addr) {
    switch (addr) {
      case 0x4210: {
        const v = this.nmiFlag | 0x02;
        this.nmiFlag = 0;
        return v;
      }
      case 0x4211: {
        const v = this.irqFlag;
        this.irqFlag = 0;
        return v;
      }
      case 0x4212: {
        const vbl = this.ppu?.vblank ? 0x80 : 0;
        const hbl = this.ppu?.hblank ? 0x40 : 0;
        return vbl | hbl | 0x01;
      }
      case 0x4213: return this.wrio;
      case 0x4214: return  this.divResult        & 0xFF;
      case 0x4215: return (this.divResult >> 8)  & 0xFF;
      case 0x4216: return  this.mpyResult        & 0xFF;
      case 0x4217: return (this.mpyResult >> 8)  & 0xFF;
      case 0x4218: return  this.joypad[0]        & 0xFF;
      case 0x4219: return (this.joypad[0] >> 8)  & 0xFF;
      case 0x421A: return  this.joypad[1]        & 0xFF;
      case 0x421B: return (this.joypad[1] >> 8)  & 0xFF;
      case 0x421C: case 0x421D: case 0x421E: case 0x421F: return 0;
      default: {
        if (addr >= 0x4300 && addr <= 0x437F) {
          const ch = (addr >> 4) & 7, reg = addr & 0xF, c = this.dmaChannels[ch];
          if (reg === 0) return c.ctrl;
          if (reg === 1) return c.destReg;
          if (reg === 2) return  c.srcAddr       & 0xFF;
          if (reg === 3) return (c.srcAddr >> 8)  & 0xFF;
          if (reg === 4) return  c.srcBank;
          if (reg === 5) return  c.size           & 0xFF;
          if (reg === 6) return (c.size >> 8)     & 0xFF;
          if (reg === 7) return  c.tableAddr      & 0xFF;
          if (reg === 8) return (c.tableAddr >> 8) & 0xFF;
          if (reg === 9) return  c.tableBank;
          if (reg === 0xA) return c.lineCounter;
        }
        return this.openBus;
      }
    }
  }

  // ── Internal register writes ($4200–$44FF) ────────────────────────────────
  _wi(addr, val) {
    switch (addr) {
      case 0x4200: this.nmitimen = val; return;
      case 0x4201: this.wrio     = val; return;
      case 0x4202: this.wrmpya = val; return;
      case 0x4203:
        this.wrmpyb    = val;
        this.mpyResult = (this.wrmpya * this.wrmpyb) & 0xFFFF;
        this.divResult = this.wrmpyb;
        return;
      case 0x4204: this.wrdivl = val; return;
      case 0x4205: this.wrdivh = val; return;
      case 0x4206: {
        this.wrdivb = val;
        const dividend = (this.wrdivh << 8) | this.wrdivl;
        if (val === 0) {
          this.divResult = 0xFFFF;
          this.modResult = dividend;
        } else {
          this.divResult = (dividend / val) >>> 0;
          this.modResult = dividend % val;
        }
        this.mpyResult = this.modResult;
        return;
      }
      case 0x4207: this.htime = (this.htime & 0x100) | val; return;
      case 0x4208: this.htime = (this.htime & 0x0FF) | ((val & 1) << 8); return;
      case 0x4209: this.vtime = (this.vtime & 0x100) | val; return;
      case 0x420A: this.vtime = (this.vtime & 0x0FF) | ((val & 1) << 8); return;
      case 0x420B: this.mdmaen = val; this._runDMA(); return;
      case 0x420C: this.hdmaen = val; return;
      case 0x420D: this.memsel = val; return;
      default: {
        if (addr >= 0x4300 && addr <= 0x437F) {
          const ch = (addr >> 4) & 7, reg = addr & 0xF, c = this.dmaChannels[ch];
          if (reg === 0) c.ctrl      = val;
          if (reg === 1) c.destReg   = val;
          if (reg === 2) c.srcAddr   = (c.srcAddr   & 0xFF00) | val;
          if (reg === 3) c.srcAddr   = (c.srcAddr   & 0x00FF) | (val << 8);
          if (reg === 4) c.srcBank   = val;
          if (reg === 5) c.size      = (c.size      & 0xFF00) | val;
          if (reg === 6) c.size      = (c.size      & 0x00FF) | (val << 8);
          if (reg === 7) c.tableAddr = (c.tableAddr & 0xFF00) | val;
          if (reg === 8) c.tableAddr = (c.tableAddr & 0x00FF) | (val << 8);
          if (reg === 9) c.tableBank = val;
          if (reg === 0xA) c.lineCounter = val;
        }
      }
    }
  }

  // ── DMA execution ($420B write triggers this) ─────────────────────────────
  _runDMA() {
    const PATTERNS = [
      [0],[0,1],[0,0],[0,0,1,1],[0,1,2,3],[0,1],[0,0],[0,0,1,1],
    ];
    for (let ch = 0; ch < 8; ch++) {
      if (!(this.mdmaen & (1 << ch))) continue;
      const c       = this.dmaChannels[ch];
      const pattern = PATTERNS[c.ctrl & 0x7];
      const toWRAM  = !!(c.ctrl & 0x80);
      const fixed   = !!(c.ctrl & 0x08);
      const decr    = !!(c.ctrl & 0x10);

      let src   = (c.srcBank << 16) | c.srcAddr;
      let count = c.size === 0 ? 0x10000 : c.size;
      let pi    = 0;

      while (count-- > 0) {
        const destAddr = 0x2100 | c.destReg | pattern[pi % pattern.length];
        pi++;
        if (toWRAM) {
          this.write(src, this.read(destAddr));
        } else {
          this.write(destAddr, this.read(src));
        }
        if (!fixed) {
          if (decr) src = (src & 0xFF0000) | ((src - 1) & 0xFFFF);
          else      src = (src & 0xFF0000) | ((src + 1) & 0xFFFF);
        }
      }
    }
    this.mdmaen = 0;
  }

  // Called by PPU at start of VBlank to raise NMI if enabled
  triggerNMI() {
    this.nmiFlag = 0x80;
    if (this.nmitimen & 0x80) { if (this.cpu) this.cpu.pendingNMI = true; }
  }

  // ── HDMA init — called once per frame at scanline 0 ──────────────────────
  hdmaInit() {
    for (let ch = 0; ch < 8; ch++) {
      if (!(this.hdmaen & (1 << ch))) continue;
      const c = this.dmaChannels[ch];
      c.tableAddr    = c.srcAddr;
      c.tableBank    = c.srcBank;
      c.hdmaFinished = false;
      c.lineCounter = this.read((c.tableBank << 16) | c.tableAddr);
      c.tableAddr   = (c.tableAddr + 1) & 0xFFFF;
      if (c.lineCounter === 0) { c.hdmaFinished = true; continue; }
      if (c.ctrl & 0x40) {
        const lo = this.read((c.tableBank << 16) | c.tableAddr);
        c.tableAddr = (c.tableAddr + 1) & 0xFFFF;
        const hi = this.read((c.tableBank << 16) | c.tableAddr);
        c.tableAddr = (c.tableAddr + 1) & 0xFFFF;
        c.indirectAddr = (hi << 8) | lo;
      }
    }
  }

  // ── HDMA per-scanline ─────────────────────────────────────────────────────
  hdmaRun() {
    const PATTERNS = [[0],[0,1],[0,0],[0,0,1,1],[0,1,2,3],[0,1],[0,0],[0,0,1,1]];
    for (let ch = 0; ch < 8; ch++) {
      if (!(this.hdmaen & (1 << ch))) continue;
      const c = this.dmaChannels[ch];
      if (c.hdmaFinished) continue;

      const pattern  = PATTERNS[c.ctrl & 7];
      const indirect = !!(c.ctrl & 0x40);
      for (let i = 0; i < pattern.length; i++) {
        const dest = (0x2100 | c.destReg | pattern[i]) & 0xFFFF;
        let byte;
        if (indirect) {
          byte = this.read((c.tableBank << 16) | (c.indirectAddr & 0xFFFF));
          c.indirectAddr = (c.indirectAddr + 1) & 0xFFFF;
        } else {
          byte = this.read((c.tableBank << 16) | c.tableAddr);
          c.tableAddr = (c.tableAddr + 1) & 0xFFFF;
        }
        this.write(dest, byte);
      }

      const lo7 = (c.lineCounter & 0x7F) - 1;
      if (lo7 > 0) {
        c.lineCounter = (c.lineCounter & 0x80) | lo7;
      } else {
        c.lineCounter = this.read((c.tableBank << 16) | c.tableAddr);
        c.tableAddr   = (c.tableAddr + 1) & 0xFFFF;
        if (c.lineCounter === 0) { c.hdmaFinished = true; continue; }
        if (indirect) {
          const lo = this.read((c.tableBank << 16) | c.tableAddr);
          c.tableAddr = (c.tableAddr + 1) & 0xFFFF;
          const hi = this.read((c.tableBank << 16) | c.tableAddr);
          c.tableAddr = (c.tableAddr + 1) & 0xFFFF;
          c.indirectAddr = (hi << 8) | lo;
        }
      }
    }
  }

  // ── H/V timer IRQ ─────────────────────────────────────────────────────────
  checkIRQ(scanline) {
    const mode = (this.nmitimen >> 4) & 3;
    if (!mode) return;
    let fire = false;
    if      (mode === 1) fire = true;
    else if (mode === 2) fire = (scanline === this.vtime);
    else if (mode === 3) fire = (scanline === this.vtime);
    if (fire) {
      this.irqFlag = 0x80;
      if (this.cpu) this.cpu.pendingIRQ = true;
    }
  }
}

// ── CPU  (WDC 65C816 / Ricoh 5A22) ───────────────────────────────────────────
class CPU65816 {
  constructor(bus) {
    this.bus = bus;
    this.PC=0; this.PBR=0; this.DBR=0;
    this.SP=0x01FF; this.DP=0;
    this.A=0; this.X=0; this.Y=0;
    this.P=0x34;
    this.E=true;
    this.pendingNMI=false; this.pendingIRQ=false;
    this.stopped=false; this.waiting=false;
    this.cycles=0;
    this.pcTrace=[]; this._lastTracedPC=-1;
    this.pcHistory=[]; this._lastTracePc=-1;
  }

  get fN(){ return !!(this.P&0x80); }
  get fV(){ return !!(this.P&0x40); }
  get fM(){ return this.E||!!(this.P&0x20); }
  get fX(){ return this.E||!!(this.P&0x10); }
  get fD(){ return !!(this.P&0x08); }
  get fI(){ return !!(this.P&0x04); }
  get fZ(){ return !!(this.P&0x02); }
  get fC(){ return !!(this.P&0x01); }
  sN(v){ v?this.P|=0x80:this.P&=~0x80; }
  sV(v){ v?this.P|=0x40:this.P&=~0x40; }
  sM(v){ v?this.P|=0x20:this.P&=~0x20; }
  sX(v){ v?this.P|=0x10:this.P&=~0x10; }
  sD(v){ v?this.P|=0x08:this.P&=~0x08; }
  sI(v){ v?this.P|=0x04:this.P&=~0x04; }
  sZ(v){ v?this.P|=0x02:this.P&=~0x02; }
  sC(v){ v?this.P|=0x01:this.P&=~0x01; }

  nzM(v){ this.sN(!!(v&(this.fM?0x80:0x8000))); this.sZ((v&(this.fM?0xFF:0xFFFF))===0); }
  nzX(v){ this.sN(!!(v&(this.fX?0x80:0x8000))); this.sZ((v&(this.fX?0xFF:0xFFFF))===0); }

  rd(a24)       { return this.bus.read(a24&0xFFFFFF); }
  wr(a24,v)     { this.bus.write(a24&0xFFFFFF, v&0xFF); }
  r8(bk,ad)     { return this.rd(((bk&0xFF)<<16)|(ad&0xFFFF)); }
  r16(bk,ad)    { return this.r8(bk,ad)|(this.r8(bk,(ad+1)&0xFFFF)<<8); }
  r24(bk,ad)    { return this.r8(bk,ad)|(this.r8(bk,(ad+1)&0xFFFF)<<8)|(this.r8(bk,(ad+2)&0xFFFF)<<16); }
  w8(bk,ad,v)   { this.wr(((bk&0xFF)<<16)|(ad&0xFFFF),v); }
  w16(bk,ad,v)  { this.w8(bk,ad,v); this.w8(bk,(ad+1)&0xFFFF,v>>8); }

  f8()  { const v=this.r8(this.PBR,this.PC); this.PC=(this.PC+1)&0xFFFF; return v; }
  f16() { const lo=this.f8(),hi=this.f8(); return (hi<<8)|lo; }
  f24() { const lo=this.f8(),hi=this.f8(),bk=this.f8(); return (bk<<16)|(hi<<8)|lo; }

  ph8(v)  { this.w8(0,this.SP,v); this.SP=this.E?(0x100|((this.SP-1)&0xFF)):((this.SP-1)&0xFFFF); }
  ph16(v) { this.ph8(v>>8); this.ph8(v); }
  pl8()   { this.SP=this.E?(0x100|((this.SP+1)&0xFF)):((this.SP+1)&0xFFFF); return this.r8(0,this.SP); }
  pl16()  { const lo=this.pl8(),hi=this.pl8(); return (hi<<8)|lo; }

  rdM(a){ if(this.fM) return this.rd(a)&0xFF; return this.rd(a)|((this.rd((a&0xFF0000)|((a+1)&0xFFFF)))<<8); }
  wrM(a,v){ this.wr(a,v); if(!this.fM) this.wr((a&0xFF0000)|((a+1)&0xFFFF),v>>8); }
  rdX(a){ if(this.fX) return this.rd(a)&0xFF; return this.rd(a)|((this.rd((a&0xFF0000)|((a+1)&0xFFFF)))<<8); }
  wrX(a,v){ this.wr(a,v); if(!this.fX) this.wr((a&0xFF0000)|((a+1)&0xFFFF),v>>8); }

  amDP()  { const d=this.f8();   return (this.DP+d)&0xFFFF; }
  amDPX() { const d=this.f8();   return (this.DP+d+this.X)&0xFFFF; }
  amDPY() { const d=this.f8();   return (this.DP+d+this.Y)&0xFFFF; }
  amAbs() { return (this.DBR<<16)|this.f16(); }
  amAbX() { return ((this.DBR<<16)|((this.f16()+this.X)&0xFFFF)); }
  amAbY() { return ((this.DBR<<16)|((this.f16()+this.Y)&0xFFFF)); }
  amLng() { return this.f24(); }
  amLnX() { return (this.f24()+this.X)&0xFFFFFF; }
  amSR()  { const o=this.f8(); return (this.SP+o)&0xFFFF; }
  amDPI()  { const ea=this.amDP(); return (this.DBR<<16)|(this.r16(0,ea)); }
  amDPIX() { const d=this.f8(), ptr=(this.DP+d+this.X)&0xFFFF; return (this.DBR<<16)|this.r16(0,ptr); }
  amDPIY() { const ea=this.amDP(); return ((this.DBR<<16)|((this.r16(0,ea)+this.Y)&0xFFFF)); }
  amDPIL() { const ea=this.amDP(); return this.r24(0,ea); }
  amDPILY(){ const ea=this.amDP(); return (this.r24(0,ea)+this.Y)&0xFFFFFF; }
  amSRIY() { const ptr=(this.SP+this.f8())&0xFFFF; return ((this.DBR<<16)|((this.r16(0,ptr)+this.Y)&0xFFFF)); }
  amImmM() { const a=(this.PBR<<16)|this.PC; this.PC=(this.PC+(this.fM?1:2))&0xFFFF; return a; }
  amImmX() { const a=(this.PBR<<16)|this.PC; this.PC=(this.PC+(this.fX?1:2))&0xFFFF; return a; }

  _adc(val) {
    if (this.fM) {
      const a=this.A&0xFF, r=a+(val&0xFF)+(this.fC?1:0);
      this.sV(!!(~(a^val)&(a^r)&0x80)); this.sC(r>0xFF);
      this.A=(this.A&0xFF00)|(r&0xFF); this.nzM(this.A);
    } else {
      const a=this.A&0xFFFF, r=a+(val&0xFFFF)+(this.fC?1:0);
      this.sV(!!(~(a^val)&(a^r)&0x8000)); this.sC(r>0xFFFF);
      this.A=r&0xFFFF; this.nzM(this.A);
    }
  }
  _sbc(val) {
    if (this.fM) {
      const a=this.A&0xFF, v=val&0xFF, r=a-v-(this.fC?0:1);
      this.sV(!!((a^v)&(a^r)&0x80)); this.sC(r>=0);
      this.A=(this.A&0xFF00)|(r&0xFF); this.nzM(this.A);
    } else {
      const a=this.A&0xFFFF, v=val&0xFFFF, r=a-v-(this.fC?0:1);
      this.sV(!!((a^v)&(a^r)&0x8000)); this.sC(r>=0);
      this.A=r&0xFFFF; this.nzM(this.A);
    }
  }
  _cmpM(a,b) {
    if(this.fM){const r=(a&0xFF)-(b&0xFF); this.sN(!!(r&0x80));this.sZ((r&0xFF)===0);this.sC(r>=0);}
    else{const r=(a&0xFFFF)-(b&0xFFFF); this.sN(!!(r&0x8000));this.sZ((r&0xFFFF)===0);this.sC(r>=0);}
  }
  _cmpX(a,b) {
    if(this.fX){const r=(a&0xFF)-(b&0xFF); this.sN(!!(r&0x80));this.sZ((r&0xFF)===0);this.sC(r>=0);}
    else{const r=(a&0xFFFF)-(b&0xFFFF); this.sN(!!(r&0x8000));this.sZ((r&0xFFFF)===0);this.sC(r>=0);}
  }
  _asl(ea) {
    if(ea===null){if(this.fM){this.sC(!!(this.A&0x80));const v=(this.A<<1)&0xFF;this.A=(this.A&0xFF00)|v;this.nzM(v);}else{this.sC(!!(this.A&0x8000));this.A=(this.A<<1)&0xFFFF;this.nzM(this.A);}}
    else{const o=this.rdM(ea);if(this.fM){this.sC(!!(o&0x80));const v=(o<<1)&0xFF;this.wrM(ea,v);this.nzM(v);}else{this.sC(!!(o&0x8000));const v=(o<<1)&0xFFFF;this.wrM(ea,v);this.nzM(v);}}
  }
  _lsr(ea) {
    if(ea===null){if(this.fM){this.sC(!!(this.A&1));const v=(this.A&0xFF)>>1;this.A=(this.A&0xFF00)|v;this.nzM(v);}else{this.sC(!!(this.A&1));this.A>>=1;this.nzM(this.A);}}
    else{const o=this.rdM(ea);if(this.fM){this.sC(!!(o&1));const v=(o&0xFF)>>1;this.wrM(ea,v);this.nzM(v);}else{this.sC(!!(o&1));const v=(o&0xFFFF)>>1;this.wrM(ea,v);this.nzM(v);}}
  }
  _rol(ea) {
    const c=this.fC?1:0;
    if(ea===null){if(this.fM){this.sC(!!(this.A&0x80));const v=((this.A<<1)|c)&0xFF;this.A=(this.A&0xFF00)|v;this.nzM(v);}else{this.sC(!!(this.A&0x8000));this.A=((this.A<<1)|c)&0xFFFF;this.nzM(this.A);}}
    else{const o=this.rdM(ea);if(this.fM){this.sC(!!(o&0x80));const v=((o<<1)|c)&0xFF;this.wrM(ea,v);this.nzM(v);}else{this.sC(!!(o&0x8000));const v=((o<<1)|c)&0xFFFF;this.wrM(ea,v);this.nzM(v);}}
  }
  _ror(ea) {
    const c=this.fC?1:0;
    if(ea===null){if(this.fM){this.sC(!!(this.A&1));const v=((c<<7)|(this.A>>1))&0xFF;this.A=(this.A&0xFF00)|v;this.nzM(v);}else{this.sC(!!(this.A&1));this.A=((c<<15)|(this.A>>1))&0xFFFF;this.nzM(this.A);}}
    else{const o=this.rdM(ea);if(this.fM){this.sC(!!(o&1));const v=((c<<7)|((o&0xFF)>>1))&0xFF;this.wrM(ea,v);this.nzM(v);}else{this.sC(!!(o&1));const v=((c<<15)|((o&0xFFFF)>>1))&0xFFFF;this.wrM(ea,v);this.nzM(v);}}
  }
  _andA(v) { if(this.fM){this.A=(this.A&0xFF00)|((this.A&v)&0xFF);}else{this.A=(this.A&v)&0xFFFF;} this.nzM(this.A); }
  _oraA(v) { if(this.fM){this.A=(this.A&0xFF00)|((this.A|v)&0xFF);}else{this.A=(this.A|v)&0xFFFF;} this.nzM(this.A); }
  _eorA(v) { if(this.fM){this.A=(this.A&0xFF00)|((this.A^v)&0xFF);}else{this.A=(this.A^v)&0xFFFF;} this.nzM(this.A); }
  _ldA(v)  { if(this.fM){this.A=(this.A&0xFF00)|(v&0xFF);}else{this.A=v&0xFFFF;} this.nzM(this.A); }

  _branch(t) { const o=this.f8(); if(!t)return 2; this.PC=(this.PC+(o<0x80?o:o-0x100))&0xFFFF; return 3; }

  _doNMI() {
    if(!this.E) this.ph8(this.PBR);
    this.ph16(this.PC); this.ph8(this.P&(this.E?0xEF:0xFF));
    this.sI(true); this.PBR=0;
    this.PC=this.r16(0,this.E?0xFFFA:0xFFEA);
    this.pendingNMI=false; this.waiting=false;
  }
  _doIRQ() {
    if(!this.E) this.ph8(this.PBR);
    this.ph16(this.PC); this.ph8(this.P&(this.E?0xEF:0xFF));
    this.sI(true); this.PBR=0;
    this.PC=this.r16(0,this.E?0xFFFE:0xFFEE);
    this.pendingIRQ=false; this.waiting=false;
  }

  reset() {
    this.E=true; this.P=0x34; this.SP=0x01FF; this.DP=this.DBR=this.PBR=0;
    this.stopped=false; this.waiting=false; this.cycles=0;
    this.PC=this.r16(0,0xFFFC);
    this.pcTrace=[]; this._lastTracedPC=-1;
  }

  step() {
    if(this.stopped) return 2;
    if(this.waiting){
      const nmiSignal = !!(this.bus.nmiFlag & 0x80);
      if(!this.pendingNMI && !this.pendingIRQ && !nmiSignal) return 2;
      this.waiting=false;
    }
    if(this.pendingNMI){ this._doNMI(); return 8; }
    if(this.pendingIRQ&&!this.fI){ this._doIRQ(); return 8; }

    const op=this.f8();
    const traceKey=(this.PBR<<16)|(this.PC-1)&0xFFFF;
    if(traceKey!==this._lastTracedPC){
      this._lastTracedPC=traceKey;
      this.pcTrace.push({bank:this.PBR,pc:(this.PC-1)&0xFFFF,op});
      if(this.pcTrace.length>24) this.pcTrace.shift();
    }
    let cy=2;

    switch(op){
      case 0xA9:{this._ldA(this.rdM(this.amImmM()));cy=this.fM?2:3;break;}
      case 0xA5:{this._ldA(this.rdM(this.amDP()));cy=3;break;}
      case 0xB5:{this._ldA(this.rdM(this.amDPX()));cy=4;break;}
      case 0xAD:{this._ldA(this.rdM(this.amAbs()));cy=4;break;}
      case 0xBD:{this._ldA(this.rdM(this.amAbX()));cy=4;break;}
      case 0xB9:{this._ldA(this.rdM(this.amAbY()));cy=4;break;}
      case 0xAF:{this._ldA(this.rdM(this.amLng()));cy=5;break;}
      case 0xBF:{this._ldA(this.rdM(this.amLnX()));cy=5;break;}
      case 0xA1:{this._ldA(this.rdM(this.amDPIX()));cy=6;break;}
      case 0xB1:{this._ldA(this.rdM(this.amDPIY()));cy=5;break;}
      case 0xB2:{this._ldA(this.rdM(this.amDPI()));cy=5;break;}
      case 0xA7:{this._ldA(this.rdM(this.amDPIL()));cy=6;break;}
      case 0xB7:{this._ldA(this.rdM(this.amDPILY()));cy=6;break;}
      case 0xA3:{this._ldA(this.rdM(this.amSR()));cy=4;break;}
      case 0xB3:{this._ldA(this.rdM(this.amSRIY()));cy=7;break;}
      case 0xA2:{this.X=this.rdX(this.amImmX());this.nzX(this.X);cy=this.fX?2:3;break;}
      case 0xA6:{this.X=this.rdX(this.amDP());this.nzX(this.X);cy=3;break;}
      case 0xB6:{this.X=this.rdX(this.amDPY());this.nzX(this.X);cy=4;break;}
      case 0xAE:{this.X=this.rdX(this.amAbs());this.nzX(this.X);cy=4;break;}
      case 0xBE:{this.X=this.rdX(this.amAbY());this.nzX(this.X);cy=4;break;}
      case 0xA0:{this.Y=this.rdX(this.amImmX());this.nzX(this.Y);cy=this.fX?2:3;break;}
      case 0xA4:{this.Y=this.rdX(this.amDP());this.nzX(this.Y);cy=3;break;}
      case 0xB4:{this.Y=this.rdX(this.amDPX());this.nzX(this.Y);cy=4;break;}
      case 0xAC:{this.Y=this.rdX(this.amAbs());this.nzX(this.Y);cy=4;break;}
      case 0xBC:{this.Y=this.rdX(this.amAbX());this.nzX(this.Y);cy=4;break;}
      case 0x85:{this.wrM(this.amDP(),this.A);cy=3;break;}
      case 0x95:{this.wrM(this.amDPX(),this.A);cy=4;break;}
      case 0x8D:{this.wrM(this.amAbs(),this.A);cy=4;break;}
      case 0x9D:{this.wrM(this.amAbX(),this.A);cy=5;break;}
      case 0x99:{this.wrM(this.amAbY(),this.A);cy=5;break;}
      case 0x8F:{this.wrM(this.amLng(),this.A);cy=5;break;}
      case 0x9F:{this.wrM(this.amLnX(),this.A);cy=5;break;}
      case 0x81:{this.wrM(this.amDPIX(),this.A);cy=6;break;}
      case 0x91:{this.wrM(this.amDPIY(),this.A);cy=6;break;}
      case 0x92:{this.wrM(this.amDPI(),this.A);cy=5;break;}
      case 0x87:{this.wrM(this.amDPIL(),this.A);cy=6;break;}
      case 0x97:{this.wrM(this.amDPILY(),this.A);cy=6;break;}
      case 0x83:{this.wrM(this.amSR(),this.A);cy=4;break;}
      case 0x93:{this.wrM(this.amSRIY(),this.A);cy=7;break;}
      case 0x86:{this.wrX(this.amDP(),this.X);cy=3;break;}
      case 0x96:{this.wrX(this.amDPY(),this.X);cy=4;break;}
      case 0x8E:{this.wrX(this.amAbs(),this.X);cy=4;break;}
      case 0x84:{this.wrX(this.amDP(),this.Y);cy=3;break;}
      case 0x94:{this.wrX(this.amDPX(),this.Y);cy=4;break;}
      case 0x8C:{this.wrX(this.amAbs(),this.Y);cy=4;break;}
      case 0x64:{this.wrM(this.amDP(),0);cy=3;break;}
      case 0x74:{this.wrM(this.amDPX(),0);cy=4;break;}
      case 0x9C:{this.wrM(this.amAbs(),0);cy=4;break;}
      case 0x9E:{this.wrM(this.amAbX(),0);cy=5;break;}
      case 0xAA:{this.X=this.fX?this.A&0xFF:this.A&0xFFFF;this.nzX(this.X);break;}
      case 0xA8:{this.Y=this.fX?this.A&0xFF:this.A&0xFFFF;this.nzX(this.Y);break;}
      case 0x8A:{this._ldA(this.X);break;}
      case 0x98:{this._ldA(this.Y);break;}
      case 0xBA:{this.X=this.fX?this.SP&0xFF:this.SP&0xFFFF;this.nzX(this.X);break;}
      case 0x9A:{this.SP=this.E?0x100|(this.X&0xFF):this.X&0xFFFF;break;}
      case 0x9B:{this.Y=this.X;this.nzX(this.Y);break;}
      case 0xBB:{this.X=this.Y;this.nzX(this.X);break;}
      case 0x5B:{this.DP=this.A&0xFFFF;this.nzX(this.DP);break;}
      case 0x7B:{this.A=this.DP;this.nzX(this.A);break;}
      case 0x1B:{this.SP=this.E?0x100|(this.A&0xFF):this.A&0xFFFF;break;}
      case 0x3B:{this.A=this.SP;this.nzX(this.A);break;}
      case 0x48:{this.fM?this.ph8(this.A):this.ph16(this.A);cy=3;break;}
      case 0xDA:{this.fX?this.ph8(this.X):this.ph16(this.X);cy=3;break;}
      case 0x5A:{this.fX?this.ph8(this.Y):this.ph16(this.Y);cy=3;break;}
      case 0x08:{this.ph8(this.E?this.P|0x30:this.P);cy=3;break;}
      case 0x8B:{this.ph8(this.DBR);cy=3;break;}
      case 0x0B:{this.ph16(this.DP);cy=4;break;}
      case 0x4B:{this.ph8(this.PBR);cy=3;break;}
      case 0xF4:{this.ph16(this.f16());cy=5;break;}
      case 0xD4:{this.ph16(this.r16(0,this.amDP()));cy=6;break;}
      case 0x62:{const o=this.f16();this.ph16((this.PC+(o<0x8000?o:o-0x10000))&0xFFFF);cy=6;break;}
      case 0x68:{const v=this.fM?this.pl8():this.pl16();this._ldA(v);cy=4;break;}
      case 0xFA:{this.X=this.fX?this.pl8():this.pl16();this.nzX(this.X);cy=4;break;}
      case 0x7A:{this.Y=this.fX?this.pl8():this.pl16();this.nzX(this.Y);cy=4;break;}
      case 0x28:{this.P=this.pl8();if(this.E)this.P|=0x30;if(this.fX){this.X&=0xFF;this.Y&=0xFF;}cy=4;break;}
      case 0xAB:{this.DBR=this.pl8();this.nzX(this.DBR);cy=4;break;}
      case 0x2B:{this.DP=this.pl16();this.nzX(this.DP);cy=5;break;}
      case 0x69:{this._adc(this.rdM(this.amImmM()));cy=this.fM?2:3;break;}
      case 0x65:{this._adc(this.rdM(this.amDP()));cy=3;break;}
      case 0x75:{this._adc(this.rdM(this.amDPX()));cy=4;break;}
      case 0x6D:{this._adc(this.rdM(this.amAbs()));cy=4;break;}
      case 0x7D:{this._adc(this.rdM(this.amAbX()));cy=4;break;}
      case 0x79:{this._adc(this.rdM(this.amAbY()));cy=4;break;}
      case 0x6F:{this._adc(this.rdM(this.amLng()));cy=5;break;}
      case 0x7F:{this._adc(this.rdM(this.amLnX()));cy=5;break;}
      case 0x61:{this._adc(this.rdM(this.amDPIX()));cy=6;break;}
      case 0x71:{this._adc(this.rdM(this.amDPIY()));cy=5;break;}
      case 0x72:{this._adc(this.rdM(this.amDPI()));cy=5;break;}
      case 0x67:{this._adc(this.rdM(this.amDPIL()));cy=6;break;}
      case 0x77:{this._adc(this.rdM(this.amDPILY()));cy=6;break;}
      case 0x63:{this._adc(this.rdM(this.amSR()));cy=4;break;}
      case 0x73:{this._adc(this.rdM(this.amSRIY()));cy=7;break;}
      case 0xE9:{this._sbc(this.rdM(this.amImmM()));cy=this.fM?2:3;break;}
      case 0xE5:{this._sbc(this.rdM(this.amDP()));cy=3;break;}
      case 0xF5:{this._sbc(this.rdM(this.amDPX()));cy=4;break;}
      case 0xED:{this._sbc(this.rdM(this.amAbs()));cy=4;break;}
      case 0xFD:{this._sbc(this.rdM(this.amAbX()));cy=4;break;}
      case 0xF9:{this._sbc(this.rdM(this.amAbY()));cy=4;break;}
      case 0xEF:{this._sbc(this.rdM(this.amLng()));cy=5;break;}
      case 0xFF:{this._sbc(this.rdM(this.amLnX()));cy=5;break;}
      case 0xE1:{this._sbc(this.rdM(this.amDPIX()));cy=6;break;}
      case 0xF1:{this._sbc(this.rdM(this.amDPIY()));cy=5;break;}
      case 0xF2:{this._sbc(this.rdM(this.amDPI()));cy=5;break;}
      case 0xE7:{this._sbc(this.rdM(this.amDPIL()));cy=6;break;}
      case 0xF7:{this._sbc(this.rdM(this.amDPILY()));cy=6;break;}
      case 0xE3:{this._sbc(this.rdM(this.amSR()));cy=4;break;}
      case 0xF3:{this._sbc(this.rdM(this.amSRIY()));cy=7;break;}
      case 0xC9:{this._cmpM(this.A,this.rdM(this.amImmM()));cy=this.fM?2:3;break;}
      case 0xC5:{this._cmpM(this.A,this.rdM(this.amDP()));cy=3;break;}
      case 0xD5:{this._cmpM(this.A,this.rdM(this.amDPX()));cy=4;break;}
      case 0xCD:{this._cmpM(this.A,this.rdM(this.amAbs()));cy=4;break;}
      case 0xDD:{this._cmpM(this.A,this.rdM(this.amAbX()));cy=4;break;}
      case 0xD9:{this._cmpM(this.A,this.rdM(this.amAbY()));cy=4;break;}
      case 0xCF:{this._cmpM(this.A,this.rdM(this.amLng()));cy=5;break;}
      case 0xDF:{this._cmpM(this.A,this.rdM(this.amLnX()));cy=5;break;}
      case 0xC1:{this._cmpM(this.A,this.rdM(this.amDPIX()));cy=6;break;}
      case 0xD1:{this._cmpM(this.A,this.rdM(this.amDPIY()));cy=5;break;}
      case 0xD2:{this._cmpM(this.A,this.rdM(this.amDPI()));cy=5;break;}
      case 0xC7:{this._cmpM(this.A,this.rdM(this.amDPIL()));cy=6;break;}
      case 0xD7:{this._cmpM(this.A,this.rdM(this.amDPILY()));cy=6;break;}
      case 0xC3:{this._cmpM(this.A,this.rdM(this.amSR()));cy=4;break;}
      case 0xD3:{this._cmpM(this.A,this.rdM(this.amSRIY()));cy=7;break;}
      case 0xE0:{this._cmpX(this.X,this.rdX(this.amImmX()));cy=this.fX?2:3;break;}
      case 0xE4:{this._cmpX(this.X,this.rdX(this.amDP()));cy=3;break;}
      case 0xEC:{this._cmpX(this.X,this.rdX(this.amAbs()));cy=4;break;}
      case 0xC0:{this._cmpX(this.Y,this.rdX(this.amImmX()));cy=this.fX?2:3;break;}
      case 0xC4:{this._cmpX(this.Y,this.rdX(this.amDP()));cy=3;break;}
      case 0xCC:{this._cmpX(this.Y,this.rdX(this.amAbs()));cy=4;break;}
      case 0x29:{this._andA(this.rdM(this.amImmM()));cy=this.fM?2:3;break;}
      case 0x25:{this._andA(this.rdM(this.amDP()));cy=3;break;}
      case 0x35:{this._andA(this.rdM(this.amDPX()));cy=4;break;}
      case 0x2D:{this._andA(this.rdM(this.amAbs()));cy=4;break;}
      case 0x3D:{this._andA(this.rdM(this.amAbX()));cy=4;break;}
      case 0x39:{this._andA(this.rdM(this.amAbY()));cy=4;break;}
      case 0x2F:{this._andA(this.rdM(this.amLng()));cy=5;break;}
      case 0x3F:{this._andA(this.rdM(this.amLnX()));cy=5;break;}
      case 0x21:{this._andA(this.rdM(this.amDPIX()));cy=6;break;}
      case 0x31:{this._andA(this.rdM(this.amDPIY()));cy=5;break;}
      case 0x32:{this._andA(this.rdM(this.amDPI()));cy=5;break;}
      case 0x27:{this._andA(this.rdM(this.amDPIL()));cy=6;break;}
      case 0x37:{this._andA(this.rdM(this.amDPILY()));cy=6;break;}
      case 0x23:{this._andA(this.rdM(this.amSR()));cy=4;break;}
      case 0x33:{this._andA(this.rdM(this.amSRIY()));cy=7;break;}
      case 0x09:{this._oraA(this.rdM(this.amImmM()));cy=this.fM?2:3;break;}
      case 0x05:{this._oraA(this.rdM(this.amDP()));cy=3;break;}
      case 0x15:{this._oraA(this.rdM(this.amDPX()));cy=4;break;}
      case 0x0D:{this._oraA(this.rdM(this.amAbs()));cy=4;break;}
      case 0x1D:{this._oraA(this.rdM(this.amAbX()));cy=4;break;}
      case 0x19:{this._oraA(this.rdM(this.amAbY()));cy=4;break;}
      case 0x0F:{this._oraA(this.rdM(this.amLng()));cy=5;break;}
      case 0x1F:{this._oraA(this.rdM(this.amLnX()));cy=5;break;}
      case 0x01:{this._oraA(this.rdM(this.amDPIX()));cy=6;break;}
      case 0x11:{this._oraA(this.rdM(this.amDPIY()));cy=5;break;}
      case 0x12:{this._oraA(this.rdM(this.amDPI()));cy=5;break;}
      case 0x07:{this._oraA(this.rdM(this.amDPIL()));cy=6;break;}
      case 0x17:{this._oraA(this.rdM(this.amDPILY()));cy=6;break;}
      case 0x03:{this._oraA(this.rdM(this.amSR()));cy=4;break;}
      case 0x13:{this._oraA(this.rdM(this.amSRIY()));cy=7;break;}
      case 0x49:{this._eorA(this.rdM(this.amImmM()));cy=this.fM?2:3;break;}
      case 0x45:{this._eorA(this.rdM(this.amDP()));cy=3;break;}
      case 0x55:{this._eorA(this.rdM(this.amDPX()));cy=4;break;}
      case 0x4D:{this._eorA(this.rdM(this.amAbs()));cy=4;break;}
      case 0x5D:{this._eorA(this.rdM(this.amAbX()));cy=4;break;}
      case 0x59:{this._eorA(this.rdM(this.amAbY()));cy=4;break;}
      case 0x4F:{this._eorA(this.rdM(this.amLng()));cy=5;break;}
      case 0x5F:{this._eorA(this.rdM(this.amLnX()));cy=5;break;}
      case 0x41:{this._eorA(this.rdM(this.amDPIX()));cy=6;break;}
      case 0x51:{this._eorA(this.rdM(this.amDPIY()));cy=5;break;}
      case 0x52:{this._eorA(this.rdM(this.amDPI()));cy=5;break;}
      case 0x47:{this._eorA(this.rdM(this.amDPIL()));cy=6;break;}
      case 0x57:{this._eorA(this.rdM(this.amDPILY()));cy=6;break;}
      case 0x43:{this._eorA(this.rdM(this.amSR()));cy=4;break;}
      case 0x53:{this._eorA(this.rdM(this.amSRIY()));cy=7;break;}
      case 0x04:{const a=this.amDP(); const v=this.rdM(a); this.sZ((this.A&v)===0); this.wrM(a,v|(this.fM?this.A&0xFF:this.A));cy=5;break;}
      case 0x0C:{const a=this.amAbs();const v=this.rdM(a); this.sZ((this.A&v)===0); this.wrM(a,v|(this.fM?this.A&0xFF:this.A));cy=6;break;}
      case 0x14:{const a=this.amDP(); const v=this.rdM(a); this.sZ((this.A&v)===0); this.wrM(a,v&~(this.fM?this.A&0xFF:this.A)&(this.fM?0xFF:0xFFFF));cy=5;break;}
      case 0x1C:{const a=this.amAbs();const v=this.rdM(a); this.sZ((this.A&v)===0); this.wrM(a,v&~(this.fM?this.A&0xFF:this.A)&(this.fM?0xFF:0xFFFF));cy=6;break;}
      case 0x89:{const v=this.rdM(this.amImmM());this.sZ((this.A&v)===0);cy=this.fM?2:3;break;}
      case 0x24:{const v=this.rdM(this.amDP());this.sN(!!(v&(this.fM?0x80:0x8000)));this.sV(!!(v&(this.fM?0x40:0x4000)));this.sZ((this.A&v)===0);cy=3;break;}
      case 0x34:{const v=this.rdM(this.amDPX());this.sN(!!(v&(this.fM?0x80:0x8000)));this.sV(!!(v&(this.fM?0x40:0x4000)));this.sZ((this.A&v)===0);cy=4;break;}
      case 0x2C:{const v=this.rdM(this.amAbs());this.sN(!!(v&(this.fM?0x80:0x8000)));this.sV(!!(v&(this.fM?0x40:0x4000)));this.sZ((this.A&v)===0);cy=4;break;}
      case 0x3C:{const v=this.rdM(this.amAbX());this.sN(!!(v&(this.fM?0x80:0x8000)));this.sV(!!(v&(this.fM?0x40:0x4000)));this.sZ((this.A&v)===0);cy=4;break;}
      case 0x0A:{this._asl(null);cy=2;break;} case 0x06:{this._asl(this.amDP());cy=5;break;}
      case 0x16:{this._asl(this.amDPX());cy=6;break;} case 0x0E:{this._asl(this.amAbs());cy=6;break;}
      case 0x1E:{this._asl(this.amAbX());cy=7;break;}
      case 0x4A:{this._lsr(null);cy=2;break;} case 0x46:{this._lsr(this.amDP());cy=5;break;}
      case 0x56:{this._lsr(this.amDPX());cy=6;break;} case 0x4E:{this._lsr(this.amAbs());cy=6;break;}
      case 0x5E:{this._lsr(this.amAbX());cy=7;break;}
      case 0x2A:{this._rol(null);cy=2;break;} case 0x26:{this._rol(this.amDP());cy=5;break;}
      case 0x36:{this._rol(this.amDPX());cy=6;break;} case 0x2E:{this._rol(this.amAbs());cy=6;break;}
      case 0x3E:{this._rol(this.amAbX());cy=7;break;}
      case 0x6A:{this._ror(null);cy=2;break;} case 0x66:{this._ror(this.amDP());cy=5;break;}
      case 0x76:{this._ror(this.amDPX());cy=6;break;} case 0x6E:{this._ror(this.amAbs());cy=6;break;}
      case 0x7E:{this._ror(this.amAbX());cy=7;break;}
      case 0x1A:{const m=this.fM;const v=m?((this.A&0xFF)+1)&0xFF:(this.A+1)&0xFFFF;this._ldA(m?(this.A&0xFF00)|v:v);cy=2;break;}
      case 0x3A:{const m=this.fM;const v=m?((this.A&0xFF)-1)&0xFF:(this.A-1)&0xFFFF;this._ldA(m?(this.A&0xFF00)|v:v);cy=2;break;}
      case 0xE6:{const a=this.amDP();const v=(this.rdM(a)+1)&(this.fM?0xFF:0xFFFF);this.wrM(a,v);this.nzM(v);cy=5;break;}
      case 0xF6:{const a=this.amDPX();const v=(this.rdM(a)+1)&(this.fM?0xFF:0xFFFF);this.wrM(a,v);this.nzM(v);cy=6;break;}
      case 0xEE:{const a=this.amAbs();const v=(this.rdM(a)+1)&(this.fM?0xFF:0xFFFF);this.wrM(a,v);this.nzM(v);cy=6;break;}
      case 0xFE:{const a=this.amAbX();const v=(this.rdM(a)+1)&(this.fM?0xFF:0xFFFF);this.wrM(a,v);this.nzM(v);cy=7;break;}
      case 0xC6:{const a=this.amDP();const v=(this.rdM(a)-1)&(this.fM?0xFF:0xFFFF);this.wrM(a,v);this.nzM(v);cy=5;break;}
      case 0xD6:{const a=this.amDPX();const v=(this.rdM(a)-1)&(this.fM?0xFF:0xFFFF);this.wrM(a,v);this.nzM(v);cy=6;break;}
      case 0xCE:{const a=this.amAbs();const v=(this.rdM(a)-1)&(this.fM?0xFF:0xFFFF);this.wrM(a,v);this.nzM(v);cy=6;break;}
      case 0xDE:{const a=this.amAbX();const v=(this.rdM(a)-1)&(this.fM?0xFF:0xFFFF);this.wrM(a,v);this.nzM(v);cy=7;break;}
      case 0xE8:{this.X=(this.X+1)&(this.fX?0xFF:0xFFFF);this.nzX(this.X);break;}
      case 0xC8:{this.Y=(this.Y+1)&(this.fX?0xFF:0xFFFF);this.nzX(this.Y);break;}
      case 0xCA:{this.X=(this.X-1)&(this.fX?0xFF:0xFFFF);this.nzX(this.X);break;}
      case 0x88:{this.Y=(this.Y-1)&(this.fX?0xFF:0xFFFF);this.nzX(this.Y);break;}
      case 0x4C:{this.PC=this.f16();cy=3;break;}
      case 0x5C:{const a=this.f24();this.PBR=(a>>16)&0xFF;this.PC=a&0xFFFF;cy=4;break;}
      case 0x6C:{const p=this.f16();this.PC=this.r16(0,p);cy=5;break;}
      case 0x7C:{const p=(this.f16()+this.X)&0xFFFF;this.PC=this.r16(this.PBR,p);cy=6;break;}
      case 0xDC:{const p=this.f16();const a=this.r24(0,p);this.PBR=(a>>16)&0xFF;this.PC=a&0xFFFF;cy=6;break;}
      case 0x20:{const t=this.f16();this.ph16((this.PC-1)&0xFFFF);this.PC=t;cy=6;break;}
      case 0x22:{const t=this.f24();this.ph8(this.PBR);this.ph16((this.PC-1)&0xFFFF);this.PBR=(t>>16)&0xFF;this.PC=t&0xFFFF;cy=8;break;}
      case 0xFC:{const p=(this.f16()+this.X)&0xFFFF;this.ph16((this.PC-1)&0xFFFF);this.PC=this.r16(this.PBR,p);cy=8;break;}
      case 0x60:{this.PC=(this.pl16()+1)&0xFFFF;cy=6;break;}
      case 0x6B:{const pc=this.pl16();this.PBR=this.pl8();this.PC=(pc+1)&0xFFFF;cy=6;break;}
      case 0x40:{this.P=this.pl8();if(this.E)this.P|=0x30;else if(this.fX){this.X&=0xFF;this.Y&=0xFF;}this.PC=this.pl16();if(!this.E)this.PBR=this.pl8();cy=6;break;}
      case 0x00:{this.f8();if(!this.E)this.ph8(this.PBR);this.ph16(this.PC);this.ph8(this.E?this.P|0x30:this.P|0x10);this.sI(true);this.sD(false);this.PBR=0;this.PC=this.r16(0,this.E?0xFFFE:0xFFE6);cy=8;break;}
      case 0x02:{this.f8();if(!this.E)this.ph8(this.PBR);this.ph16(this.PC);this.ph8(this.E?this.P|0x30:this.P);this.sI(true);this.sD(false);this.PBR=0;this.PC=this.r16(0,this.E?0xFFF4:0xFFE4);cy=8;break;}
      case 0x90:cy=this._branch(!this.fC);break;
      case 0xB0:cy=this._branch( this.fC);break;
      case 0xF0:cy=this._branch( this.fZ);break;
      case 0x30:cy=this._branch( this.fN);break;
      case 0xD0:cy=this._branch(!this.fZ);break;
      case 0x10:cy=this._branch(!this.fN);break;
      case 0x50:cy=this._branch(!this.fV);break;
      case 0x70:cy=this._branch( this.fV);break;
      case 0x80:cy=this._branch(true);break;
      case 0x82:{const o=this.f16();this.PC=(this.PC+(o<0x8000?o:o-0x10000))&0xFFFF;cy=4;break;}
      case 0x18:this.sC(false);break;
      case 0x38:this.sC(true); break;
      case 0x58:this.sI(false);break;
      case 0x78:this.sI(true); break;
      case 0xD8:this.sD(false);break;
      case 0xF8:this.sD(true); break;
      case 0xB8:this.sV(false);break;
      case 0xC2:{const m=this.f8();this.P&=~m;if(this.E)this.P|=0x30;if(this.fX){this.X&=0xFF;this.Y&=0xFF;}cy=3;break;}
      case 0xE2:{const m=this.f8();this.P|=m;if(this.fX){this.X&=0xFF;this.Y&=0xFF;}cy=3;break;}
      case 0xFB:{const oe=this.E,oc=this.fC;this.E=oc;this.sC(oe);if(this.E){this.P|=0x30;this.SP=0x100|(this.SP&0xFF);this.X&=0xFF;this.Y&=0xFF;}cy=2;break;}
      case 0x54:{const dst=this.f8(),src=this.f8();this.DBR=dst;this.wr((dst<<16)|(this.Y&0xFFFF),this.rd((src<<16)|(this.X&0xFFFF)));this.X=(this.X+1)&(this.fX?0xFF:0xFFFF);this.Y=(this.Y+1)&(this.fX?0xFF:0xFFFF);this.A=(this.A-1)&0xFFFF;if(this.A!==0xFFFF)this.PC=(this.PC-3)&0xFFFF;cy=7;break;}
      case 0x44:{const dst=this.f8(),src=this.f8();this.DBR=dst;this.wr((dst<<16)|(this.Y&0xFFFF),this.rd((src<<16)|(this.X&0xFFFF)));this.X=(this.X-1)&(this.fX?0xFF:0xFFFF);this.Y=(this.Y-1)&(this.fX?0xFF:0xFFFF);this.A=(this.A-1)&0xFFFF;if(this.A!==0xFFFF)this.PC=(this.PC-3)&0xFFFF;cy=7;break;}
      case 0xEA:cy=2;break;
      case 0x42:this.f8();cy=2;break;
      case 0xCB:this.waiting=true;cy=3;break;
      case 0xDB:this.stopped=true;cy=3;break;
      default:cy=2;break;
    }
    const fullPc = (this.PBR << 16) | this.PC;
    if (fullPc !== this._lastTracePc) {
      this._lastTracePc = fullPc;
      this.pcHistory.push(fullPc);
      if (this.pcHistory.length > 24) this.pcHistory.shift();
    }
    this.cycles+=cy;
    return cy;
  }

  snapshot() {
    return { pc:this.PC, a:this.A, x:this.X, y:this.Y,
             sp:this.SP, dp:this.DP, pbr:this.PBR, dbr:this.DBR,
             p:this.P, e:this.E, cycles:this.cycles };
  }
}

// ── PPU (Picture Processing Unit) ────────────────────────────────────────────
class PPU {
  constructor() {
    this.vram  = new Uint8Array(0x10000);
    this.cgram = new Uint16Array(0x100);
    this.oam   = new Uint8Array(0x220);
    this.regs  = new Uint8Array(0x40);
    // Power-on: INIDISP = $8F (forced blank active, brightness=15)
    this.regs[0x00] = 0x8F;

    this.clk=0; this.scanline=0; this.vblank=false; this.hblank=false; this.frame=0;
    this.bus=null;

    this.vramAddr=0; this.vramInc=1; this.vramIncOnHi=false; this.vramRdBuf=0;
    this.cgramAddr=0; this.cgramBuf=0; this.cgramLatch=false;
    this.oamByteAddr=0; this.oamLow=0;

    this.bgH=[0,0,0,0]; this.bgV=[0,0,0,0];
    this.m7prev=0; this.bgPrev=0;

    this.m7a=0x0100; this.m7b=0; this.m7c=0; this.m7d=0x0100;
    this.m7cx=0; this.m7cy=0;

    this.fixedColor=0;

    this.pixels = new Uint32Array(W*H);
    this.pixels.fill(0xFF1A1428);
  }

  _c(c15) {
    const r=((c15&0x1F)*255/31+0.5)|0;
    const g=(((c15>>5)&0x1F)*255/31+0.5)|0;
    const b=(((c15>>10)&0x1F)*255/31+0.5)|0;
    return (0xFF000000|0)|(b<<16)|(g<<8)|r;
  }

  _pfVRAM() {
    const a=(this.vramAddr<<1)&0xFFFF;
    this.vramRdBuf=this.vram[a]|(this.vram[a+1]<<8);
  }

  regRead(addr) {
    const r=addr-0x2100;
    switch(r) {
      case 0x34: { const p=(this.m7a*(this.m7b>>8))&0xFFFFFF; return p&0xFF; }
      case 0x35: { const p=(this.m7a*(this.m7b>>8))&0xFFFFFF; return (p>>8)&0xFF; }
      case 0x36: { const p=(this.m7a*(this.m7b>>8))&0xFFFFFF; return (p>>16)&0xFF; }
      case 0x38: {
        const v=this.vramRdBuf&0xFF;
        if(!this.vramIncOnHi){this.vramAddr=(this.vramAddr+this.vramInc)&0x7FFF;this._pfVRAM();}
        return v;
      }
      case 0x39: {
        const v=(this.vramRdBuf>>8)&0xFF;
        if(this.vramIncOnHi){this.vramAddr=(this.vramAddr+this.vramInc)&0x7FFF;this._pfVRAM();}
        return v;
      }
      case 0x3B: {
        let v;
        if(!this.cgramLatch){ v=this.cgram[this.cgramAddr&0xFF]&0xFF; this.cgramLatch=true; }
        else{ v=(this.cgram[this.cgramAddr&0xFF]>>8)&0x7F; this.cgramAddr=(this.cgramAddr+1)&0xFF; this.cgramLatch=false; }
        return v;
      }
      case 0x3E: return 0x01;
      case 0x3F: return (this.vblank?0x80:0)|0x02;
      default:   return this.regs[r]??0;
    }
  }

  regWrite(addr,val) {
    const r=addr-0x2100;
    this.regs[r]=val;
    switch(r) {
      case 0x02: this.oamByteAddr=(((this.regs[0x03]&1)<<8)|val)<<1; break;
      case 0x03: this.oamByteAddr=(((val&1)<<8)|this.regs[0x02])<<1; break;
      case 0x04:
        if(this.oamByteAddr<0x200){
          if(this.oamByteAddr&1){
            this.oam[this.oamByteAddr^1]=this.oamLow;
            this.oam[this.oamByteAddr]=val;
          } else { this.oamLow=val; }
        } else if(this.oamByteAddr<0x220){ this.oam[this.oamByteAddr]=val; }
        this.oamByteAddr=(this.oamByteAddr+1)&0x3FF;
        break;
      case 0x0D: this.bgH[0]=((val<<8)|this.m7prev)&0x3FF; this.m7prev=val; break;
      case 0x0E: this.bgV[0]=((val<<8)|this.m7prev)&0x3FF; this.m7prev=val; break;
      case 0x0F: this.bgH[1]=((val<<8)|this.bgPrev)&0x3FF; this.bgPrev=val; break;
      case 0x10: this.bgV[1]=((val<<8)|this.bgPrev)&0x3FF; this.bgPrev=val; break;
      case 0x11: this.bgH[2]=((val<<8)|this.bgPrev)&0x3FF; this.bgPrev=val; break;
      case 0x12: this.bgV[2]=((val<<8)|this.bgPrev)&0x3FF; this.bgPrev=val; break;
      case 0x13: this.bgH[3]=((val<<8)|this.bgPrev)&0x3FF; this.bgPrev=val; break;
      case 0x14: this.bgV[3]=((val<<8)|this.bgPrev)&0x3FF; this.bgPrev=val; break;
      case 0x15:
        this.vramInc=[1,32,128,128][val&3];
        this.vramIncOnHi=!!(val&0x80);
        break;
      case 0x16: this.vramAddr=(this.vramAddr&0x7F00)|val; this._pfVRAM(); break;
      case 0x17: this.vramAddr=(this.vramAddr&0x00FF)|((val&0x7F)<<8); this._pfVRAM(); break;
      case 0x18:
        this.vram[(this.vramAddr<<1)&0xFFFF]=val;
        if(!this.vramIncOnHi){this.vramAddr=(this.vramAddr+this.vramInc)&0x7FFF;this._pfVRAM();}
        break;
      case 0x19:
        this.vram[((this.vramAddr<<1)+1)&0xFFFF]=val;
        if(this.vramIncOnHi){this.vramAddr=(this.vramAddr+this.vramInc)&0x7FFF;this._pfVRAM();}
        break;
      case 0x1B: this.m7a=(val<<8)|this.m7prev; this.m7prev=val; break;
      case 0x1C: this.m7b=(val<<8)|this.m7prev; this.m7prev=val; break;
      case 0x1D: this.m7c=(val<<8)|this.m7prev; this.m7prev=val; break;
      case 0x1E: this.m7d=(val<<8)|this.m7prev; this.m7prev=val; break;
      case 0x1F: this.m7cx=(val<<8)|this.m7prev; this.m7prev=val; break;
      case 0x20: this.m7cy=(val<<8)|this.m7prev; this.m7prev=val; break;
      case 0x21: this.cgramAddr=val; this.cgramLatch=false; break;
      case 0x22:
        if(!this.cgramLatch){ this.cgramBuf=val; this.cgramLatch=true; }
        else{
          this.cgram[this.cgramAddr&0xFF]=((val&0x7F)<<8)|this.cgramBuf;
          this.cgramAddr=(this.cgramAddr+1)&0xFF;
          this.cgramLatch=false;
        }
        break;
      case 0x32: {
        const i=val&0x1F;
        if(val&0x20) this.fixedColor=(this.fixedColor&~0x001F)|i;
        if(val&0x40) this.fixedColor=(this.fixedColor&~0x03E0)|(i<<5);
        if(val&0x80) this.fixedColor=(this.fixedColor&~0x7C00)|(i<<10);
        break;
      }
    }
  }

  _bgLine(bg, y, bpp) {
    const mosaic = this.regs[0x06];
    const mSize  = ((mosaic >> 4) & 0xF) + 1;
    const mEn    = !!(mosaic & (1 << bg));
    const mY = mEn && mSize > 1 ? y - (y % mSize) : y;

    const bgSC    = this.regs[0x07+bg];
    const mapBase = ((bgSC>>2)&0x3F)<<10;
    const mapSzX  = (bgSC&1)?64:32;
    const mapSzY  = (bgSC&2)?64:32;
    const nba     = this.regs[0x0B+(bg>>1)];
    const charBase= ((bg&1)?(nba>>4):(nba&0xF))<<12;
    const wpt     = bpp===2?8:bpp===4?16:32;
    const effY    = (mY+this.bgV[bg])&((mapSzY<<3)-1);
    const tileRow = effY>>3;
    const pxRow   = effY&7;
    const pgsW    = mapSzX===64?2:1;
    const mode    = this.regs[0x05]&7;

    const out=new Array(256);
    let lastTC=-1, p0=0,p1=0,p2=0,p3=0,p4=0,p5=0,p6=0,p7=0;
    let palette=0,prio=0,xflip=0;

    for(let x=0;x<256;x++){
      const mx = mEn && mSize > 1 ? x - (x % mSize) : x;
      const effX=(mx+this.bgH[bg])&((mapSzX<<3)-1);
      const tc=effX>>3;
      const pc=effX&7;

      if(tc!==lastTC){
        lastTC=tc;
        const pgX=(tc>>5)&1, pgY=(tileRow>>5)&1;
        const page=pgY*pgsW+pgX;
        const ma=mapBase+page*0x400+((tileRow&31)<<5)+(tc&31);
        const lo=this.vram[(ma<<1)&0xFFFF], hi=this.vram[((ma<<1)+1)&0xFFFF];
        const e=(hi<<8)|lo;
        const tileNo=e&0x3FF;
        palette=(e>>10)&7; prio=(e>>13)&1; xflip=(e>>14)&1;
        const yflip=(e>>15)&1;
        const row=yflip?7-pxRow:pxRow;
        const wa=(charBase+tileNo*wpt+row)&0x7FFF;
        p0=this.vram[(wa<<1)&0xFFFF]; p1=this.vram[((wa<<1)+1)&0xFFFF];
        if(bpp>=4){
          p2=this.vram[((wa+8)<<1)&0xFFFF]; p3=this.vram[(((wa+8)<<1)+1)&0xFFFF];
        }
        if(bpp===8){
          p4=this.vram[((wa+16)<<1)&0xFFFF]; p5=this.vram[(((wa+16)<<1)+1)&0xFFFF];
          p6=this.vram[((wa+24)<<1)&0xFFFF]; p7=this.vram[(((wa+24)<<1)+1)&0xFFFF];
        }
      }

      const col=xflip?7-pc:pc, bit=7-col;
      let ci;
      if(bpp===2)      ci=((p0>>bit)&1)|(((p1>>bit)&1)<<1);
      else if(bpp===4) ci=((p0>>bit)&1)|(((p1>>bit)&1)<<1)|(((p2>>bit)&1)<<2)|(((p3>>bit)&1)<<3);
      else             ci=((p0>>bit)&1)|(((p1>>bit)&1)<<1)|(((p2>>bit)&1)<<2)|(((p3>>bit)&1)<<3)|
                          (((p4>>bit)&1)<<4)|(((p5>>bit)&1)<<5)|(((p6>>bit)&1)<<6)|(((p7>>bit)&1)<<7);

      if(ci===0){out[x]=null;continue;}

      let cgi;
      if(mode===0)     cgi=bg*32+palette*4+ci;
      else if(bpp===8) cgi=ci;
      else             cgi=palette*(1<<bpp)+ci;
      out[x]={cgi:cgi&0xFF,prio};
    }
    return out;
  }

  _m7Line(y) {
    const out=new Array(256);
    const sel=this.regs[0x1A];
    const sx=(this.bgH[0]<<19)>>19, sy=(this.bgV[0]<<19)>>19;
    const cx=(this.m7cx<<16)>>16,   cy=(this.m7cy<<16)>>16;
    const a=(this.m7a<<16)>>16,     b=(this.m7b<<16)>>16;
    const c=(this.m7c<<16)>>16,     d=(this.m7d<<16)>>16;
    const ry=y+sy-cy;
    for(let x=0;x<256;x++){
      const rx=x+sx-cx;
      let tx=((a*rx+b*ry)>>8)+cx;
      let ty=((c*rx+d*ry)>>8)+cy;
      if(tx<0||tx>1023||ty<0||ty>1023){
        if(sel&0x80){
          if(sel&0x40){out[x]=null;continue;}
          out[x]={cgi:0,prio:0};continue;
        }
        tx&=1023; ty&=1023;
      }
      const mapAddr=((ty>>3)&0x7F)*128+((tx>>3)&0x7F);
      const tileNo=this.vram[(mapAddr<<1)&0xFFFF];
      const charAddr=(tileNo*64+((ty&7)<<3)+(tx&7))&0x7FFF;
      const ci=this.vram[((charAddr<<1)+1)&0xFFFF];
      out[x]=ci===0?null:{cgi:ci,prio:0};
    }
    return out;
  }

  _sprLine(y) {
    const out=new Array(256).fill(null);
    const obsel=this.regs[0x01];
    const SZ=[[8,8,16,16],[8,8,32,32],[8,8,64,64],[16,16,32,32],[16,16,64,64],[32,32,64,64],[16,32,32,64],[16,32,32,64]];
    const sz=SZ[obsel&7];
    const nb0=((obsel>>3)&3)*0x1000;
    const nb1=nb0+((((obsel>>5)&7)+1)<<11);
    let count=0;

    for(let s=0;s<128;s++){
      const o4=s<<2;
      const eb=(this.oam[0x200+(s>>2)]>>((s&3)<<1))&3;
      const large=(eb>>1)&1;
      const spW=large?sz[2]:sz[0], spH=large?sz[3]:sz[1];
      const yPos=this.oam[o4+1];
      const row=(y-yPos)&0xFF;
      if(row>=spH) continue;
      if(count>=32) break;
      count++;

      const xLo=this.oam[o4];
      const xPos=(eb&1)?xLo-256:xLo;
      if(xPos+spW<=0||xPos>=W) continue;

      const tileNo=this.oam[o4+2];
      const attr=this.oam[o4+3];
      const nameT= attr&1;
      const pal  =(attr>>1)&7;
      const prio =(attr>>4)&3;
      const hflip=(attr>>6)&1;
      const vflip=(attr>>7)&1;

      const chrBase=nameT?nb1:nb0;
      const sprRow=vflip?spH-1-row:row;

      for(let px=0;px<spW;px++){
        const sx=xPos+px;
        if(sx<0||sx>=W||out[sx]) continue;
        const col=hflip?spW-1-px:px;
        const stX=(col>>3)&0xF, stY=(sprRow>>3)&0xF;
        const ptX=col&7,        ptY=sprRow&7;
        const sub=((((tileNo>>4)+stY)&0xF)<<4)|((tileNo+stX)&0xF);
        const wa=(chrBase+(sub&0xFF)*16+ptY)&0x7FFF;
        const p0=this.vram[(wa<<1)&0xFFFF];
        const p1=this.vram[((wa<<1)+1)&0xFFFF];
        const p2=this.vram[((wa+8)<<1)&0xFFFF];
        const p3=this.vram[(((wa+8)<<1)+1)&0xFFFF];
        const bit=7-ptX;
        const ci=((p0>>bit)&1)|(((p1>>bit)&1)<<1)|(((p2>>bit)&1)<<2)|(((p3>>bit)&1)<<3);
        if(ci===0) continue;
        out[sx]={cgi:(128+pal*16+ci)&0xFF,prio};
      }
    }
    return out;
  }

  _layers(mode) {
    const bg3hi=!!(this.regs[0x05]&8);
    if(mode===0) return [
      [1,0,3],[0,0,1],[0,1,1],[1,0,2],[0,0,0],[0,1,0],
      [1,0,1],[0,2,1],[0,3,1],[1,0,0],[0,2,0],[0,3,0]
    ];
    if(mode===1&&bg3hi) return [
      [0,2,1],[1,0,3],[0,0,1],[0,1,1],[1,0,2],[0,0,0],[0,1,0],[1,0,1],[1,0,0],[0,2,0]
    ];
    if(mode===1) return [
      [1,0,3],[0,0,1],[0,1,1],[1,0,2],[0,0,0],[0,1,0],[1,0,1],[0,2,1],[1,0,0],[0,2,0]
    ];
    return [
      [1,0,3],[0,0,1],[1,0,2],[0,1,1],[1,0,1],[0,0,0],[1,0,0],[0,1,0]
    ];
  }

  _buildWinMask(layer) {
    let cfgReg, shift;
    if (layer <= 1)      { cfgReg = this.regs[0x23]; shift = layer * 4; }
    else if (layer <= 3) { cfgReg = this.regs[0x24]; shift = (layer-2) * 4; }
    else                 { cfgReg = this.regs[0x25]; shift = (layer-4) * 4; }
    const cfg = (cfgReg >> shift) & 0xF;

    const w1en  = !!(cfg & 0x2), w1inv = !!(cfg & 0x1);
    const w2en  = !!(cfg & 0x8), w2inv = !!(cfg & 0x4);

    const w1l = this.regs[0x26], w1r = this.regs[0x27];
    const w2l = this.regs[0x28], w2r = this.regs[0x29];

    let logReg, logShift;
    if (layer <= 3) { logReg = this.regs[0x2A]; logShift = layer * 2; }
    else            { logReg = this.regs[0x2B]; logShift = (layer-4) * 2; }
    const logOp = (logReg >> logShift) & 0x3;

    const mask = new Uint8Array(256);
    for (let x = 0; x < 256; x++) {
      const in1 = w1en ? ((x >= w1l && x <= w1r) !== w1inv) : false;
      const in2 = w2en ? ((x >= w2l && x <= w2r) !== w2inv) : false;
      if (!w1en && !w2en) { mask[x] = 1; continue; }
      if (w1en && !w2en)  { mask[x] = in1 ? 1 : 0; continue; }
      if (!w1en && w2en)  { mask[x] = in2 ? 1 : 0; continue; }
      let v;
      switch (logOp) {
        case 0: v = in1 || in2;  break;
        case 1: v = in1 && in2;  break;
        case 2: v = in1 !== in2; break;
        case 3: v = in1 === in2; break;
        default: v = false;
      }
      mask[x] = v ? 1 : 0;
    }
    return mask;
  }

  _blendC(main, sub, op, half) {
    let r = (main & 0x1F);
    let g = ((main >> 5) & 0x1F);
    let b = ((main >> 10) & 0x1F);
    const sr = (sub & 0x1F);
    const sg = ((sub >> 5) & 0x1F);
    const sb = ((sub >> 10) & 0x1F);
    if (op === 0) { r+=sr; g+=sg; b+=sb; }
    else          { r-=sr; g-=sg; b-=sb; }
    if (half) { r>>=1; g>>=1; b>>=1; }
    r = Math.max(0, Math.min(31, r));
    g = Math.max(0, Math.min(31, g));
    b = Math.max(0, Math.min(31, b));
    return r | (g<<5) | (b<<10);
  }

  _renderScanline(y) {
    const base=y*W;
    const inidisp=this.regs[0x00];
    if(inidisp&0x80){ this.pixels.fill(0xFF000000,base,base+W); return; }

    const brightness=inidisp&0xF;
    const tm  = this.regs[0x2C];
    const ts  = this.regs[0x2D];
    const mode= this.regs[0x05]&7;
    const bppT=[[2,2,2,2],[4,4,2,0],[4,4,0,0],[8,4,0,0],[8,2,0,0],[4,2,0,0],[4,0,0,0],[0,0,0,0]];

    const cgwsel  = this.regs[0x30];
    const cgadsub = this.regs[0x31];
    const cmAdd   = !(cgadsub & 0x80);
    const cmHalf  = !!(cgadsub & 0x40);
    const cmEn    = cgadsub & 0x3F;
    const cmForce = (cgwsel >> 4) & 0x3;

    const anyWin = (this.regs[0x23]|this.regs[0x24]|this.regs[0x25]) !== 0;
    const tmWin = this.regs[0x2E];
    const tsWin = this.regs[0x2F];
    const needWin = anyWin && (tmWin || tsWin || cmEn);

    const layerWin = [null, null, null, null, null, null];
    const getWin = (li) => {
      if (!layerWin[li]) layerWin[li] = this._buildWinMask(li);
      return layerWin[li];
    };

    let bgL=[null,null,null,null], sprL=null;
    if(mode===7){ if(tm&1) bgL[0]=this._m7Line(y); }
    else{
      for(let bg=0;bg<4;bg++){
        const bpp=bppT[mode][bg];
        if(bpp&&((tm|ts)>>bg)&1) bgL[bg]=this._bgLine(bg,y,bpp);
      }
    }
    if((tm|ts)&0x10) sprL=this._sprLine(y);

    const layers=this._layers(mode);
    const subBD = (cgwsel & 0x02) ? this.fixedColor : this.cgram[0];

    for(let x=0;x<W;x++){
      let mainCGI=0, mainLayer=-1;
      outer:
      for(let li=0;li<layers.length;li++){
        const layer=layers[li];
        const lIdx = layer[0]===1 ? 4 : layer[1];
        const tmBit = layer[0]===1 ? 0x10 : (1<<layer[1]);
        if(!(tm & tmBit)) continue;
        if(needWin && (tmWin & tmBit)) {
          const wm = getWin(lIdx);
          if(!wm[x]) continue;
        }
        if(layer[0]===1){
          if(!sprL) continue;
          const sp=sprL[x];
          if(sp&&sp.prio===layer[2]){mainCGI=sp.cgi; mainLayer=4; break outer;}
        } else {
          const bg=layer[1];
          if(!bgL[bg]) continue;
          const px=bgL[bg][x];
          if(px&&px.prio===layer[2]){mainCGI=px.cgi; mainLayer=bg; break outer;}
        }
      }

      let subC15 = subBD;
      if(cmEn) {
        outer2:
        for(let li=0;li<layers.length;li++){
          const layer=layers[li];
          const tsBit = layer[0]===1 ? 0x10 : (1<<layer[1]);
          if(!(ts & tsBit)) continue;
          const lIdx = layer[0]===1 ? 4 : layer[1];
          if(needWin && (tsWin & tsBit)) {
            const wm = getWin(lIdx);
            if(!wm[x]) continue;
          }
          if(layer[0]===1){
            if(!sprL) continue;
            const sp=sprL[x];
            if(sp&&sp.prio===layer[2]){subC15=this.cgram[sp.cgi]; break outer2;}
          } else {
            const bg=layer[1];
            if(!bgL[bg]) continue;
            const px=bgL[bg][x];
            if(px&&px.prio===layer[2]){subC15=this.cgram[px.cgi]; break outer2;}
          }
        }
      }

      let c15 = this.cgram[mainCGI];
      let doCM = false;
      if(cmForce !== 3 && cmEn) {
        const lbit = mainLayer === -1 ? 0x20 : mainLayer === 4 ? 0x10 : (1 << mainLayer);
        if(cmEn & lbit) {
          if(cmForce === 0) {
            doCM = true;
          } else {
            const cwm = getWin(5);
            if(cmForce === 1) doCM =  !!cwm[x];
            else              doCM = !cwm[x];
          }
        }
      }
      if(doCM) {
        c15 = this._blendC(c15, subC15, cmAdd ? 0 : 1, cmHalf);
      }

      let rgba = this._c(c15);
      if(brightness<15){
        const sc=brightness/15;
        const r=((rgba&0xFF)*sc+0.5)|0;
        const g=(((rgba>>8)&0xFF)*sc+0.5)|0;
        const b=(((rgba>>16)&0xFF)*sc+0.5)|0;
        rgba=(0xFF000000|0)|(b<<16)|(g<<8)|r;
      }
      this.pixels[base+x]=rgba;
    }
  }

  ppuSnapshot() {
    return {
      frame:    this.frame,
      scanline: this.scanline,
      vblank:   this.vblank,
      mode:     this.regs[0x05] & 7,
      bg3hi:    !!(this.regs[0x05] & 8),
      tm:       this.regs[0x2C],
      inidisp:  this.regs[0x00],
    };
  }

  advance(mc) {
    this.clk += mc;
    let done = false;
    while (this.clk >= LINE_MC) {
      this.clk -= LINE_MC;
      const sl = this.scanline;
      if (sl < H) this._renderScanline(sl);
      this.scanline++;
      if (this.scanline === H) {
        this.vblank = true;
        if (this.bus) this.bus.triggerNMI();
      }
      if (this.scanline >= SCANLINES) {
        this.scanline = 0; this.vblank = false; this.frame++; done = true;
      }
    }
    this.hblank = this.clk >= HBLANK_MC;
    return done;
  }
}

// ── SPC700 (Sony SNES Sound CPU) ─────────────────────────────────────────────
class SPC700 {
  constructor() {
    this.ram    = new Uint8Array(0x10000);
    this.A = 0; this.X = 0; this.Y = 0;
    this.SP = 0xFF;
    this.PC = 0xFFC0;
    this.N=0; this.V=0; this.P=0; this.B=0; this.H=0; this.I=0; this.Z=0; this.C=0;
    this.inPorts  = new Uint8Array(4);
    this.outPorts = new Uint8Array(4);
    this.timerEn     = [false,false,false];
    this.timerDiv    = [0,0,0];
    this.timerTarget = [0,0,0];
    this.timerOut    = [0,0,0];
    this.timerCycles = [0,0,0];
    this.dspRegs  = new Uint8Array(128);
    this.dspAddr  = 0;
    this.ctrlReg  = 0xB0;
    this.cycles   = 0;
    this.iplRom = new Uint8Array([
      0xCD,0xEF,0xBD,0xE8,0x00,0xC6,0x1D,0xD0,0xFC,0x8F,0xAA,0xF4,0x8F,0xBB,0xF5,0x78,
      0xCC,0xF4,0xD0,0xFB,0x2F,0x19,0xEB,0xF4,0xD0,0xFC,0x7E,0xF4,0xD0,0x0B,0xE4,0xF5,
      0xCB,0xF4,0xD7,0x00,0xFC,0xD0,0xF3,0xAB,0x01,0x10,0xEF,0x7E,0xF4,0x10,0xEB,0xBA,
      0xF6,0xDA,0x00,0xBA,0xF4,0xC4,0xF4,0xDD,0x5D,0xD0,0xDB,0x1F,0x00,0x00,0xC0,0xFF,
    ]);
    for (let i=0; i<64; i++) this.ram[0xFFC0+i] = this.iplRom[i];
    this.ram[0xFFFE] = 0xC0; this.ram[0xFFFF] = 0xFF;
    this.PC = (this.ram[0xFFFF]<<8)|this.ram[0xFFFE];
  }

  rd(addr) {
    addr &= 0xFFFF;
    if (addr >= 0xFFC0 && (this.ctrlReg & 0x80)) return this.iplRom[addr - 0xFFC0];
    if (addr >= 0x00F0 && addr <= 0x00FF) return this._ioRead(addr);
    return this.ram[addr];
  }

  wr(addr, val) {
    addr &= 0xFFFF; val &= 0xFF;
    if (addr >= 0xFFC0 && (this.ctrlReg & 0x80)) return;
    if (addr >= 0x00F0 && addr <= 0x00FF) { this._ioWrite(addr, val); return; }
    this.ram[addr] = val;
  }

  _ioRead(addr) {
    switch(addr) {
      case 0x00F2: return this.dspAddr;
      case 0x00F3: return this.dspRegs[this.dspAddr & 0x7F];
      case 0x00F4: return this.inPorts[0];
      case 0x00F5: return this.inPorts[1];
      case 0x00F6: return this.inPorts[2];
      case 0x00F7: return this.inPorts[3];
      case 0x00F8: return this.ram[0x00F8];
      case 0x00F9: return this.ram[0x00F9];
      case 0x00FD: return this._readTimer(0);
      case 0x00FE: return this._readTimer(1);
      case 0x00FF: return this._readTimer(2);
      default: return 0;
    }
  }

  _ioWrite(addr, val) {
    switch(addr) {
      case 0x00F1:
        this.ctrlReg = val;
        for (let i=0;i<3;i++) {
          const wasEn = this.timerEn[i];
          this.timerEn[i] = !!(val & (1<<i));
          if (!wasEn && this.timerEn[i]) {
            this.timerDiv[i] = 0; this.timerOut[i] = 0; this.timerCycles[i] = 0;
          }
        }
        if (val & 0x10) { this.inPorts[0]=0; this.inPorts[1]=0; }
        if (val & 0x20) { this.inPorts[2]=0; this.inPorts[3]=0; }
        break;
      case 0x00F2: this.dspAddr = val; break;
      case 0x00F3: if(!(this.dspAddr&0x80)) this.dspRegs[this.dspAddr&0x7F]=val; break;
      case 0x00F4: this.outPorts[0]=val; break;
      case 0x00F5: this.outPorts[1]=val; break;
      case 0x00F6: this.outPorts[2]=val; break;
      case 0x00F7: this.outPorts[3]=val; break;
      case 0x00F8: this.ram[0x00F8]=val; break;
      case 0x00F9: this.ram[0x00F9]=val; break;
      case 0x00FA: this.timerTarget[0]=val||0x100; break;
      case 0x00FB: this.timerTarget[1]=val||0x100; break;
      case 0x00FC: this.timerTarget[2]=val||0x100; break;
    }
  }

  _readTimer(n) {
    const v = this.timerOut[n]; this.timerOut[n]=0; return v & 0x0F;
  }

  _tickTimers() {
    const thresh = [128,128,16];
    for (let i=0;i<3;i++) {
      if (!this.timerEn[i]) continue;
      this.timerCycles[i]++;
      if (this.timerCycles[i] >= thresh[i]) {
        this.timerCycles[i] = 0;
        this.timerDiv[i] = (this.timerDiv[i]+1) & 0xFF;
        if (this.timerDiv[i] >= (this.timerTarget[i]||0x100)) {
          this.timerDiv[i] = 0;
          this.timerOut[i] = (this.timerOut[i]+1) & 0x0F;
        }
      }
    }
  }

  _getP() {
    return (this.N<<7)|(this.V<<6)|(this.P<<5)|(this.B<<4)|
           (this.H<<3)|(this.I<<2)|(this.Z<<1)|(this.C);
  }
  _setP(v) {
    this.N=(v>>7)&1; this.V=(v>>6)&1; this.P=(v>>5)&1; this.B=(v>>4)&1;
    this.H=(v>>3)&1; this.I=(v>>2)&1; this.Z=(v>>1)&1; this.C=v&1;
  }
  _sNZ(v) { this.N=!!(v&0x80); this.Z=((v&0xFF)===0); return v&0xFF; }
  _sNZ16(v){ this.N=!!(v&0x8000);this.Z=((v&0xFFFF)===0);return v&0xFFFF; }

  _push(v) { this.ram[0x100+this.SP]=v&0xFF; this.SP=(this.SP-1)&0xFF; }
  _pop()   { this.SP=(this.SP+1)&0xFF; return this.ram[0x100+this.SP]; }

  _dp()     { return (this.P?0x100:0) + this.rd(this.PC++); }
  _dpx()    { const b=this.rd(this.PC++); return ((this.P?0x100:0)+b+this.X)&0xFFFF; }
  _dpy()    { const b=this.rd(this.PC++); return ((this.P?0x100:0)+b+this.Y)&0xFFFF; }
  _abs()    { const lo=this.rd(this.PC++),hi=this.rd(this.PC++); return (hi<<8)|lo; }
  _absx()   { return (this._abs()+this.X)&0xFFFF; }
  _absy()   { return (this._abs()+this.Y)&0xFFFF; }
  _idx()    { const b=this.rd(this.PC++); const p=this.P?0x100:0;
               const ea=(p+b+this.X)&0xFFFF;
               return (this.rd((ea+1)&0xFFFF)<<8)|this.rd(ea); }
  _idy()    { const b=this.rd(this.PC++); const p=this.P?0x100:0;
               const ea=(p+b)&0xFFFF;
               return (((this.rd((ea+1)&0xFFFF)<<8)|this.rd(ea))+this.Y)&0xFFFF; }

  _adc(a,b){ const r=a+b+this.C; this.C=r>0xFF;
              this.V=(!!(( a^r)&(b^r)&0x80));
              this.H=!!((a^b^r)&0x10); return this._sNZ(r); }
  _sbc(a,b){ return this._adc(a, b^0xFF); }

  _asl(v){ this.C=!!(v&0x80); return this._sNZ((v<<1)&0xFF); }
  _lsr(v){ this.C=v&1;        return this._sNZ(v>>1); }
  _rol(v){ const r=((v<<1)|this.C)&0xFF; this.C=!!(v&0x80); return this._sNZ(r); }
  _ror(v){ const r=((v>>1)|(this.C<<7))&0xFF; this.C=v&1;   return this._sNZ(r); }

  step() {
    this._tickTimers();
    const op = this.rd(this.PC++);
    let cy = 2;
    switch(op) {
      case 0xE8: this.A=this._sNZ(this.rd(this.PC++)); cy=2; break;
      case 0xCD: this.X=this._sNZ(this.rd(this.PC++)); cy=2; break;
      case 0x8D: this.Y=this._sNZ(this.rd(this.PC++)); cy=2; break;
      case 0x7D: this.A=this._sNZ(this.X); cy=2; break;
      case 0xDD: this.A=this._sNZ(this.Y); cy=2; break;
      case 0x5D: this.X=this._sNZ(this.A); cy=2; break;
      case 0xFD: this.Y=this._sNZ(this.A); cy=2; break;
      case 0x9D: this.X=this._sNZ(this.SP);cy=2; break;
      case 0xBD: this.SP=this.X; cy=2; break;
      case 0xE4: this.A=this._sNZ(this.rd(this._dp()));   cy=3; break;
      case 0xF4: this.A=this._sNZ(this.rd(this._dpx()));  cy=4; break;
      case 0xE5: this.A=this._sNZ(this.rd(this._abs()));  cy=4; break;
      case 0xF5: this.A=this._sNZ(this.rd(this._absx())); cy=5; break;
      case 0xF6: this.A=this._sNZ(this.rd(this._absy())); cy=5; break;
      case 0xE6: this.A=this._sNZ(this.rd(this.X+(this.P?0x100:0))); cy=3; break;
      case 0xBF: this.A=this._sNZ(this.rd(this.X+(this.P?0x100:0))); this.X=(this.X+1)&0xFF; cy=4; break;
      case 0xE7: this.A=this._sNZ(this.rd(this._idx()));  cy=6; break;
      case 0xF7: this.A=this._sNZ(this.rd(this._idy()));  cy=6; break;
      case 0xF8: this.X=this._sNZ(this.rd(this._dp()));   cy=3; break;
      case 0xF9: this.X=this._sNZ(this.rd(this._dpy()));  cy=4; break;
      case 0xE9: this.X=this._sNZ(this.rd(this._abs()));  cy=4; break;
      case 0xEB: this.Y=this._sNZ(this.rd(this._dp()));   cy=3; break;
      case 0xFB: this.Y=this._sNZ(this.rd(this._dpy()));  cy=4; break;
      case 0xEC: this.Y=this._sNZ(this.rd(this._abs()));  cy=4; break;
      case 0xC4: this.wr(this._dp(),   this.A); cy=4; break;
      case 0xD4: this.wr(this._dpx(),  this.A); cy=5; break;
      case 0xC5: this.wr(this._abs(),  this.A); cy=5; break;
      case 0xD5: this.wr(this._absx(), this.A); cy=6; break;
      case 0xD6: this.wr(this._absy(), this.A); cy=6; break;
      case 0xC6: this.wr(this.X+(this.P?0x100:0), this.A); cy=4; break;
      case 0xAF: this.wr(this.X+(this.P?0x100:0), this.A); this.X=(this.X+1)&0xFF; cy=4; break;
      case 0xC7: this.wr(this._idx(),  this.A); cy=7; break;
      case 0xD7: this.wr(this._idy(),  this.A); cy=7; break;
      case 0xD8: this.wr(this._dp(),   this.X); cy=4; break;
      case 0xD9: this.wr(this._dpy(),  this.X); cy=5; break;
      case 0xC9: this.wr(this._abs(),  this.X); cy=5; break;
      case 0xCB: this.wr(this._dp(),   this.Y); cy=4; break;
      case 0xDB: this.wr(this._dpy(),  this.Y); cy=5; break;
      case 0xCC: this.wr(this._abs(),  this.Y); cy=5; break;
      case 0x8F: { const imm=this.rd(this.PC++),dst=this._dp(); this.wr(dst,imm); cy=5; break; }
      case 0xFA: { const src=this._dp(),dst=this._dp(); this.wr(dst,this.rd(src)); cy=5; break; }
      case 0xBA: { const a=this._dp(); this.A=this.rd(a); this.Y=this.rd((a+1)&0xFFFF);
                   this._sNZ16((this.Y<<8)|this.A); cy=5; break; }
      case 0xDA: { const a=this._dp(); this.wr(a,this.A); this.wr((a+1)&0xFFFF,this.Y); cy=5; break; }
      case 0x88: this.A=this._adc(this.A,this.rd(this.PC++)); cy=2; break;
      case 0x84: this.A=this._adc(this.A,this.rd(this._dp()));   cy=3; break;
      case 0x94: this.A=this._adc(this.A,this.rd(this._dpx()));  cy=4; break;
      case 0x85: this.A=this._adc(this.A,this.rd(this._abs()));  cy=4; break;
      case 0x95: this.A=this._adc(this.A,this.rd(this._absx())); cy=5; break;
      case 0x96: this.A=this._adc(this.A,this.rd(this._absy())); cy=5; break;
      case 0x86: this.A=this._adc(this.A,this.rd(this.X+(this.P?0x100:0))); cy=3; break;
      case 0x87: this.A=this._adc(this.A,this.rd(this._idx()));  cy=6; break;
      case 0x97: this.A=this._adc(this.A,this.rd(this._idy()));  cy=6; break;
      case 0x99: { const v=this.rd(this.X+(this.P?0x100:0)); this.wr(this.X+(this.P?0x100:0),this._adc(v,this.rd(this.Y+(this.P?0x100:0)))); cy=5; break; }
      case 0x89: { const src=this._dp(),dst=this._dp(); this.wr(dst,this._adc(this.rd(dst),this.rd(src))); cy=6; break; }
      case 0x98: { const imm=this.rd(this.PC++),dst=this._dp(); this.wr(dst,this._adc(this.rd(dst),imm)); cy=5; break; }
      case 0xA8: this.A=this._sbc(this.A,this.rd(this.PC++)); cy=2; break;
      case 0xA4: this.A=this._sbc(this.A,this.rd(this._dp()));   cy=3; break;
      case 0xB4: this.A=this._sbc(this.A,this.rd(this._dpx()));  cy=4; break;
      case 0xA5: this.A=this._sbc(this.A,this.rd(this._abs()));  cy=4; break;
      case 0xB5: this.A=this._sbc(this.A,this.rd(this._absx())); cy=5; break;
      case 0xB6: this.A=this._sbc(this.A,this.rd(this._absy())); cy=5; break;
      case 0xA6: this.A=this._sbc(this.A,this.rd(this.X+(this.P?0x100:0))); cy=3; break;
      case 0xA7: this.A=this._sbc(this.A,this.rd(this._idx()));  cy=6; break;
      case 0xB7: this.A=this._sbc(this.A,this.rd(this._idy()));  cy=6; break;
      case 0xB9: { const v=this.rd(this.X+(this.P?0x100:0)); this.wr(this.X+(this.P?0x100:0),this._sbc(v,this.rd(this.Y+(this.P?0x100:0)))); cy=5; break; }
      case 0xA9: { const src=this._dp(),dst=this._dp(); this.wr(dst,this._sbc(this.rd(dst),this.rd(src))); cy=6; break; }
      case 0xB8: { const imm=this.rd(this.PC++),dst=this._dp(); this.wr(dst,this._sbc(this.rd(dst),imm)); cy=5; break; }
      case 0x68: { const r=this.A-this.rd(this.PC++); this.N=!!(r&0x80); this.Z=!(r&0xFF); this.C=r>=0; cy=2; break; }
      case 0x64: { const r=this.A-this.rd(this._dp());   this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=3;break;}
      case 0x74: { const r=this.A-this.rd(this._dpx());  this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=4;break;}
      case 0x65: { const r=this.A-this.rd(this._abs());  this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=4;break;}
      case 0x75: { const r=this.A-this.rd(this._absx()); this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=5;break;}
      case 0x76: { const r=this.A-this.rd(this._absy()); this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=5;break;}
      case 0x66: { const r=this.A-this.rd(this.X+(this.P?0x100:0)); this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=3;break;}
      case 0x67: { const r=this.A-this.rd(this._idx());  this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=6;break;}
      case 0x77: { const r=this.A-this.rd(this._idy());  this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=6;break;}
      case 0x79: { const r=this.rd(this.X+(this.P?0x100:0))-this.rd(this.Y+(this.P?0x100:0)); this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=5;break;}
      case 0x69: { const src=this._dp(),dst=this._dp(); const r=this.rd(dst)-this.rd(src); this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=6;break;}
      case 0x78: { const imm=this.rd(this.PC++),dst=this._dp(); const r=this.rd(dst)-imm; this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=5;break;}
      case 0xC8: { const r=this.X-this.rd(this.PC++); this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=2;break;}
      case 0x3E: { const r=this.X-this.rd(this._dp());  this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=3;break;}
      case 0x1E: { const r=this.X-this.rd(this._abs()); this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=4;break;}
      case 0xAD: { const r=this.Y-this.rd(this.PC++); this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=2;break;}
      case 0x7E: { const r=this.Y-this.rd(this._dp());  this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=3;break;}
      case 0x5E: { const r=this.Y-this.rd(this._abs()); this.N=!!(r&0x80);this.Z=!(r&0xFF);this.C=r>=0;cy=4;break;}
      case 0x28: this.A=this._sNZ(this.A&this.rd(this.PC++)); cy=2; break;
      case 0x24: this.A=this._sNZ(this.A&this.rd(this._dp()));   cy=3; break;
      case 0x34: this.A=this._sNZ(this.A&this.rd(this._dpx()));  cy=4; break;
      case 0x25: this.A=this._sNZ(this.A&this.rd(this._abs()));  cy=4; break;
      case 0x35: this.A=this._sNZ(this.A&this.rd(this._absx())); cy=5; break;
      case 0x36: this.A=this._sNZ(this.A&this.rd(this._absy())); cy=5; break;
      case 0x26: this.A=this._sNZ(this.A&this.rd(this.X+(this.P?0x100:0))); cy=3; break;
      case 0x27: this.A=this._sNZ(this.A&this.rd(this._idx()));  cy=6; break;
      case 0x37: this.A=this._sNZ(this.A&this.rd(this._idy()));  cy=6; break;
      case 0x39: { const v=this.rd(this.X+(this.P?0x100:0))&this.rd(this.Y+(this.P?0x100:0)); this.wr(this.X+(this.P?0x100:0),this._sNZ(v)); cy=5; break;}
      case 0x29: { const src=this._dp(),dst=this._dp(); this.wr(dst,this._sNZ(this.rd(dst)&this.rd(src))); cy=6; break;}
      case 0x38: { const imm=this.rd(this.PC++),dst=this._dp(); this.wr(dst,this._sNZ(this.rd(dst)&imm)); cy=5; break;}
      case 0x08: this.A=this._sNZ(this.A|this.rd(this.PC++)); cy=2; break;
      case 0x04: this.A=this._sNZ(this.A|this.rd(this._dp()));   cy=3; break;
      case 0x14: this.A=this._sNZ(this.A|this.rd(this._dpx()));  cy=4; break;
      case 0x05: this.A=this._sNZ(this.A|this.rd(this._abs()));  cy=4; break;
      case 0x15: this.A=this._sNZ(this.A|this.rd(this._absx())); cy=5; break;
      case 0x16: this.A=this._sNZ(this.A|this.rd(this._absy())); cy=5; break;
      case 0x06: this.A=this._sNZ(this.A|this.rd(this.X+(this.P?0x100:0))); cy=3; break;
      case 0x07: this.A=this._sNZ(this.A|this.rd(this._idx()));  cy=6; break;
      case 0x17: this.A=this._sNZ(this.A|this.rd(this._idy()));  cy=6; break;
      case 0x19: { const v=this.rd(this.X+(this.P?0x100:0))|this.rd(this.Y+(this.P?0x100:0)); this.wr(this.X+(this.P?0x100:0),this._sNZ(v)); cy=5; break;}
      case 0x09: { const src=this._dp(),dst=this._dp(); this.wr(dst,this._sNZ(this.rd(dst)|this.rd(src))); cy=6; break;}
      case 0x18: { const imm=this.rd(this.PC++),dst=this._dp(); this.wr(dst,this._sNZ(this.rd(dst)|imm)); cy=5; break;}
      case 0x48: this.A=this._sNZ(this.A^this.rd(this.PC++)); cy=2; break;
      case 0x44: this.A=this._sNZ(this.A^this.rd(this._dp()));   cy=3; break;
      case 0x54: this.A=this._sNZ(this.A^this.rd(this._dpx()));  cy=4; break;
      case 0x45: this.A=this._sNZ(this.A^this.rd(this._abs()));  cy=4; break;
      case 0x55: this.A=this._sNZ(this.A^this.rd(this._absx())); cy=5; break;
      case 0x56: this.A=this._sNZ(this.A^this.rd(this._absy())); cy=5; break;
      case 0x46: this.A=this._sNZ(this.A^this.rd(this.X+(this.P?0x100:0))); cy=3; break;
      case 0x47: this.A=this._sNZ(this.A^this.rd(this._idx()));  cy=6; break;
      case 0x57: this.A=this._sNZ(this.A^this.rd(this._idy()));  cy=6; break;
      case 0x59: { const v=this.rd(this.X+(this.P?0x100:0))^this.rd(this.Y+(this.P?0x100:0)); this.wr(this.X+(this.P?0x100:0),this._sNZ(v)); cy=5; break;}
      case 0x49: { const src=this._dp(),dst=this._dp(); this.wr(dst,this._sNZ(this.rd(dst)^this.rd(src))); cy=6; break;}
      case 0x58: { const imm=this.rd(this.PC++),dst=this._dp(); this.wr(dst,this._sNZ(this.rd(dst)^imm)); cy=5; break;}
      case 0xBC: this.A=this._sNZ((this.A+1)&0xFF); cy=2; break;
      case 0x3D: this.X=this._sNZ((this.X+1)&0xFF); cy=2; break;
      case 0xFC: this.Y=this._sNZ((this.Y+1)&0xFF); cy=2; break;
      case 0xAB: { const a=this._dp(); this.wr(a,this._sNZ((this.rd(a)+1)&0xFF)); cy=4; break; }
      case 0xBB: { const a=this._dpx(); this.wr(a,this._sNZ((this.rd(a)+1)&0xFF)); cy=5; break; }
      case 0xAC: { const a=this._abs(); this.wr(a,this._sNZ((this.rd(a)+1)&0xFF)); cy=5; break; }
      case 0x9C: this.A=this._sNZ((this.A-1)&0xFF); cy=2; break;
      case 0x1D: this.X=this._sNZ((this.X-1)&0xFF); cy=2; break;
      case 0xDC: this.Y=this._sNZ((this.Y-1)&0xFF); cy=2; break;
      case 0x8B: { const a=this._dp(); this.wr(a,this._sNZ((this.rd(a)-1)&0xFF)); cy=4; break; }
      case 0x9B: { const a=this._dpx(); this.wr(a,this._sNZ((this.rd(a)-1)&0xFF)); cy=5; break; }
      case 0x8C: { const a=this._abs(); this.wr(a,this._sNZ((this.rd(a)-1)&0xFF)); cy=5; break; }
      case 0x1C: this.A=this._asl(this.A); cy=2; break;
      case 0x0B: { const a=this._dp();  this.wr(a,this._asl(this.rd(a))); cy=4; break; }
      case 0x1B: { const a=this._dpx(); this.wr(a,this._asl(this.rd(a))); cy=5; break; }
      case 0x0C: { const a=this._abs(); this.wr(a,this._asl(this.rd(a))); cy=5; break; }
      case 0x5C: this.A=this._lsr(this.A); cy=2; break;
      case 0x4B: { const a=this._dp();  this.wr(a,this._lsr(this.rd(a))); cy=4; break; }
      case 0x5B: { const a=this._dpx(); this.wr(a,this._lsr(this.rd(a))); cy=5; break; }
      case 0x4C: { const a=this._abs(); this.wr(a,this._lsr(this.rd(a))); cy=5; break; }
      case 0x3C: this.A=this._rol(this.A); cy=2; break;
      case 0x2B: { const a=this._dp();  this.wr(a,this._rol(this.rd(a))); cy=4; break; }
      case 0x3B: { const a=this._dpx(); this.wr(a,this._rol(this.rd(a))); cy=5; break; }
      case 0x2C: { const a=this._abs(); this.wr(a,this._rol(this.rd(a))); cy=5; break; }
      case 0x7C: this.A=this._ror(this.A); cy=2; break;
      case 0x6B: { const a=this._dp();  this.wr(a,this._ror(this.rd(a))); cy=4; break; }
      case 0x7B: { const a=this._dpx(); this.wr(a,this._ror(this.rd(a))); cy=5; break; }
      case 0x6C: { const a=this._abs(); this.wr(a,this._ror(this.rd(a))); cy=5; break; }
      case 0x9F: this.A=this._sNZ(((this.A>>4)|(this.A<<4))&0xFF); cy=5; break;
      case 0xCF: { const r=(this.Y*this.A)&0xFFFF; this.A=r&0xFF; this.Y=(r>>8)&0xFF;
                   this.N=!!(this.Y&0x80); this.Z=this.Y===0; cy=9; break; }
      case 0x9E: { if(this.X===0){this.A=0xFF;this.Y=this.A;this.N=1;this.Z=0;}
                   else{const ya=(this.Y<<8)|this.A; this.A=(ya/this.X)|0; this.Y=ya%this.X;
                        this.N=!!(this.A&0x80); this.Z=this.A===0; this.C=(ya%this.X)>0;}
                   cy=12; break; }
      case 0xDF: { if(this.C||(this.A>0x99)){this.A=(this.A+0x60)&0xFF;this.C=1;}
                   if(this.H||((this.A&0x0F)>9)){this.A=(this.A+6)&0xFF;}
                   this._sNZ(this.A); cy=3; break; }
      case 0xBE: { if(!this.C||(this.A>0x99)){this.A=(this.A-0x60)&0xFF;this.C=0;}
                   if(!this.H||((this.A&0x0F)>9)){this.A=(this.A-6)&0xFF;}
                   this._sNZ(this.A); cy=3; break; }
      case 0x2F: { const off=this.rd(this.PC++)<<24>>24; this.PC=(this.PC+off)&0xFFFF; cy=4; break; }
      case 0xD0: { const off=this.rd(this.PC++)<<24>>24; if(!this.Z){this.PC=(this.PC+off)&0xFFFF;cy=4;}else cy=2; break; }
      case 0xF0: { const off=this.rd(this.PC++)<<24>>24; if( this.Z){this.PC=(this.PC+off)&0xFFFF;cy=4;}else cy=2; break; }
      case 0xB0: { const off=this.rd(this.PC++)<<24>>24; if( this.C){this.PC=(this.PC+off)&0xFFFF;cy=4;}else cy=2; break; }
      case 0x90: { const off=this.rd(this.PC++)<<24>>24; if(!this.C){this.PC=(this.PC+off)&0xFFFF;cy=4;}else cy=2; break; }
      case 0x70: { const off=this.rd(this.PC++)<<24>>24; if( this.V){this.PC=(this.PC+off)&0xFFFF;cy=4;}else cy=2; break; }
      case 0x50: { const off=this.rd(this.PC++)<<24>>24; if(!this.V){this.PC=(this.PC+off)&0xFFFF;cy=4;}else cy=2; break; }
      case 0x30: { const off=this.rd(this.PC++)<<24>>24; if( this.N){this.PC=(this.PC+off)&0xFFFF;cy=4;}else cy=2; break; }
      case 0x10: { const off=this.rd(this.PC++)<<24>>24; if(!this.N){this.PC=(this.PC+off)&0xFFFF;cy=4;}else cy=2; break; }
      case 0x13:case 0x33:case 0x53:case 0x73:case 0x93:case 0xB3:case 0xD3:case 0xF3: {
        const bit=(op>>5)&7,a=this._dp(),off=this.rd(this.PC++)<<24>>24;
        if( (this.rd(a)>>bit)&1){this.PC=(this.PC+off)&0xFFFF;cy=6;}else cy=4; break; }
      case 0x03:case 0x23:case 0x43:case 0x63:case 0x83:case 0xA3:case 0xC3:case 0xE3: {
        const bit=(op>>5)&7,a=this._dp(),off=this.rd(this.PC++)<<24>>24;
        if(!((this.rd(a)>>bit)&1)){this.PC=(this.PC+off)&0xFFFF;cy=6;}else cy=4; break; }
      case 0x2E: { const a=this._dp(),off=this.rd(this.PC++)<<24>>24;
                   if(this.A!==this.rd(a)){this.PC=(this.PC+off)&0xFFFF;cy=6;}else cy=4; break; }
      case 0xDE: { const a=this._dpx(),off=this.rd(this.PC++)<<24>>24;
                   if(this.A!==this.rd(a)){this.PC=(this.PC+off)&0xFFFF;cy=7;}else cy=5; break; }
      case 0x6E: { const a=this._dp(),v=(this.rd(a)-1)&0xFF; this.wr(a,v);
                   const off=this.rd(this.PC++)<<24>>24;
                   if(v!==0){this.PC=(this.PC+off)&0xFFFF;cy=6;}else cy=4; break; }
      case 0xFE: { this.Y=(this.Y-1)&0xFF; const off=this.rd(this.PC++)<<24>>24;
                   if(this.Y!==0){this.PC=(this.PC+off)&0xFFFF;cy=4;}else cy=2; break; }
      case 0x5F: { this.PC=this._abs(); cy=3; break; }
      case 0x1F: { const a=this._abs(); this.PC=(this.rd((a+this.X+1)&0xFFFF)<<8)|this.rd((a+this.X)&0xFFFF); cy=6; break; }
      case 0x3F: { const lo=this.rd(this.PC++),hi=this.rd(this.PC++);
                   this._push((this.PC>>8)&0xFF); this._push(this.PC&0xFF);
                   this.PC=(hi<<8)|lo; cy=8; break; }
      case 0x4F: { const pg=this.rd(this.PC++);
                   this._push((this.PC>>8)&0xFF); this._push(this.PC&0xFF);
                   this.PC=0xFF00|pg; cy=6; break; }
      case 0x6F: { const lo=this._pop(),hi=this._pop(); this.PC=(hi<<8)|lo; cy=5; break; }
      case 0x7F: { const lo=this._pop(),hi=this._pop(); this.PC=(hi<<8)|lo; this._setP(this._pop()); cy=6; break; }
      case 0x01:case 0x11:case 0x21:case 0x31:case 0x41:case 0x51:case 0x61:case 0x71:
      case 0x81:case 0x91:case 0xA1:case 0xB1:case 0xC1:case 0xD1:case 0xE1:case 0xF1: {
        const n=(op>>4),vec=0xFFDE-(n*2);
        this._push((this.PC>>8)&0xFF); this._push(this.PC&0xFF);
        this.PC=(this.rd((vec+1)&0xFFFF)<<8)|this.rd(vec&0xFFFF); cy=8; break; }
      case 0x2D: this._push(this.A); cy=4; break;
      case 0x4D: this._push(this.X); cy=4; break;
      case 0x6D: this._push(this.Y); cy=4; break;
      case 0x0D: this._push(this._getP()); cy=4; break;
      case 0xAE: this.A=this._pop(); cy=4; break;
      case 0xCE: this.X=this._pop(); cy=4; break;
      case 0xEE: this.Y=this._pop(); cy=4; break;
      case 0x8E: this._setP(this._pop()); cy=4; break;
      case 0x02:case 0x22:case 0x42:case 0x62:case 0x82:case 0xA2:case 0xC2:case 0xE2: {
        const bit=(op>>5)&7,a=this._dp(); this.wr(a,this.rd(a)|(1<<bit)); cy=4; break; }
      case 0x12:case 0x32:case 0x52:case 0x72:case 0x92:case 0xB2:case 0xD2:case 0xF2: {
        const bit=(op>>5)&7,a=this._dp(); this.wr(a,this.rd(a)&~(1<<bit)); cy=4; break; }
      case 0xAA: { const ab=this._abs(); this.C=(this.rd(ab>>3)>>(ab&7))&1; cy=4; break; }
      case 0xCA: { const ab=this._abs(); const b=ab&7,a=ab>>3;
                   this.wr(a,this.C?(this.rd(a)|(1<<b)):(this.rd(a)&~(1<<b))); cy=6; break; }
      case 0x6A: { const ab=this._abs(); this.C=this.C&((this.rd(ab>>3)>>(ab&7))&1); cy=4; break; }
      case 0x4A: { const ab=this._abs(); this.C=this.C&(1^((this.rd(ab>>3)>>(ab&7))&1)); cy=4; break; }
      case 0x0A: { const ab=this._abs(); this.C=this.C|((this.rd(ab>>3)>>(ab&7))&1); cy=4; break; }
      case 0x2A: { const ab=this._abs(); this.C=this.C|(1^((this.rd(ab>>3)>>(ab&7))&1)); cy=4; break; }
      case 0x8A: { const ab=this._abs(); this.C=this.C^((this.rd(ab>>3)>>(ab&7))&1); cy=4; break; }
      case 0xEA: { const ab=this._abs(); const b=ab&7,a=ab>>3;
                   this.wr(a,this.rd(a)^(1<<b)); cy=5; break; }
      case 0x60: this.C=0; cy=2; break;
      case 0x80: this.C=1; cy=2; break;
      case 0xED: this.C^=1; cy=2; break;
      case 0xE0: this.V=0;this.H=0; cy=2; break;
      case 0x20: this.P=0; cy=2; break;
      case 0x40: this.P=1; cy=2; break;
      case 0xA0: this.I=1; cy=2; break;
      case 0xC0: this.I=0; cy=2; break;
      case 0x00: cy=2; break;
      case 0xFF: cy=2; break;
      case 0xEF: cy=2; break;
      default:   cy=2; break;
    }
    this.cycles += cy;
    return cy;
  }
}

// ── SNES SYSTEM ───────────────────────────────────────────────────────────────
class SNES {
  constructor(cart) {
    this.cart=cart;
    this.bus=new Bus(cart);
    this.cpu=new CPU65816(this.bus);
    this.spc=new SPC700();
    this.ppu=new PPU();
    this.bus.ppu=this.ppu;
    this.bus.cpu=this.cpu;
    this.bus.spc=this.spc;
    this.ppu.bus=this.bus;
    this.cpu.reset();
    // SPC700 clock ratio: ~1.024 MHz / 21.477272 MHz ≈ 0.04768 SPC cycles per master clock
    this._spcAcc = 0;
  }

  // ── WRAM helpers for Ding achievement engine ──────────────────────────────
  memRead(addr)    { return this.bus.wram[addr & 0x1FFFF]; }
  memReadU16(addr) { return this.memRead(addr) | (this.memRead(addr+1) << 8); }
  memSnapshot(memMap) {
    const s = {};
    for (const { key, addr, u16 } of memMap)
      s[key] = u16 ? this.memReadU16(addr) : this.memRead(addr);
    return s;
  }

  // ── Full diagnostic snapshot (called ~1×/sec from frontend) ──────────────
  diagSnap() {
    const cpu = this.cpu, bus = this.bus, ppu = this.ppu;
    const h2 = v => `$${(v&0xFF).toString(16).toUpperCase().padStart(2,'0')}`;
    const h4 = v => `$${(v&0xFFFF).toString(16).toUpperCase().padStart(4,'0')}`;
    const vecReset = bus.read(0xFFFC) | (bus.read(0xFFFD)<<8);
    const vecNMI   = bus.read(0xFFEA) | (bus.read(0xFFEB)<<8);
    const vecIRQ   = bus.read(0xFFEE) | (bus.read(0xFFEF)<<8);
    const vecBRK   = bus.read(0xFFE6) | (bus.read(0xFFE7)<<8);
    return {
      pc:cpu.PC, pbr:cpu.PBR, a:cpu.A, x:cpu.X, y:cpu.Y,
      sp:cpu.SP, dp:cpu.DP, dbr:cpu.DBR, p:cpu.P, e:cpu.E,
      pendingNMI:cpu.pendingNMI, pendingIRQ:cpu.pendingIRQ,
      stopped:cpu.stopped, waiting:cpu.waiting, cycles:cpu.cycles,
      inidisp:ppu.regs[0x00], obsel:ppu.regs[0x01],
      bgmode:ppu.regs[0x05],  mosaic:ppu.regs[0x06],
      tm:ppu.regs[0x2C],      ts:ppu.regs[0x2D],
      cgwsel:ppu.regs[0x30],  cgadsub:ppu.regs[0x31],
      coldata:ppu.fixedColor,
      vramAddr:ppu.vramAddr, cgramAddr:ppu.cgramAddr, oamAddr:ppu.oamByteAddr,
      scanline:ppu.scanline, frame:ppu.frame, vblank:ppu.vblank,
      nmitimen:bus.nmitimen, nmiFlag:bus.nmiFlag, irqFlag:bus.irqFlag,
      htime:bus.htime, vtime:bus.vtime, memsel:bus.memsel,
      mdmaen:bus.mdmaen, hdmaen:bus.hdmaen,
      apuOut:[...this.bus.apuOut], apuIn:[...this.bus.apuIn],
      apuLog:[...this.bus.apuLog],
      spc: this.spc ? {
        pc:this.spc.PC, a:this.spc.A, x:this.spc.X, y:this.spc.Y,
        sp:this.spc.SP, cycles:this.spc.cycles,
        out:[...this.spc.outPorts], inp:[...this.spc.inPorts],
      } : null,
      vecReset:h4(vecReset), vecNMI:h4(vecNMI), vecIRQ:h4(vecIRQ), vecBRK:h4(vecBRK),
      dma:bus.dmaChannels.map(c=>({
        ctrl:h2(c.ctrl), dest:h2(c.destReg),
        src:`${h2(c.srcBank)}:${h4(c.srcAddr)}`,
        size:c.size, done:c.hdmaFinished,
      })),
      pcHistory:[...cpu.pcHistory],
      bytesAtPC:Array.from({length:8},(_,i)=>bus.read(((cpu.PBR<<16)|((cpu.PC+i)&0xFFFF)))).map(b=>b.toString(16).toUpperCase().padStart(2,'0')),
    };
  }

  runFrame() {
    if (this.bus.hdmaen) this.bus.hdmaInit();

    let lastScanline = this.ppu.scanline;
    let ppuDone      = false;
    let guard        = 0;

    while (!ppuDone && guard++ < 750000) {
      const cyc = this.cpu.step();
      const mc = cyc * (this.bus.memsel & 1 ? 6 : 8);

      this._spcAcc += mc * 0.04768;
      while (this._spcAcc >= 1) {
        this.spc.step();
        this.bus.apuOut[0] = this.spc.outPorts[0];
        this.bus.apuOut[1] = this.spc.outPorts[1];
        this.bus.apuOut[2] = this.spc.outPorts[2];
        this.bus.apuOut[3] = this.spc.outPorts[3];
        this._spcAcc -= 1;
      }

      ppuDone = this.ppu.advance(mc);

      const sl = this.ppu.scanline;
      if (sl !== lastScanline) {
        if (lastScanline < H && this.bus.hdmaen) this.bus.hdmaRun();
        this.bus.checkIRQ(lastScanline);
        lastScanline = sl;
      }
    }
  }
}
