// ── CPU65816.cpp ─────────────────────────────────────────────────────────────
#include "CPU65816.h"

#include "Bus.h"

namespace ding::snes {

CPU65816::CPU65816(Bus& bus) : bus(bus) {}

void CPU65816::nzM(uint32_t v) {
    sN((v & (fM() ? 0x80 : 0x8000)) != 0);
    sZ((v & (fM() ? 0xFF : 0xFFFF)) == 0);
}
void CPU65816::nzX(uint32_t v) {
    sN((v & (fX() ? 0x80 : 0x8000)) != 0);
    sZ((v & (fX() ? 0xFF : 0xFFFF)) == 0);
}

// ── Memory access ───────────────────────────────────────────────────────────
uint8_t  CPU65816::rd(uint32_t a24)      { return bus.read(a24 & 0xFFFFFF); }
void     CPU65816::wr(uint32_t a24, uint8_t v) { bus.write(a24 & 0xFFFFFF, v & 0xFF); }
uint8_t  CPU65816::r8(uint8_t bk, uint16_t ad)  { return rd(((bk & 0xFF) << 16) | (ad & 0xFFFF)); }
uint16_t CPU65816::r16(uint8_t bk, uint16_t ad) { return r8(bk, ad) | (r8(bk, (ad + 1) & 0xFFFF) << 8); }
uint32_t CPU65816::r24(uint8_t bk, uint16_t ad) {
    return r8(bk, ad) | (r8(bk, (ad + 1) & 0xFFFF) << 8) | (r8(bk, (ad + 2) & 0xFFFF) << 16);
}
void CPU65816::w8(uint8_t bk, uint16_t ad, uint8_t v) { wr(((bk & 0xFF) << 16) | (ad & 0xFFFF), v); }
void CPU65816::w16(uint8_t bk, uint16_t ad, uint16_t v) { w8(bk, ad, v & 0xFF); w8(bk, (ad + 1) & 0xFFFF, v >> 8); }

uint8_t CPU65816::f8() { uint8_t v = r8(PBR, PC); PC = (PC + 1) & 0xFFFF; return v; }
uint16_t CPU65816::f16() { uint8_t lo = f8(), hi = f8(); return (hi << 8) | lo; }
uint32_t CPU65816::f24() { uint8_t lo = f8(), hi = f8(), bk = f8(); return (bk << 16) | (hi << 8) | lo; }

void CPU65816::ph8(uint8_t v) {
    w8(0, SP, v);
    SP = E ? (0x100 | ((SP - 1) & 0xFF)) : ((SP - 1) & 0xFFFF);
}
void CPU65816::ph16(uint16_t v) { ph8(v >> 8); ph8(v & 0xFF); }
uint8_t CPU65816::pl8() {
    SP = E ? (0x100 | ((SP + 1) & 0xFF)) : ((SP + 1) & 0xFFFF);
    return r8(0, SP);
}
uint16_t CPU65816::pl16() { uint8_t lo = pl8(), hi = pl8(); return (hi << 8) | lo; }

uint16_t CPU65816::rdM(uint32_t a) {
    if (fM()) return rd(a) & 0xFF;
    return rd(a) | (rd((a & 0xFF0000) | ((a + 1) & 0xFFFF)) << 8);
}
void CPU65816::wrM(uint32_t a, uint16_t v) {
    wr(a, v & 0xFF);
    if (!fM()) wr((a & 0xFF0000) | ((a + 1) & 0xFFFF), (v >> 8) & 0xFF);
}
uint16_t CPU65816::rdX(uint32_t a) {
    if (fX()) return rd(a) & 0xFF;
    return rd(a) | (rd((a & 0xFF0000) | ((a + 1) & 0xFFFF)) << 8);
}
void CPU65816::wrX(uint32_t a, uint16_t v) {
    wr(a, v & 0xFF);
    if (!fX()) wr((a & 0xFF0000) | ((a + 1) & 0xFFFF), (v >> 8) & 0xFF);
}

// ── Addressing modes ─────────────────────────────────────────────────────────
uint32_t CPU65816::amDP()  { uint8_t d = f8(); return (DP + d) & 0xFFFF; }
uint32_t CPU65816::amDPX() { uint8_t d = f8(); return (DP + d + X) & 0xFFFF; }
uint32_t CPU65816::amDPY() { uint8_t d = f8(); return (DP + d + Y) & 0xFFFF; }
uint32_t CPU65816::amAbs() { return (static_cast<uint32_t>(DBR) << 16) | f16(); }
uint32_t CPU65816::amAbX() { return (static_cast<uint32_t>(DBR) << 16) | ((f16() + X) & 0xFFFF); }
uint32_t CPU65816::amAbY() { return (static_cast<uint32_t>(DBR) << 16) | ((f16() + Y) & 0xFFFF); }
uint32_t CPU65816::amLng() { return f24(); }
uint32_t CPU65816::amLnX() { return (f24() + X) & 0xFFFFFF; }
uint32_t CPU65816::amSR()  { uint8_t o = f8(); return (SP + o) & 0xFFFF; }
uint32_t CPU65816::amDPI()  { uint32_t ea = amDP(); return (static_cast<uint32_t>(DBR) << 16) | r16(0, static_cast<uint16_t>(ea)); }
uint32_t CPU65816::amDPIX() { uint8_t d = f8(); uint16_t ptr = (DP + d + X) & 0xFFFF; return (static_cast<uint32_t>(DBR) << 16) | r16(0, ptr); }
uint32_t CPU65816::amDPIY() { uint32_t ea = amDP(); return (static_cast<uint32_t>(DBR) << 16) | ((r16(0, static_cast<uint16_t>(ea)) + Y) & 0xFFFF); }
uint32_t CPU65816::amDPIL() { uint32_t ea = amDP(); return r24(0, static_cast<uint16_t>(ea)); }
uint32_t CPU65816::amDPILY(){ uint32_t ea = amDP(); return (r24(0, static_cast<uint16_t>(ea)) + Y) & 0xFFFFFF; }
uint32_t CPU65816::amSRIY() { uint16_t ptr = (SP + f8()) & 0xFFFF; return (static_cast<uint32_t>(DBR) << 16) | ((r16(0, ptr) + Y) & 0xFFFF); }
uint32_t CPU65816::amImmM() { uint32_t a = (static_cast<uint32_t>(PBR) << 16) | PC; PC = (PC + (fM() ? 1 : 2)) & 0xFFFF; return a; }
uint32_t CPU65816::amImmX() { uint32_t a = (static_cast<uint32_t>(PBR) << 16) | PC; PC = (PC + (fX() ? 1 : 2)) & 0xFFFF; return a; }

// ── Arithmetic ────────────────────────────────────────────────────────────────
void CPU65816::adc(uint32_t val) {
    if (fM()) {
        uint32_t a = A & 0xFF, r = a + (val & 0xFF) + (fC() ? 1 : 0);
        sV((~(a ^ val) & (a ^ r) & 0x80) != 0);
        sC(r > 0xFF);
        A = (A & 0xFF00) | (r & 0xFF);
        nzM(A);
    } else {
        uint32_t a = A & 0xFFFF, r = a + (val & 0xFFFF) + (fC() ? 1 : 0);
        sV((~(a ^ val) & (a ^ r) & 0x8000) != 0);
        sC(r > 0xFFFF);
        A = r & 0xFFFF;
        nzM(A);
    }
}
void CPU65816::sbc(uint32_t val) {
    if (fM()) {
        uint32_t a = A & 0xFF, v = val & 0xFF, r = a - v - (fC() ? 0 : 1);
        sV(((a ^ v) & (a ^ r) & 0x80) != 0);
        sC(static_cast<int32_t>(r) >= 0);
        A = (A & 0xFF00) | (r & 0xFF);
        nzM(A);
    } else {
        uint32_t a = A & 0xFFFF, v = val & 0xFFFF, r = a - v - (fC() ? 0 : 1);
        sV(((a ^ v) & (a ^ r) & 0x8000) != 0);
        sC(static_cast<int32_t>(r) >= 0);
        A = r & 0xFFFF;
        nzM(A);
    }
}

void CPU65816::reset() {
    E = true; P = 0x34; SP = 0x01FF; DP = DBR = PBR = 0;
    stopped = false; waiting = false; cycles = 0;
    PC = r16(0, 0xFFFC);
    pcTrace.clear(); _lastTracedPC = -1;
    pcHistory.clear(); _lastTracePc = -1;
}

int CPU65816::branch(bool taken) {
    uint8_t o = f8();
    if (!taken) return 2;
    PC = (PC + (o < 0x80 ? o : o - 0x100)) & 0xFFFF;
    return 3;
}

void CPU65816::doNMI() {
    if (!E) ph8(PBR);
    ph16(PC); ph8(P & (E ? 0xEF : 0xFF));
    sI(true); PBR = 0;
    PC = r16(0, E ? 0xFFFA : 0xFFEA);
    pendingNMI = false; waiting = false;
}
void CPU65816::doIRQ() {
    if (!E) ph8(PBR);
    ph16(PC); ph8(P & (E ? 0xEF : 0xFF));
    sI(true); PBR = 0;
    PC = r16(0, E ? 0xFFFE : 0xFFEE);
    pendingIRQ = false; waiting = false;
}

// ── Step ──────────────────────────────────────────────────────────────────────
int CPU65816::step() {
    if (stopped) return 2;
    if (waiting) {
        bool nmiSignal = (bus.nmiFlag & 0x80) != 0;
        if (!pendingNMI && !pendingIRQ && !nmiSignal) return 2;
        waiting = false;
    }
    if (pendingNMI) { doNMI(); return 8; }
    if (pendingIRQ && !fI()) { doIRQ(); return 8; }

    uint8_t op = f8();
    int32_t traceKey = (static_cast<int32_t>(PBR) << 16) | ((PC - 1) & 0xFFFF);
    if (traceKey != _lastTracedPC) {
        _lastTracedPC = traceKey;
        pcTrace.push_back({PBR, static_cast<uint16_t>((PC - 1) & 0xFFFF), op});
        if (pcTrace.size() > 24) pcTrace.erase(pcTrace.begin());
    }
    int cy = 2;

    switch (op) {
        case 0xA9: { auto v = rdM(amImmM()); A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = fM() ? 2 : 3; break; }
        case 0xA5: { auto v = rdM(amDP());  A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 3; break; }
        case 0xB5: { auto v = rdM(amDPX()); A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 4; break; }
        case 0xAD: { auto v = rdM(amAbs()); A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 4; break; }
        case 0xBD: { auto v = rdM(amAbX()); A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 4; break; }
        case 0xB9: { auto v = rdM(amAbY()); A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 4; break; }
        case 0xAF: { auto v = rdM(amLng()); A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 5; break; }
        case 0xBF: { auto v = rdM(amLnX()); A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 5; break; }
        case 0xA1: { auto v = rdM(amDPIX()); A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 6; break; }
        case 0xB1: { auto v = rdM(amDPIY()); A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 5; break; }
        case 0xB2: { auto v = rdM(amDPI());  A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 5; break; }
        case 0xA7: { auto v = rdM(amDPIL()); A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 6; break; }
        case 0xB7: { auto v = rdM(amDPILY()); A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 6; break; }
        case 0xA3: { auto v = rdM(amSR());   A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 4; break; }
        case 0xB3: { auto v = rdM(amSRIY()); A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 7; break; }
        case 0xA2: { X = rdX(amImmX()); nzX(X); cy = fX() ? 2 : 3; break; }
        case 0xA6: { X = rdX(amDP());  nzX(X); cy = 3; break; }
        case 0xB6: { X = rdX(amDPY()); nzX(X); cy = 4; break; }
        case 0xAE: { X = rdX(amAbs()); nzX(X); cy = 4; break; }
        case 0xBE: { X = rdX(amAbY()); nzX(X); cy = 4; break; }
        case 0xA0: { Y = rdX(amImmX()); nzX(Y); cy = fX() ? 2 : 3; break; }
        case 0xA4: { Y = rdX(amDP());  nzX(Y); cy = 3; break; }
        case 0xB4: { Y = rdX(amDPX()); nzX(Y); cy = 4; break; }
        case 0xAC: { Y = rdX(amAbs()); nzX(Y); cy = 4; break; }
        case 0xBC: { Y = rdX(amAbX()); nzX(Y); cy = 4; break; }
        case 0x85: wrM(amDP(),  A); cy = 3; break;
        case 0x95: wrM(amDPX(), A); cy = 4; break;
        case 0x8D: wrM(amAbs(), A); cy = 4; break;
        case 0x9D: wrM(amAbX(), A); cy = 5; break;
        case 0x99: wrM(amAbY(), A); cy = 5; break;
        case 0x8F: wrM(amLng(), A); cy = 5; break;
        case 0x9F: wrM(amLnX(), A); cy = 5; break;
        case 0x81: wrM(amDPIX(), A); cy = 6; break;
        case 0x91: wrM(amDPIY(), A); cy = 6; break;
        case 0x92: wrM(amDPI(),  A); cy = 5; break;
        case 0x87: wrM(amDPIL(), A); cy = 6; break;
        case 0x97: wrM(amDPILY(),A); cy = 6; break;
        case 0x83: wrM(amSR(),   A); cy = 4; break;
        case 0x93: wrM(amSRIY(), A); cy = 7; break;
        case 0x86: wrX(amDP(),  X); cy = 3; break;
        case 0x96: wrX(amDPY(), X); cy = 4; break;
        case 0x8E: wrX(amAbs(), X); cy = 4; break;
        case 0x84: wrX(amDP(),  Y); cy = 3; break;
        case 0x94: wrX(amDPX(), Y); cy = 4; break;
        case 0x8C: wrX(amAbs(), Y); cy = 4; break;
        case 0x64: wrM(amDP(),  0); cy = 3; break;
        case 0x74: wrM(amDPX(), 0); cy = 4; break;
        case 0x9C: wrM(amAbs(), 0); cy = 4; break;
        case 0x9E: wrM(amAbX(), 0); cy = 5; break;
        case 0xAA: X = fX() ? (A & 0xFF) : (A & 0xFFFF); nzX(X); break;
        case 0xA8: Y = fX() ? (A & 0xFF) : (A & 0xFFFF); nzX(Y); break;
        case 0x8A: A = fM() ? (A & 0xFF00) | (X & 0xFF) : (X & 0xFFFF); nzM(A); break;
        case 0x98: A = fM() ? (A & 0xFF00) | (Y & 0xFF) : (Y & 0xFFFF); nzM(A); break;
        case 0xBA: X = fX() ? (SP & 0xFF) : (SP & 0xFFFF); nzX(X); break;
        case 0x9A: SP = E ? (0x100 | (X & 0xFF)) : (X & 0xFFFF); break;
        case 0x9B: Y = X; nzX(Y); break;
        case 0xBB: X = Y; nzX(X); break;
        case 0x5B: DP = A & 0xFFFF; nzX(DP); break;
        case 0x7B: A = DP; nzX(A); break;
        case 0x1B: SP = E ? (0x100 | (A & 0xFF)) : (A & 0xFFFF); break;
        case 0x3B: A = SP; nzX(A); break;
        case 0x48: fM() ? ph8(A & 0xFF) : ph16(A); cy = 3; break;
        case 0xDA: fX() ? ph8(X & 0xFF) : ph16(X); cy = 3; break;
        case 0x5A: fX() ? ph8(Y & 0xFF) : ph16(Y); cy = 3; break;
        case 0x08: ph8(E ? (P | 0x30) : P); cy = 3; break;
        case 0x8B: ph8(DBR); cy = 3; break;
        case 0x0B: ph16(DP); cy = 4; break;
        case 0x4B: ph8(PBR); cy = 3; break;
        case 0xF4: ph16(f16()); cy = 5; break;
        case 0xD4: ph16(r16(0, static_cast<uint16_t>(amDP()))); cy = 6; break;
        case 0x62: { uint16_t o = f16(); ph16((PC + (o < 0x8000 ? o : o - 0x10000)) & 0xFFFF); cy = 6; break; }
        case 0x68: { auto v = fM() ? pl8() : pl16(); A = fM() ? (A & 0xFF00) | (v & 0xFF) : (v & 0xFFFF); nzM(A); cy = 4; break; }
        case 0xFA: { X = fX() ? pl8() : pl16(); nzX(X); cy = 4; break; }
        case 0x7A: { Y = fX() ? pl8() : pl16(); nzX(Y); cy = 4; break; }
        case 0x28: { P = pl8(); if (E) P |= 0x30; if (fX()) { X &= 0xFF; Y &= 0xFF; } cy = 4; break; }
        case 0xAB: { DBR = pl8(); nzX(DBR); cy = 4; break; }
        case 0x2B: { DP = pl16(); nzX(DP); cy = 5; break; }
        case 0x69: adc(rdM(amImmM())); cy = fM() ? 2 : 3; break;
        case 0x65: adc(rdM(amDP()));   cy = 3; break;
        case 0x75: adc(rdM(amDPX()));  cy = 4; break;
        case 0x6D: adc(rdM(amAbs()));  cy = 4; break;
        case 0x7D: adc(rdM(amAbX()));  cy = 4; break;
        case 0x79: adc(rdM(amAbY()));  cy = 4; break;
        case 0x6F: adc(rdM(amLng()));  cy = 5; break;
        case 0x7F: adc(rdM(amLnX()));  cy = 5; break;
        case 0x61: adc(rdM(amDPIX())); cy = 6; break;
        case 0x71: adc(rdM(amDPIY())); cy = 5; break;
        case 0x72: adc(rdM(amDPI()));  cy = 5; break;
        case 0x67: adc(rdM(amDPIL())); cy = 6; break;
        case 0x77: adc(rdM(amDPILY()));cy = 6; break;
        case 0x63: adc(rdM(amSR()));   cy = 4; break;
        case 0x73: adc(rdM(amSRIY())); cy = 7; break;
        case 0xE9: sbc(rdM(amImmM())); cy = fM() ? 2 : 3; break;
        case 0xE5: sbc(rdM(amDP()));   cy = 3; break;
        case 0xF5: sbc(rdM(amDPX()));  cy = 4; break;
        case 0xED: sbc(rdM(amAbs()));  cy = 4; break;
        case 0xFD: sbc(rdM(amAbX()));  cy = 4; break;
        case 0xF9: sbc(rdM(amAbY()));  cy = 4; break;
        case 0xEF: sbc(rdM(amLng()));  cy = 5; break;
        case 0xFF: sbc(rdM(amLnX()));  cy = 5; break;
        case 0xE1: sbc(rdM(amDPIX())); cy = 6; break;
        case 0xF1: sbc(rdM(amDPIY())); cy = 5; break;
        case 0xF2: sbc(rdM(amDPI()));  cy = 5; break;
        case 0xE7: sbc(rdM(amDPIL())); cy = 6; break;
        case 0xF7: sbc(rdM(amDPILY()));cy = 6; break;
        case 0xE3: sbc(rdM(amSR()));   cy = 4; break;
        case 0xF3: sbc(rdM(amSRIY())); cy = 7; break;

        case 0xC9: { auto v = rdM(amImmM()); cy = fM() ? 2 : 3;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xC5: { auto v = rdM(amDP()); cy = 3;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xD5: { auto v = rdM(amDPX()); cy = 4;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xCD: { auto v = rdM(amAbs()); cy = 4;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xDD: { auto v = rdM(amAbX()); cy = 4;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xD9: { auto v = rdM(amAbY()); cy = 4;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xCF: { auto v = rdM(amLng()); cy = 5;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xDF: { auto v = rdM(amLnX()); cy = 5;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xC1: { auto v = rdM(amDPIX()); cy = 6;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xD1: { auto v = rdM(amDPIY()); cy = 5;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xD2: { auto v = rdM(amDPI()); cy = 5;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xC7: { auto v = rdM(amDPIL()); cy = 6;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xD7: { auto v = rdM(amDPILY()); cy = 6;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xC3: { auto v = rdM(amSR()); cy = 4;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xD3: { auto v = rdM(amSRIY()); cy = 7;
            if (fM()) { int32_t r = (A & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (A & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }

        case 0xE0: { auto v = rdX(amImmX()); cy = fX() ? 2 : 3;
            if (fX()) { int32_t r = (X & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (X & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xE4: { auto v = rdX(amDP()); cy = 3;
            if (fX()) { int32_t r = (X & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (X & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xEC: { auto v = rdX(amAbs()); cy = 4;
            if (fX()) { int32_t r = (X & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (X & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xC0: { auto v = rdX(amImmX()); cy = fX() ? 2 : 3;
            if (fX()) { int32_t r = (Y & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (Y & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xC4: { auto v = rdX(amDP()); cy = 3;
            if (fX()) { int32_t r = (Y & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (Y & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }
        case 0xCC: { auto v = rdX(amAbs()); cy = 4;
            if (fX()) { int32_t r = (Y & 0xFF) - (v & 0xFF); sN(r & 0x80); sZ((r & 0xFF) == 0); sC(r >= 0); }
            else { int32_t r = (Y & 0xFFFF) - (v & 0xFFFF); sN(r & 0x8000); sZ((r & 0xFFFF) == 0); sC(r >= 0); }
            break; }

        case 0x29: { auto v = rdM(amImmM()); A = fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = fM() ? 2 : 3; break; }
        case 0x25: { auto v = rdM(amDP());  A = fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = 3; break; }
        case 0x35: { auto v = rdM(amDPX()); A = fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x2D: { auto v = rdM(amAbs()); A = fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x3D: { auto v = rdM(amAbX()); A = fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x39: { auto v = rdM(amAbY()); A = fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x2F: { auto v = rdM(amLng()); A = fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = 5; break; }
        case 0x3F: { auto v = rdM(amLnX()); A = fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = 5; break; }
        case 0x21: { auto v = rdM(amDPIX());A = fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = 6; break; }
        case 0x31: { auto v = rdM(amDPIY());A = fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = 5; break; }
        case 0x32: { auto v = rdM(amDPI()); A = fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = 5; break; }
        case 0x27: { auto v = rdM(amDPIL());A = fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = 6; break; }
        case 0x37: { auto v = rdM(amDPILY());A= fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = 6; break; }
        case 0x23: { auto v = rdM(amSR());  A = fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x33: { auto v = rdM(amSRIY());A = fM() ? (A & 0xFF00) | ((A & v) & 0xFF) : ((A & v) & 0xFFFF); nzM(A); cy = 7; break; }

        case 0x09: { auto v = rdM(amImmM()); A = fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = fM() ? 2 : 3; break; }
        case 0x05: { auto v = rdM(amDP());  A = fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = 3; break; }
        case 0x15: { auto v = rdM(amDPX()); A = fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x0D: { auto v = rdM(amAbs()); A = fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x1D: { auto v = rdM(amAbX()); A = fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x19: { auto v = rdM(amAbY()); A = fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x0F: { auto v = rdM(amLng()); A = fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = 5; break; }
        case 0x1F: { auto v = rdM(amLnX()); A = fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = 5; break; }
        case 0x01: { auto v = rdM(amDPIX());A = fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = 6; break; }
        case 0x11: { auto v = rdM(amDPIY());A = fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = 5; break; }
        case 0x12: { auto v = rdM(amDPI()); A = fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = 5; break; }
        case 0x07: { auto v = rdM(amDPIL());A = fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = 6; break; }
        case 0x17: { auto v = rdM(amDPILY());A= fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = 6; break; }
        case 0x03: { auto v = rdM(amSR());  A = fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x13: { auto v = rdM(amSRIY());A = fM() ? (A & 0xFF00) | ((A | v) & 0xFF) : ((A | v) & 0xFFFF); nzM(A); cy = 7; break; }

        case 0x49: { auto v = rdM(amImmM()); A = fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = fM() ? 2 : 3; break; }
        case 0x45: { auto v = rdM(amDP());  A = fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = 3; break; }
        case 0x55: { auto v = rdM(amDPX()); A = fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x4D: { auto v = rdM(amAbs()); A = fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x5D: { auto v = rdM(amAbX()); A = fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x59: { auto v = rdM(amAbY()); A = fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x4F: { auto v = rdM(amLng()); A = fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = 5; break; }
        case 0x5F: { auto v = rdM(amLnX()); A = fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = 5; break; }
        case 0x41: { auto v = rdM(amDPIX());A = fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = 6; break; }
        case 0x51: { auto v = rdM(amDPIY());A = fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = 5; break; }
        case 0x52: { auto v = rdM(amDPI()); A = fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = 5; break; }
        case 0x47: { auto v = rdM(amDPIL());A = fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = 6; break; }
        case 0x57: { auto v = rdM(amDPILY());A= fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = 6; break; }
        case 0x43: { auto v = rdM(amSR());  A = fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = 4; break; }
        case 0x53: { auto v = rdM(amSRIY());A = fM() ? (A & 0xFF00) | ((A ^ v) & 0xFF) : ((A ^ v) & 0xFFFF); nzM(A); cy = 7; break; }

        case 0x04: { uint32_t a = amDP();  auto v = rdM(a); sZ((A & v) == 0); wrM(a, v | (fM() ? (A & 0xFF) : A)); cy = 5; break; }
        case 0x0C: { uint32_t a = amAbs(); auto v = rdM(a); sZ((A & v) == 0); wrM(a, v | (fM() ? (A & 0xFF) : A)); cy = 6; break; }
        case 0x14: { uint32_t a = amDP();  auto v = rdM(a); sZ((A & v) == 0); wrM(a, (v & ~(fM() ? (A & 0xFF) : A)) & (fM() ? 0xFF : 0xFFFF)); cy = 5; break; }
        case 0x1C: { uint32_t a = amAbs(); auto v = rdM(a); sZ((A & v) == 0); wrM(a, (v & ~(fM() ? (A & 0xFF) : A)) & (fM() ? 0xFF : 0xFFFF)); cy = 6; break; }
        case 0x89: { auto v = rdM(amImmM()); sZ((A & v) == 0); cy = fM() ? 2 : 3; break; }
        case 0x24: { auto v = rdM(amDP());  sN((v & (fM() ? 0x80 : 0x8000)) != 0); sV((v & (fM() ? 0x40 : 0x4000)) != 0); sZ((A & v) == 0); cy = 3; break; }
        case 0x34: { auto v = rdM(amDPX()); sN((v & (fM() ? 0x80 : 0x8000)) != 0); sV((v & (fM() ? 0x40 : 0x4000)) != 0); sZ((A & v) == 0); cy = 4; break; }
        case 0x2C: { auto v = rdM(amAbs()); sN((v & (fM() ? 0x80 : 0x8000)) != 0); sV((v & (fM() ? 0x40 : 0x4000)) != 0); sZ((A & v) == 0); cy = 4; break; }
        case 0x3C: { auto v = rdM(amAbX()); sN((v & (fM() ? 0x80 : 0x8000)) != 0); sV((v & (fM() ? 0x40 : 0x4000)) != 0); sZ((A & v) == 0); cy = 4; break; }

        case 0x0A: asl_reg(); cy = 2; break;
        case 0x06: { uint32_t a = amDP();  asl_mem(a); cy = 5; break; }
        case 0x16: { uint32_t a = amDPX(); asl_mem(a); cy = 6; break; }
        case 0x0E: { uint32_t a = amAbs(); asl_mem(a); cy = 6; break; }
        case 0x1E: { uint32_t a = amAbX(); asl_mem(a); cy = 7; break; }
        case 0x4A: lsr_reg(); cy = 2; break;
        case 0x46: { uint32_t a = amDP();  lsr_mem(a); cy = 5; break; }
        case 0x56: { uint32_t a = amDPX(); lsr_mem(a); cy = 6; break; }
        case 0x4E: { uint32_t a = amAbs(); lsr_mem(a); cy = 6; break; }
        case 0x5E: { uint32_t a = amAbX(); lsr_mem(a); cy = 7; break; }
        case 0x2A: rol_reg(); cy = 2; break;
        case 0x26: { uint32_t a = amDP();  rol_mem(a); cy = 5; break; }
        case 0x36: { uint32_t a = amDPX(); rol_mem(a); cy = 6; break; }
        case 0x2E: { uint32_t a = amAbs(); rol_mem(a); cy = 6; break; }
        case 0x3E: { uint32_t a = amAbX(); rol_mem(a); cy = 7; break; }
        case 0x6A: ror_reg(); cy = 2; break;
        case 0x66: { uint32_t a = amDP();  ror_mem(a); cy = 5; break; }
        case 0x76: { uint32_t a = amDPX(); ror_mem(a); cy = 6; break; }
        case 0x6E: { uint32_t a = amAbs(); ror_mem(a); cy = 6; break; }
        case 0x7E: { uint32_t a = amAbX(); ror_mem(a); cy = 7; break; }

        case 0x1A: { uint32_t v = fM() ? (((A & 0xFF) + 1) & 0xFF) : ((A + 1) & 0xFFFF); A = static_cast<uint16_t>(fM() ? (A & 0xFF00) | v : v); nzM(A); cy = 2; break; }
        case 0x3A: { uint32_t v = fM() ? (((A & 0xFF) - 1) & 0xFF) : ((A - 1) & 0xFFFF); A = static_cast<uint16_t>(fM() ? (A & 0xFF00) | v : v); nzM(A); cy = 2; break; }
        case 0xE6: { uint32_t a = amDP();  uint32_t v = (rdM(a) + 1) & (fM() ? 0xFF : 0xFFFF); wrM(a, static_cast<uint16_t>(v)); nzM(v); cy = 5; break; }
        case 0xF6: { uint32_t a = amDPX(); uint32_t v = (rdM(a) + 1) & (fM() ? 0xFF : 0xFFFF); wrM(a, static_cast<uint16_t>(v)); nzM(v); cy = 6; break; }
        case 0xEE: { uint32_t a = amAbs(); uint32_t v = (rdM(a) + 1) & (fM() ? 0xFF : 0xFFFF); wrM(a, static_cast<uint16_t>(v)); nzM(v); cy = 6; break; }
        case 0xFE: { uint32_t a = amAbX(); uint32_t v = (rdM(a) + 1) & (fM() ? 0xFF : 0xFFFF); wrM(a, static_cast<uint16_t>(v)); nzM(v); cy = 7; break; }
        case 0xC6: { uint32_t a = amDP();  uint32_t v = (rdM(a) - 1) & (fM() ? 0xFF : 0xFFFF); wrM(a, static_cast<uint16_t>(v)); nzM(v); cy = 5; break; }
        case 0xD6: { uint32_t a = amDPX(); uint32_t v = (rdM(a) - 1) & (fM() ? 0xFF : 0xFFFF); wrM(a, static_cast<uint16_t>(v)); nzM(v); cy = 6; break; }
        case 0xCE: { uint32_t a = amAbs(); uint32_t v = (rdM(a) - 1) & (fM() ? 0xFF : 0xFFFF); wrM(a, static_cast<uint16_t>(v)); nzM(v); cy = 6; break; }
        case 0xDE: { uint32_t a = amAbX(); uint32_t v = (rdM(a) - 1) & (fM() ? 0xFF : 0xFFFF); wrM(a, static_cast<uint16_t>(v)); nzM(v); cy = 7; break; }
        case 0xE8: X = (X + 1) & (fX() ? 0xFF : 0xFFFF); nzX(X); break;
        case 0xC8: Y = (Y + 1) & (fX() ? 0xFF : 0xFFFF); nzX(Y); break;
        case 0xCA: X = (X - 1) & (fX() ? 0xFF : 0xFFFF); nzX(X); break;
        case 0x88: Y = (Y - 1) & (fX() ? 0xFF : 0xFFFF); nzX(Y); break;

        case 0x4C: PC = f16(); cy = 3; break;
        case 0x5C: { uint32_t a = f24(); PBR = (a >> 16) & 0xFF; PC = a & 0xFFFF; cy = 4; break; }
        case 0x6C: { uint16_t p = f16(); PC = r16(0, p); cy = 5; break; }
        case 0x7C: { uint16_t p = (f16() + X) & 0xFFFF; PC = r16(PBR, p); cy = 6; break; }
        case 0xDC: { uint16_t p = f16(); uint32_t a = r24(0, p); PBR = (a >> 16) & 0xFF; PC = a & 0xFFFF; cy = 6; break; }
        case 0x20: { uint16_t t = f16(); ph16((PC - 1) & 0xFFFF); PC = t; cy = 6; break; }
        case 0x22: { uint32_t t = f24(); ph8(PBR); ph16((PC - 1) & 0xFFFF); PBR = (t >> 16) & 0xFF; PC = t & 0xFFFF; cy = 8; break; }
        case 0xFC: { uint16_t p = (f16() + X) & 0xFFFF; ph16((PC - 1) & 0xFFFF); PC = r16(PBR, p); cy = 8; break; }
        case 0x60: PC = (pl16() + 1) & 0xFFFF; cy = 6; break;
        case 0x6B: { uint16_t pc = pl16(); PBR = pl8(); PC = (pc + 1) & 0xFFFF; cy = 6; break; }
        case 0x40: {
            P = pl8();
            if (E) P |= 0x30; else if (fX()) { X &= 0xFF; Y &= 0xFF; }
            PC = pl16();
            if (!E) PBR = pl8();
            cy = 6; break;
        }
        case 0x00: {
            f8();
            if (!E) ph8(PBR);
            ph16(PC); ph8(E ? (P | 0x30) : (P | 0x10));
            sI(true); sD(false); PBR = 0;
            PC = r16(0, E ? 0xFFFE : 0xFFE6);
            cy = 8; break;
        }
        case 0x02: {
            f8();
            if (!E) ph8(PBR);
            ph16(PC); ph8(E ? (P | 0x30) : P);
            sI(true); sD(false); PBR = 0;
            PC = r16(0, E ? 0xFFF4 : 0xFFE4);
            cy = 8; break;
        }
        case 0x90: cy = branch(!fC()); break;
        case 0xB0: cy = branch( fC()); break;
        case 0xF0: cy = branch( fZ()); break;
        case 0x30: cy = branch( fN()); break;
        case 0xD0: cy = branch(!fZ()); break;
        case 0x10: cy = branch(!fN()); break;
        case 0x50: cy = branch(!fV()); break;
        case 0x70: cy = branch( fV()); break;
        case 0x80: cy = branch(true); break;
        case 0x82: { uint16_t o = f16(); PC = (PC + (o < 0x8000 ? o : o - 0x10000)) & 0xFFFF; cy = 4; break; }
        case 0x18: sC(false); break;
        case 0x38: sC(true);  break;
        case 0x58: sI(false); break;
        case 0x78: sI(true);  break;
        case 0xD8: sD(false); break;
        case 0xF8: sD(true);  break;
        case 0xB8: sV(false); break;
        case 0xC2: { uint8_t m = f8(); P &= ~m; if (E) P |= 0x30; if (fX()) { X &= 0xFF; Y &= 0xFF; } cy = 3; break; }
        case 0xE2: { uint8_t m = f8(); P |= m; if (fX()) { X &= 0xFF; Y &= 0xFF; } cy = 3; break; }
        case 0xFB: {
            bool oe = E, oc = fC();
            E = oc; sC(oe);
            if (E) { P |= 0x30; SP = 0x100 | (SP & 0xFF); X &= 0xFF; Y &= 0xFF; }
            cy = 2; break;
        }
        case 0x54: {
            uint8_t dst = f8(), src = f8(); DBR = dst;
            wr((static_cast<uint32_t>(dst) << 16) | (Y & 0xFFFF), rd((static_cast<uint32_t>(src) << 16) | (X & 0xFFFF)));
            X = (X + 1) & (fX() ? 0xFF : 0xFFFF);
            Y = (Y + 1) & (fX() ? 0xFF : 0xFFFF);
            A = (A - 1) & 0xFFFF;
            if (A != 0xFFFF) PC = (PC - 3) & 0xFFFF;
            cy = 7; break;
        }
        case 0x44: {
            uint8_t dst = f8(), src = f8(); DBR = dst;
            wr((static_cast<uint32_t>(dst) << 16) | (Y & 0xFFFF), rd((static_cast<uint32_t>(src) << 16) | (X & 0xFFFF)));
            X = (X - 1) & (fX() ? 0xFF : 0xFFFF);
            Y = (Y - 1) & (fX() ? 0xFF : 0xFFFF);
            A = (A - 1) & 0xFFFF;
            if (A != 0xFFFF) PC = (PC - 3) & 0xFFFF;
            cy = 7; break;
        }
        case 0xEA: cy = 2; break;
        case 0x42: f8(); cy = 2; break;
        case 0xCB: waiting = true; cy = 3; break;
        case 0xDB: stopped = true; cy = 3; break;
        default:   cy = 2; break;
    }

    uint32_t fullPc = (static_cast<uint32_t>(PBR) << 16) | PC;
    if (static_cast<int32_t>(fullPc) != _lastTracePc) {
        _lastTracePc = static_cast<int32_t>(fullPc);
        pcHistory.push_back(fullPc);
        if (pcHistory.size() > 24) pcHistory.erase(pcHistory.begin());
    }
    cycles += cy;
    return cy;
}

void CPU65816::shiftOp(uint32_t ea, int kind) {
    bool onReg = (ea == kRegA);
    uint32_t o = onReg ? A : rdM(ea);
    uint32_t v;
    switch (kind) {
        case 0: // ASL
            if (fM()) { sC((o & 0x80) != 0); v = (o << 1) & 0xFF; }
            else      { sC((o & 0x8000) != 0); v = (o << 1) & 0xFFFF; }
            break;
        case 1: // LSR
            sC(o & 1);
            v = fM() ? ((o & 0xFF) >> 1) : (o >> 1);
            break;
        case 2: { // ROL
            int c = fC() ? 1 : 0;
            if (fM()) { sC((o & 0x80) != 0); v = ((o << 1) | c) & 0xFF; }
            else      { sC((o & 0x8000) != 0); v = ((o << 1) | c) & 0xFFFF; }
            break;
        }
        default: { // ROR
            int c = fC() ? 1 : 0;
            if (fM()) { sC(o & 1); v = ((c << 7) | ((o & 0xFF) >> 1)) & 0xFF; }
            else      { sC(o & 1); v = ((c << 15) | (o >> 1)) & 0xFFFF; }
            break;
        }
    }
    if (onReg) { A = static_cast<uint16_t>(fM() ? (A & 0xFF00) | v : v); nzM(A); }
    else       { wrM(ea, static_cast<uint16_t>(v)); nzM(v); }
}

CPU65816::Snapshot CPU65816::snapshot() const {
    return { PC, static_cast<uint16_t>(A), static_cast<uint16_t>(X), static_cast<uint16_t>(Y),
             SP, DP, PBR, DBR, P, E, cycles };
}

} // namespace ding::snes
