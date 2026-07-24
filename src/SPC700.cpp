// ── SPC700.cpp ───────────────────────────────────────────────────────────────
#include "SPC700.h"

namespace ding::snes {

SPC700::SPC700() {
    for (int i = 0; i < 64; i++) ram[0xFFC0 + i] = kIplRom[i];
    ram[0xFFFE] = 0xC0; ram[0xFFFF] = 0xFF;
    PC = (ram[0xFFFF] << 8) | ram[0xFFFE];
}

uint8_t SPC700::rd(uint16_t addr) {
    addr &= 0xFFFF;
    if (addr >= 0xFFC0 && (ctrlReg & 0x80)) return kIplRom[addr - 0xFFC0];
    if (addr >= 0x00F0 && addr <= 0x00FF) return ioRead(addr);
    return ram[addr];
}

void SPC700::wr(uint16_t addr, uint8_t val) {
    addr &= 0xFFFF; val &= 0xFF;
    if (addr >= 0xFFC0 && (ctrlReg & 0x80)) return;
    if (addr >= 0x00F0 && addr <= 0x00FF) { ioWrite(addr, val); return; }
    ram[addr] = val;
}

uint8_t SPC700::ioRead(uint16_t addr) {
    switch (addr) {
        case 0x00F2: return dspAddr;
        case 0x00F3: return dspRegs[dspAddr & 0x7F];
        case 0x00F4: return inPorts[0];
        case 0x00F5: return inPorts[1];
        case 0x00F6: return inPorts[2];
        case 0x00F7: return inPorts[3];
        case 0x00F8: return ram[0x00F8];
        case 0x00F9: return ram[0x00F9];
        case 0x00FD: return readTimer(0);
        case 0x00FE: return readTimer(1);
        case 0x00FF: return readTimer(2);
        default: return 0;
    }
}

void SPC700::ioWrite(uint16_t addr, uint8_t val) {
    switch (addr) {
        case 0x00F1:
            ctrlReg = val;
            for (int i = 0; i < 3; i++) {
                bool wasEn = timerEn[i];
                timerEn[i] = val & (1 << i);
                if (!wasEn && timerEn[i]) {
                    timerDiv[i] = 0; timerOut[i] = 0; timerCycles[i] = 0;
                }
            }
            if (val & 0x10) { inPorts[0] = 0; inPorts[1] = 0; }
            if (val & 0x20) { inPorts[2] = 0; inPorts[3] = 0; }
            break;
        case 0x00F2: dspAddr = val; break;
        case 0x00F3: if (!(dspAddr & 0x80)) dspRegs[dspAddr & 0x7F] = val; break;
        case 0x00F4: outPorts[0] = val; break;
        case 0x00F5: outPorts[1] = val; break;
        case 0x00F6: outPorts[2] = val; break;
        case 0x00F7: outPorts[3] = val; break;
        case 0x00F8: ram[0x00F8] = val; break;
        case 0x00F9: ram[0x00F9] = val; break;
        case 0x00FA: timerTarget[0] = val ? val : 0x100; break;
        case 0x00FB: timerTarget[1] = val ? val : 0x100; break;
        case 0x00FC: timerTarget[2] = val ? val : 0x100; break;
        default: break;
    }
}

uint8_t SPC700::readTimer(int n) {
    uint8_t v = timerOut[n];
    timerOut[n] = 0;
    return v & 0x0F;
}

void SPC700::genAudio(int masterClocks) {
    // 32000 Hz output (matches DingAudioInfo in ding_core_snes.cpp) driven
    // off the same NTSC master clock everything else uses.
    static constexpr double kSampleRatio = 32000.0 / 21477272.0;
    audioAcc += masterClocks * kSampleRatio;

    bool active = dspRegs[0x4C] != 0; // KON register — any voice key-on

    while (audioAcc >= 1.0) {
        audioAcc -= 1.0;
        float s = 0.0f;
        if (active) {
            tonePhase += 440.0 / 32000.0; // fixed 440Hz blip, not derived from real pitch regs
            if (tonePhase >= 1.0) tonePhase -= 1.0;
            s = (tonePhase < 0.5) ? 0.15f : -0.15f; // quiet — this is a diagnostic, not music
        }
        audioBuf.push_back(s); // L
        audioBuf.push_back(s); // R
    }

    // Cap growth in case the frontend falls behind on draining (~1s of audio).
    constexpr size_t kMaxBuffered = 32000 * 2;
    if (audioBuf.size() > kMaxBuffered) {
        audioBuf.erase(audioBuf.begin(), audioBuf.begin() + (audioBuf.size() - kMaxBuffered));
    }
}

namespace {

// Free functions kept file-local — step() below uses these plus a handful
// of small lambdas closed over `self` for the SPC700-specific addressing
// modes and flag helpers (mirrors the JS's `_dp`, `_sNZ`, `_adc`, etc).

} // namespace

int SPC700::step() {
    // ── local helpers (mirror JS private methods, closed over `this`) ──────
    auto push = [&](uint8_t v) { ram[0x100 + SP] = v & 0xFF; SP = (SP - 1) & 0xFF; };
    auto pop  = [&]() -> uint8_t { SP = (SP + 1) & 0xFF; return ram[0x100 + SP]; };

    auto dp   = [&]() -> uint16_t { return (P ? 0x100 : 0) + rd(PC++); };
    auto dpx  = [&]() -> uint16_t { uint8_t b = rd(PC++); return ((P ? 0x100 : 0) + b + X) & 0xFFFF; };
    auto dpy  = [&]() -> uint16_t { uint8_t b = rd(PC++); return ((P ? 0x100 : 0) + b + Y) & 0xFFFF; };
    auto abs_ = [&]() -> uint16_t { uint8_t lo = rd(PC++), hi = rd(PC++); return (hi << 8) | lo; };
    auto absx = [&]() -> uint16_t { return (abs_() + X) & 0xFFFF; };
    auto absy = [&]() -> uint16_t { return (abs_() + Y) & 0xFFFF; };
    auto idx  = [&]() -> uint16_t {
        uint8_t b = rd(PC++); uint16_t p = P ? 0x100 : 0;
        uint16_t ea = (p + b + X) & 0xFFFF;
        return (rd((ea + 1) & 0xFFFF) << 8) | rd(ea);
    };
    auto idy  = [&]() -> uint16_t {
        uint8_t b = rd(PC++); uint16_t p = P ? 0x100 : 0;
        uint16_t ea = (p + b) & 0xFFFF;
        return (((rd((ea + 1) & 0xFFFF) << 8) | rd(ea)) + Y) & 0xFFFF;
    };

    auto sNZ = [&](int v) -> uint8_t { N = (v & 0x80) != 0; Z = ((v & 0xFF) == 0); return v & 0xFF; };
    auto sNZ16 = [&](int v) -> uint16_t { N = (v & 0x8000) != 0; Z = ((v & 0xFFFF) == 0); return v & 0xFFFF; };

    auto xreg = [&]() -> uint16_t { return X + (P ? 0x100 : 0); };
    auto yreg = [&]() -> uint16_t { return Y + (P ? 0x100 : 0); };

    auto adcOp = [&](int a, int b) -> uint8_t {
        int r = a + b + C;
        C = r > 0xFF;
        V = ((a ^ r) & (b ^ r) & 0x80) != 0;
        H = ((a ^ b ^ r) & 0x10) != 0;
        return sNZ(r);
    };
    auto sbcOp = [&](int a, int b) -> uint8_t { return adcOp(a, b ^ 0xFF); };

    auto asl = [&](int v) -> uint8_t { C = (v & 0x80) != 0; return sNZ((v << 1) & 0xFF); };
    auto lsr = [&](int v) -> uint8_t { C = v & 1;            return sNZ(v >> 1); };
    auto rol = [&](int v) -> uint8_t { int r = ((v << 1) | C) & 0xFF; C = (v & 0x80) != 0; return sNZ(r); };
    auto ror = [&](int v) -> uint8_t { int r = ((v >> 1) | (C << 7)) & 0xFF; C = v & 1;    return sNZ(r); };

    auto getP = [&]() -> uint8_t {
        return (N << 7) | (V << 6) | (P << 5) | (B << 4) | (H << 3) | (I << 2) | (Z << 1) | C;
    };
    auto setP = [&](uint8_t v) {
        N = (v >> 7) & 1; V = (v >> 6) & 1; P = (v >> 5) & 1; B = (v >> 4) & 1;
        H = (v >> 3) & 1; I = (v >> 2) & 1; Z = (v >> 1) & 1; C = v & 1;
    };

    auto cmp = [&](int a, int b) { int r = a - b; N = (r & 0x80) != 0; Z = !(r & 0xFF); C = r >= 0; };
    auto sbyte = [](uint8_t v) -> int8_t { return static_cast<int8_t>(v); };

    uint8_t op = rd(PC++);
    int cy = 2;

    switch (op) {
        case 0xE8: A = sNZ(rd(PC++)); cy = 2; break;
        case 0xCD: X = sNZ(rd(PC++)); cy = 2; break;
        case 0x8D: Y = sNZ(rd(PC++)); cy = 2; break;
        case 0x7D: A = sNZ(X); cy = 2; break;
        case 0xDD: A = sNZ(Y); cy = 2; break;
        case 0x5D: X = sNZ(A); cy = 2; break;
        case 0xFD: Y = sNZ(A); cy = 2; break;
        case 0x9D: X = sNZ(SP); cy = 2; break;
        case 0xBD: SP = X; cy = 2; break;
        case 0xE4: A = sNZ(rd(dp()));   cy = 3; break;
        case 0xF4: A = sNZ(rd(dpx()));  cy = 4; break;
        case 0xE5: A = sNZ(rd(abs_()));  cy = 4; break;
        case 0xF5: A = sNZ(rd(absx())); cy = 5; break;
        case 0xF6: A = sNZ(rd(absy())); cy = 5; break;
        case 0xE6: A = sNZ(rd(xreg())); cy = 3; break;
        case 0xBF: A = sNZ(rd(xreg())); X = (X + 1) & 0xFF; cy = 4; break;
        case 0xE7: A = sNZ(rd(idx()));  cy = 6; break;
        case 0xF7: A = sNZ(rd(idy()));  cy = 6; break;
        case 0xF8: X = sNZ(rd(dp()));   cy = 3; break;
        case 0xF9: X = sNZ(rd(dpy()));  cy = 4; break;
        case 0xE9: X = sNZ(rd(abs_()));  cy = 4; break;
        case 0xEB: Y = sNZ(rd(dp()));   cy = 3; break;
        case 0xFB: Y = sNZ(rd(dpy()));  cy = 4; break;
        case 0xEC: Y = sNZ(rd(abs_()));  cy = 4; break;
        case 0xC4: wr(dp(),   A); cy = 4; break;
        case 0xD4: wr(dpx(),  A); cy = 5; break;
        case 0xC5: wr(abs_(),  A); cy = 5; break;
        case 0xD5: wr(absx(), A); cy = 6; break;
        case 0xD6: wr(absy(), A); cy = 6; break;
        case 0xC6: wr(xreg(), A); cy = 4; break;
        case 0xAF: wr(xreg(), A); X = (X + 1) & 0xFF; cy = 4; break;
        case 0xC7: wr(idx(),  A); cy = 7; break;
        case 0xD7: wr(idy(),  A); cy = 7; break;
        case 0xD8: wr(dp(),   X); cy = 4; break;
        case 0xD9: wr(dpy(),  X); cy = 5; break;
        case 0xC9: wr(abs_(),  X); cy = 5; break;
        case 0xCB: wr(dp(),   Y); cy = 4; break;
        case 0xDB: wr(dpy(),  Y); cy = 5; break;
        case 0xCC: wr(abs_(),  Y); cy = 5; break;
        case 0x8F: { uint8_t imm = rd(PC++); uint16_t dst = dp(); wr(dst, imm); cy = 5; break; }
        case 0xFA: { uint16_t src = dp(), dst = dp(); wr(dst, rd(src)); cy = 5; break; }
        case 0xBA: { uint16_t a = dp(); A = rd(a); Y = rd((a + 1) & 0xFFFF); sNZ16((Y << 8) | A); cy = 5; break; }
        case 0xDA: { uint16_t a = dp(); wr(a, A); wr((a + 1) & 0xFFFF, Y); cy = 5; break; }
        case 0x88: A = adcOp(A, rd(PC++)); cy = 2; break;
        case 0x84: A = adcOp(A, rd(dp()));   cy = 3; break;
        case 0x94: A = adcOp(A, rd(dpx()));  cy = 4; break;
        case 0x85: A = adcOp(A, rd(abs_()));  cy = 4; break;
        case 0x95: A = adcOp(A, rd(absx())); cy = 5; break;
        case 0x96: A = adcOp(A, rd(absy())); cy = 5; break;
        case 0x86: A = adcOp(A, rd(xreg())); cy = 3; break;
        case 0x87: A = adcOp(A, rd(idx()));  cy = 6; break;
        case 0x97: A = adcOp(A, rd(idy()));  cy = 6; break;
        case 0x99: { uint16_t xa = xreg(); uint8_t v = rd(xa); wr(xa, adcOp(v, rd(yreg()))); cy = 5; break; }
        case 0x89: { uint16_t src = dp(), dst = dp(); wr(dst, adcOp(rd(dst), rd(src))); cy = 6; break; }
        case 0x98: { uint8_t imm = rd(PC++); uint16_t dst = dp(); wr(dst, adcOp(rd(dst), imm)); cy = 5; break; }
        case 0xA8: A = sbcOp(A, rd(PC++)); cy = 2; break;
        case 0xA4: A = sbcOp(A, rd(dp()));   cy = 3; break;
        case 0xB4: A = sbcOp(A, rd(dpx()));  cy = 4; break;
        case 0xA5: A = sbcOp(A, rd(abs_()));  cy = 4; break;
        case 0xB5: A = sbcOp(A, rd(absx())); cy = 5; break;
        case 0xB6: A = sbcOp(A, rd(absy())); cy = 5; break;
        case 0xA6: A = sbcOp(A, rd(xreg())); cy = 3; break;
        case 0xA7: A = sbcOp(A, rd(idx()));  cy = 6; break;
        case 0xB7: A = sbcOp(A, rd(idy()));  cy = 6; break;
        case 0xB9: { uint16_t xa = xreg(); uint8_t v = rd(xa); wr(xa, sbcOp(v, rd(yreg()))); cy = 5; break; }
        case 0xA9: { uint16_t src = dp(), dst = dp(); wr(dst, sbcOp(rd(dst), rd(src))); cy = 6; break; }
        case 0xB8: { uint8_t imm = rd(PC++); uint16_t dst = dp(); wr(dst, sbcOp(rd(dst), imm)); cy = 5; break; }
        case 0x68: cmp(A, rd(PC++)); cy = 2; break;
        case 0x64: cmp(A, rd(dp()));   cy = 3; break;
        case 0x74: cmp(A, rd(dpx()));  cy = 4; break;
        case 0x65: cmp(A, rd(abs_()));  cy = 4; break;
        case 0x75: cmp(A, rd(absx())); cy = 5; break;
        case 0x76: cmp(A, rd(absy())); cy = 5; break;
        case 0x66: cmp(A, rd(xreg())); cy = 3; break;
        case 0x67: cmp(A, rd(idx()));  cy = 6; break;
        case 0x77: cmp(A, rd(idy()));  cy = 6; break;
        case 0x79: cmp(rd(xreg()), rd(yreg())); cy = 5; break;
        case 0x69: { uint16_t src = dp(), dst = dp(); cmp(rd(dst), rd(src)); cy = 6; break; }
        case 0x78: { uint8_t imm = rd(PC++); uint16_t dst = dp(); cmp(rd(dst), imm); cy = 5; break; }
        case 0xC8: cmp(X, rd(PC++)); cy = 2; break;
        case 0x3E: cmp(X, rd(dp()));  cy = 3; break;
        case 0x1E: cmp(X, rd(abs_())); cy = 4; break;
        case 0xAD: cmp(Y, rd(PC++)); cy = 2; break;
        case 0x7E: cmp(Y, rd(dp()));  cy = 3; break;
        case 0x5E: cmp(Y, rd(abs_())); cy = 4; break;
        case 0x28: A = sNZ(A & rd(PC++)); cy = 2; break;
        case 0x24: A = sNZ(A & rd(dp()));   cy = 3; break;
        case 0x34: A = sNZ(A & rd(dpx()));  cy = 4; break;
        case 0x25: A = sNZ(A & rd(abs_()));  cy = 4; break;
        case 0x35: A = sNZ(A & rd(absx())); cy = 5; break;
        case 0x36: A = sNZ(A & rd(absy())); cy = 5; break;
        case 0x26: A = sNZ(A & rd(xreg())); cy = 3; break;
        case 0x27: A = sNZ(A & rd(idx()));  cy = 6; break;
        case 0x37: A = sNZ(A & rd(idy()));  cy = 6; break;
        case 0x39: { uint16_t xa = xreg(); uint8_t v = rd(xa) & rd(yreg()); wr(xa, sNZ(v)); cy = 5; break; }
        case 0x29: { uint16_t src = dp(), dst = dp(); wr(dst, sNZ(rd(dst) & rd(src))); cy = 6; break; }
        case 0x38: { uint8_t imm = rd(PC++); uint16_t dst = dp(); wr(dst, sNZ(rd(dst) & imm)); cy = 5; break; }
        case 0x08: A = sNZ(A | rd(PC++)); cy = 2; break;
        case 0x04: A = sNZ(A | rd(dp()));   cy = 3; break;
        case 0x14: A = sNZ(A | rd(dpx()));  cy = 4; break;
        case 0x05: A = sNZ(A | rd(abs_()));  cy = 4; break;
        case 0x15: A = sNZ(A | rd(absx())); cy = 5; break;
        case 0x16: A = sNZ(A | rd(absy())); cy = 5; break;
        case 0x06: A = sNZ(A | rd(xreg())); cy = 3; break;
        case 0x07: A = sNZ(A | rd(idx()));  cy = 6; break;
        case 0x17: A = sNZ(A | rd(idy()));  cy = 6; break;
        case 0x19: { uint16_t xa = xreg(); uint8_t v = rd(xa) | rd(yreg()); wr(xa, sNZ(v)); cy = 5; break; }
        case 0x09: { uint16_t src = dp(), dst = dp(); wr(dst, sNZ(rd(dst) | rd(src))); cy = 6; break; }
        case 0x18: { uint8_t imm = rd(PC++); uint16_t dst = dp(); wr(dst, sNZ(rd(dst) | imm)); cy = 5; break; }
        case 0x48: A = sNZ(A ^ rd(PC++)); cy = 2; break;
        case 0x44: A = sNZ(A ^ rd(dp()));   cy = 3; break;
        case 0x54: A = sNZ(A ^ rd(dpx()));  cy = 4; break;
        case 0x45: A = sNZ(A ^ rd(abs_()));  cy = 4; break;
        case 0x55: A = sNZ(A ^ rd(absx())); cy = 5; break;
        case 0x56: A = sNZ(A ^ rd(absy())); cy = 5; break;
        case 0x46: A = sNZ(A ^ rd(xreg())); cy = 3; break;
        case 0x47: A = sNZ(A ^ rd(idx()));  cy = 6; break;
        case 0x57: A = sNZ(A ^ rd(idy()));  cy = 6; break;
        case 0x59: { uint16_t xa = xreg(); uint8_t v = rd(xa) ^ rd(yreg()); wr(xa, sNZ(v)); cy = 5; break; }
        case 0x49: { uint16_t src = dp(), dst = dp(); wr(dst, sNZ(rd(dst) ^ rd(src))); cy = 6; break; }
        case 0x58: { uint8_t imm = rd(PC++); uint16_t dst = dp(); wr(dst, sNZ(rd(dst) ^ imm)); cy = 5; break; }
        case 0xBC: A = sNZ((A + 1) & 0xFF); cy = 2; break;
        case 0x3D: X = sNZ((X + 1) & 0xFF); cy = 2; break;
        case 0xFC: Y = sNZ((Y + 1) & 0xFF); cy = 2; break;
        case 0xAB: { uint16_t a = dp();  wr(a, sNZ((rd(a) + 1) & 0xFF)); cy = 4; break; }
        case 0xBB: { uint16_t a = dpx(); wr(a, sNZ((rd(a) + 1) & 0xFF)); cy = 5; break; }
        case 0xAC: { uint16_t a = abs_(); wr(a, sNZ((rd(a) + 1) & 0xFF)); cy = 5; break; }
        case 0x9C: A = sNZ((A - 1) & 0xFF); cy = 2; break;
        case 0x1D: X = sNZ((X - 1) & 0xFF); cy = 2; break;
        case 0xDC: Y = sNZ((Y - 1) & 0xFF); cy = 2; break;
        case 0x8B: { uint16_t a = dp();  wr(a, sNZ((rd(a) - 1) & 0xFF)); cy = 4; break; }
        case 0x9B: { uint16_t a = dpx(); wr(a, sNZ((rd(a) - 1) & 0xFF)); cy = 5; break; }
        case 0x8C: { uint16_t a = abs_(); wr(a, sNZ((rd(a) - 1) & 0xFF)); cy = 5; break; }
        case 0x1C: A = asl(A); cy = 2; break;
        case 0x0B: { uint16_t a = dp();  wr(a, asl(rd(a))); cy = 4; break; }
        case 0x1B: { uint16_t a = dpx(); wr(a, asl(rd(a))); cy = 5; break; }
        case 0x0C: { uint16_t a = abs_(); wr(a, asl(rd(a))); cy = 5; break; }
        case 0x5C: A = lsr(A); cy = 2; break;
        case 0x4B: { uint16_t a = dp();  wr(a, lsr(rd(a))); cy = 4; break; }
        case 0x5B: { uint16_t a = dpx(); wr(a, lsr(rd(a))); cy = 5; break; }
        case 0x4C: { uint16_t a = abs_(); wr(a, lsr(rd(a))); cy = 5; break; }
        case 0x3C: A = rol(A); cy = 2; break;
        case 0x2B: { uint16_t a = dp();  wr(a, rol(rd(a))); cy = 4; break; }
        case 0x3B: { uint16_t a = dpx(); wr(a, rol(rd(a))); cy = 5; break; }
        case 0x2C: { uint16_t a = abs_(); wr(a, rol(rd(a))); cy = 5; break; }
        case 0x7C: A = ror(A); cy = 2; break;
        case 0x6B: { uint16_t a = dp();  wr(a, ror(rd(a))); cy = 4; break; }
        case 0x7B: { uint16_t a = dpx(); wr(a, ror(rd(a))); cy = 5; break; }
        case 0x6C: { uint16_t a = abs_(); wr(a, ror(rd(a))); cy = 5; break; }
        case 0x9F: A = sNZ(((A >> 4) | (A << 4)) & 0xFF); cy = 5; break;
        case 0xCF: { int r = (Y * A) & 0xFFFF; A = r & 0xFF; Y = (r >> 8) & 0xFF; N = (Y & 0x80) != 0; Z = (Y == 0); cy = 9; break; }
        case 0x9E: {
            if (X == 0) { A = 0xFF; Y = A; N = 1; Z = 0; }
            else {
                int ya = (Y << 8) | A;
                A = (ya / X) & 0xFF;
                Y = ya % X;
                N = (A & 0x80) != 0; Z = (A == 0); C = (ya % X) > 0;
            }
            cy = 12; break;
        }
        case 0xDF: {
            if (C || (A > 0x99)) { A = (A + 0x60) & 0xFF; C = 1; }
            if (H || ((A & 0x0F) > 9)) { A = (A + 6) & 0xFF; }
            sNZ(A); cy = 3; break;
        }
        case 0xBE: {
            if (!C || (A > 0x99)) { A = (A - 0x60) & 0xFF; C = 0; }
            if (!H || ((A & 0x0F) > 9)) { A = (A - 6) & 0xFF; }
            sNZ(A); cy = 3; break;
        }
        case 0x2F: { int off = sbyte(rd(PC++)); PC = (PC + off) & 0xFFFF; cy = 4; break; }
        case 0xD0: { int off = sbyte(rd(PC++)); if (!Z) { PC = (PC + off) & 0xFFFF; cy = 4; } else cy = 2; break; }
        case 0xF0: { int off = sbyte(rd(PC++)); if ( Z) { PC = (PC + off) & 0xFFFF; cy = 4; } else cy = 2; break; }
        case 0xB0: { int off = sbyte(rd(PC++)); if ( C) { PC = (PC + off) & 0xFFFF; cy = 4; } else cy = 2; break; }
        case 0x90: { int off = sbyte(rd(PC++)); if (!C) { PC = (PC + off) & 0xFFFF; cy = 4; } else cy = 2; break; }
        case 0x70: { int off = sbyte(rd(PC++)); if ( V) { PC = (PC + off) & 0xFFFF; cy = 4; } else cy = 2; break; }
        case 0x50: { int off = sbyte(rd(PC++)); if (!V) { PC = (PC + off) & 0xFFFF; cy = 4; } else cy = 2; break; }
        case 0x30: { int off = sbyte(rd(PC++)); if ( N) { PC = (PC + off) & 0xFFFF; cy = 4; } else cy = 2; break; }
        case 0x10: { int off = sbyte(rd(PC++)); if (!N) { PC = (PC + off) & 0xFFFF; cy = 4; } else cy = 2; break; }
        case 0x13: case 0x33: case 0x53: case 0x73: case 0x93: case 0xB3: case 0xD3: case 0xF3: {
            int bit = (op >> 5) & 7; uint16_t a = dp(); int off = sbyte(rd(PC++));
            if ((rd(a) >> bit) & 1) { PC = (PC + off) & 0xFFFF; cy = 6; } else cy = 4;
            break;
        }
        case 0x03: case 0x23: case 0x43: case 0x63: case 0x83: case 0xA3: case 0xC3: case 0xE3: {
            int bit = (op >> 5) & 7; uint16_t a = dp(); int off = sbyte(rd(PC++));
            if (!((rd(a) >> bit) & 1)) { PC = (PC + off) & 0xFFFF; cy = 6; } else cy = 4;
            break;
        }
        case 0x2E: { uint16_t a = dp(); int off = sbyte(rd(PC++)); if (A != rd(a)) { PC = (PC + off) & 0xFFFF; cy = 6; } else cy = 4; break; }
        case 0xDE: { uint16_t a = dpx(); int off = sbyte(rd(PC++)); if (A != rd(a)) { PC = (PC + off) & 0xFFFF; cy = 7; } else cy = 5; break; }
        case 0x6E: {
            uint16_t a = dp(); uint8_t v = (rd(a) - 1) & 0xFF; wr(a, v);
            int off = sbyte(rd(PC++));
            if (v != 0) { PC = (PC + off) & 0xFFFF; cy = 6; } else cy = 4;
            break;
        }
        case 0xFE: { Y = (Y - 1) & 0xFF; int off = sbyte(rd(PC++)); if (Y != 0) { PC = (PC + off) & 0xFFFF; cy = 4; } else cy = 2; break; }
        case 0x5F: { PC = abs_(); cy = 3; break; }
        case 0x1F: { uint16_t a = abs_(); PC = (rd((a + X + 1) & 0xFFFF) << 8) | rd((a + X) & 0xFFFF); cy = 6; break; }
        case 0x3F: {
            uint8_t lo = rd(PC++), hi = rd(PC++);
            push((PC >> 8) & 0xFF); push(PC & 0xFF);
            PC = (hi << 8) | lo; cy = 8; break;
        }
        case 0x4F: {
            uint8_t pg = rd(PC++);
            push((PC >> 8) & 0xFF); push(PC & 0xFF);
            PC = 0xFF00 | pg; cy = 6; break;
        }
        case 0x6F: { uint8_t lo = pop(), hi = pop(); PC = (hi << 8) | lo; cy = 5; break; }
        case 0x7F: { uint8_t lo = pop(), hi = pop(); PC = (hi << 8) | lo; setP(pop()); cy = 6; break; }
        case 0x01: case 0x11: case 0x21: case 0x31: case 0x41: case 0x51: case 0x61: case 0x71:
        case 0x81: case 0x91: case 0xA1: case 0xB1: case 0xC1: case 0xD1: case 0xE1: case 0xF1: {
            int n = op >> 4; uint16_t vec = static_cast<uint16_t>(0xFFDE - (n * 2));
            push((PC >> 8) & 0xFF); push(PC & 0xFF);
            PC = (rd((vec + 1) & 0xFFFF) << 8) | rd(vec & 0xFFFF); cy = 8; break;
        }
        case 0x2D: push(A); cy = 4; break;
        case 0x4D: push(X); cy = 4; break;
        case 0x6D: push(Y); cy = 4; break;
        case 0x0D: push(getP()); cy = 4; break;
        case 0xAE: A = pop(); cy = 4; break;
        case 0xCE: X = pop(); cy = 4; break;
        case 0xEE: Y = pop(); cy = 4; break;
        case 0x8E: setP(pop()); cy = 4; break;
        case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xA2: case 0xC2: case 0xE2: {
            int bit = (op >> 5) & 7; uint16_t a = dp(); wr(a, rd(a) | (1 << bit)); cy = 4; break;
        }
        case 0x12: case 0x32: case 0x52: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2: {
            int bit = (op >> 5) & 7; uint16_t a = dp(); wr(a, rd(a) & ~(1 << bit)); cy = 4; break;
        }
        case 0xAA: { uint16_t ab = abs_(); C = (rd(ab >> 3) >> (ab & 7)) & 1; cy = 4; break; }
        case 0xCA: { uint16_t ab = abs_(); int b = ab & 7; uint16_t a = ab >> 3; wr(a, C ? (rd(a) | (1 << b)) : (rd(a) & ~(1 << b))); cy = 6; break; }
        case 0x6A: { uint16_t ab = abs_(); C = C & ((rd(ab >> 3) >> (ab & 7)) & 1); cy = 4; break; }
        case 0x4A: { uint16_t ab = abs_(); C = C & (1 ^ ((rd(ab >> 3) >> (ab & 7)) & 1)); cy = 4; break; }
        case 0x0A: { uint16_t ab = abs_(); C = C | ((rd(ab >> 3) >> (ab & 7)) & 1); cy = 4; break; }
        case 0x2A: { uint16_t ab = abs_(); C = C | (1 ^ ((rd(ab >> 3) >> (ab & 7)) & 1)); cy = 4; break; }
        case 0x8A: { uint16_t ab = abs_(); C = C ^ ((rd(ab >> 3) >> (ab & 7)) & 1); cy = 4; break; }
        case 0xEA: { uint16_t ab = abs_(); int b = ab & 7; uint16_t a = ab >> 3; wr(a, rd(a) ^ (1 << b)); cy = 5; break; }
        case 0x0E: { uint16_t a = abs_(); uint8_t v = rd(a); sNZ(A - v); wr(a, v | A); cy = 6; break; }
        case 0x4E: { uint16_t a = abs_(); uint8_t v = rd(a); sNZ(A - v); wr(a, v & ~A); cy = 6; break; }
        case 0x60: C = 0; cy = 2; break;
        case 0x80: C = 1; cy = 2; break;
        case 0xED: C ^= 1; cy = 2; break;
        case 0xE0: V = 0; H = 0; cy = 2; break;
        case 0x20: P = 0; cy = 2; break;
        case 0x40: P = 1; cy = 2; break;
        case 0xA0: I = 1; cy = 2; break;
        case 0xC0: I = 0; cy = 2; break;
        case 0x00: cy = 2; break;
        case 0xFF: cy = 2; break;
        case 0xEF: cy = 2; break;
        default:   cy = 2; break;
    }

    // ── timer tick — advances by this instruction's actual cycle count, not
    // by 1 per call, since real timers divide down from elapsed SPC clock
    // cycles and instructions vary from 2 to 8+ cycles each.
    {
        static constexpr int thresh[3] = {128, 128, 16};
        for (int i = 0; i < 3; i++) {
            if (!timerEn[i]) continue;
            timerCycles[i] += cy;
            while (timerCycles[i] >= thresh[i]) {
                timerCycles[i] -= thresh[i];
                timerDiv[i] = (timerDiv[i] + 1) & 0xFF;
                uint16_t target = timerTarget[i] ? timerTarget[i] : 0x100;
                if (timerDiv[i] >= target) {
                    timerDiv[i] = 0;
                    timerOut[i] = (timerOut[i] + 1) & 0x0F;
                }
            }
        }
    }

cycles += cy;
    if (pcTrace.empty() || pcTrace.back() != PC) {
        pcTrace.push_back(PC);
        if (pcTrace.size() > 16) pcTrace.erase(pcTrace.begin());
    }
    return cy;
}

} // namespace ding::snes
