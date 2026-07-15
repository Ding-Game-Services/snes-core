// ── CPU65816.h ───────────────────────────────────────────────────────────────
// D!NG SNES core — 65816 main CPU.
// Ported from snes-core.js (CPU65816 class). No GPL code.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <vector>

namespace ding::snes {

class Bus;

class CPU65816 {
public:
    explicit CPU65816(Bus& bus);

    void reset();
    int  step(); // executes one instruction, returns cycles consumed

    // ── Registers ──────────────────────────────────────────────────────────
    uint16_t PC = 0;
    uint8_t  PBR = 0, DBR = 0;
    uint16_t SP = 0x01FF;
    uint16_t DP = 0;
    uint16_t A = 0, X = 0, Y = 0;
    uint8_t  P = 0x34;
    bool     E = true; // emulation mode flag

    bool pendingNMI = false, pendingIRQ = false;
    bool stopped = false, waiting = false;
    uint64_t cycles = 0;

    struct TraceEntry { uint8_t bank; uint16_t pc; uint8_t op; };
    std::vector<TraceEntry> pcTrace;      // ring buffer, matches JS pcTrace
    std::vector<uint32_t>   pcHistory;    // ring buffer of (PBR<<16)|PC, matches JS pcHistory

    struct Snapshot {
        uint16_t pc; uint16_t a, x, y, sp, dp;
        uint8_t  pbr, dbr, p; bool e; uint64_t cycles;
    };
    Snapshot snapshot() const;

    // ── Status flag accessors (P register bits) ──────────────────────────
    bool fN() const { return P & 0x80; }
    bool fV() const { return P & 0x40; }
    bool fM() const { return E || (P & 0x20); }
    bool fX() const { return E || (P & 0x10); }
    bool fD() const { return P & 0x08; }
    bool fI() const { return P & 0x04; }
    bool fZ() const { return P & 0x02; }
    bool fC() const { return P & 0x01; }

    void sN(bool v) { v ? P |= 0x80 : P &= ~0x80; }
    void sV(bool v) { v ? P |= 0x40 : P &= ~0x40; }
    void sM(bool v) { v ? P |= 0x20 : P &= ~0x20; }
    void sX(bool v) { v ? P |= 0x10 : P &= ~0x10; }
    void sD(bool v) { v ? P |= 0x08 : P &= ~0x08; }
    void sI(bool v) { v ? P |= 0x04 : P &= ~0x04; }
    void sZ(bool v) { v ? P |= 0x02 : P &= ~0x02; }
    void sC(bool v) { v ? P |= 0x01 : P &= ~0x01; }

    void nzM(uint32_t v);
    void nzX(uint32_t v);

private:
    // ── Memory access ─────────────────────────────────────────────────────
    uint8_t  rd(uint32_t a24);
    void     wr(uint32_t a24, uint8_t v);
    uint8_t  r8(uint8_t bk, uint16_t ad);
    uint16_t r16(uint8_t bk, uint16_t ad);
    uint32_t r24(uint8_t bk, uint16_t ad);
    void     w8(uint8_t bk, uint16_t ad, uint8_t v);
    void     w16(uint8_t bk, uint16_t ad, uint16_t v);

    uint8_t  f8();  // fetch next opcode byte, advances PC
    uint16_t f16();
    uint32_t f24();

    void     ph8(uint8_t v);
    void     ph16(uint16_t v);
    uint8_t  pl8();
    uint16_t pl16();

    uint16_t rdM(uint32_t a);
    void     wrM(uint32_t a, uint16_t v);
    uint16_t rdX(uint32_t a);
    void     wrX(uint32_t a, uint16_t v);

    // ── Addressing modes (return effective 24-bit address) ────────────────
    uint32_t amDP();
    uint32_t amDPX();
    uint32_t amDPY();
    uint32_t amAbs();
    uint32_t amAbX();
    uint32_t amAbY();
    uint32_t amLng();
    uint32_t amLnX();
    uint32_t amSR();
    uint32_t amDPI();
    uint32_t amDPIX();
    uint32_t amDPIY();
    uint32_t amDPIL();
    uint32_t amDPILY();
    uint32_t amSRIY();
    uint32_t amImmM();
    uint32_t amImmX();

    // ── Arithmetic helpers ─────────────────────────────────────────────────
    void adc(uint32_t val);
    void sbc(uint32_t val);

    // Shift/rotate ops. JS used ea===null to mean "operate on register A";
    // kRegA is the equivalent sentinel here since 0xFFFFFF is out of address range.
    static constexpr uint32_t kRegA = 0xFFFFFFFF;
    void shiftOp(uint32_t ea, int kind); // kind: 0=asl 1=lsr 2=rol 3=ror
    void asl_reg() { shiftOp(kRegA, 0); }
    void lsr_reg() { shiftOp(kRegA, 1); }
    void rol_reg() { shiftOp(kRegA, 2); }
    void ror_reg() { shiftOp(kRegA, 3); }
    void asl_mem(uint32_t ea) { shiftOp(ea, 0); }
    void lsr_mem(uint32_t ea) { shiftOp(ea, 1); }
    void rol_mem(uint32_t ea) { shiftOp(ea, 2); }
    void ror_mem(uint32_t ea) { shiftOp(ea, 3); }

    int  branch(bool taken);
    void doNMI();
    void doIRQ();

    int _lastTracedPC = -1;
    int _lastTracePc  = -1;

    Bus& bus;
};

} // namespace ding::snes
