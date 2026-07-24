// ── Bus.h ────────────────────────────────────────────────────────────────────
// D!NG SNES core — full 24-bit address decoder.
// $00-$3F / $80-$BF → system area (WRAM mirror, PPU, APU, CPU I/O, cart)
// $40-$7D / $C0-$FF → cartridge ROM/SRAM
// $7E-$7F           → WRAM (128 KB)
// Ported from snes-core.js (Bus class). No GPL code.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ding::snes {

class Cartridge;
class PPU;
class CPU65816;
class SPC700;

struct DmaChannel {
    uint8_t  ctrl         = 0;
    uint8_t  destReg      = 0;
    uint16_t srcAddr      = 0;
    uint8_t  srcBank      = 0;
    uint16_t size         = 0;
    uint16_t tableAddr    = 0;
    uint8_t  tableBank    = 0;
    uint8_t  lineCounter  = 0;
    uint16_t indirectAddr = 0;
    bool     hdmaFinished = false;
};

// Ring-buffer entry for APU port diagnostics (read via Bus::apuLog)
struct ApuLogEntry {
    char     dir  = 'W';   // 'R' or 'W'
    uint8_t  port = 0;
    uint8_t  val  = 0;
    uint8_t  a    = 0;     // CPU A register at time of access
    uint32_t pc   = 0;     // (PBR<<16)|PC of the CPU instruction that caused this access
    int      rep  = 1;
};

class Bus {
public:
    explicit Bus(Cartridge& cart);

    uint8_t read(uint32_t addr24);
    void    write(uint32_t addr24, uint8_t val);

    // Called by PPU at start of VBlank to raise NMI if enabled
    void triggerNMI();

    // HDMA — called once per frame at scanline 0, then per scanline
    void hdmaInit();
    void hdmaRun();

    // Called once per scanline transition from SNES::runFrame
    void checkIRQ(int scanline);

    // Wiring (set by SNES after construction)
    PPU*      ppu = nullptr;
    CPU65816* cpu = nullptr;
    SPC700*   spc = nullptr;

    std::array<uint8_t, 0x20000> wram{}; // 128 KB
    std::array<uint16_t, 2> joypad{};    // auto-read results, per-frame

    // ── Internal CPU registers ($4200–$421F) ──────────────────────────────
    uint8_t nmitimen = 0x00; // $4200
    uint8_t wrio     = 0xFF; // $4201
    uint8_t memsel   = 0x00; // $420D

    uint8_t nmiFlag = 0x00;  // $4210 bit 7
    uint8_t irqFlag = 0x00;  // $4211 bit 7

    uint8_t  wrmpya = 0xFF, wrmpyb = 0xFF;
    uint16_t mpyResult = 0x0000;

    uint8_t  wrdivl = 0xFF, wrdivh = 0xFF, wrdivb = 0xFF;
    uint16_t divResult = 0x0000, modResult = 0x0000;

    uint16_t htime = 0x01FF, vtime = 0x01FF;

    uint32_t wmaddr = 0; // 17-bit WRAM port address

    uint8_t mdmaen = 0, hdmaen = 0;
    std::array<DmaChannel, 8> dmaChannels{};

    std::array<uint8_t, 4> apuOut{}, apuIn{};
    std::vector<ApuLogEntry> apuLog;
    static constexpr size_t kApuLogMax = 2048;

uint8_t openBus = 0;

    // Set by internalRead when a $2140-2143 poll repeats the last-seen value
    // (i.e. the CPU is spin-waiting on the APU, e.g. IPL handshake). SNES::
    // runFrame checks this to prioritize draining the SPC clock debt right
    // away instead of waiting for the next natural drain point. Purely a
    // scheduling nudge — never advances the SPC beyond what spcAcc allows.
    bool spcSyncRequested = false;

    // Serial joypad latch ($4016 strobe, $4016/$4017 serial reads).
    // Public so save-state serialization can reach it directly.
    bool    joyStrobe = false;
    uint8_t joyBit1 = 0, joyBit2 = 0;

private:
    uint8_t internalRead(uint8_t bank, uint16_t addr);
    void    internalWrite(uint8_t bank, uint16_t addr, uint8_t val);
    uint8_t internalRegRead(uint16_t addr);   // $4200–$44FF reads
    void    internalRegWrite(uint16_t addr, uint8_t val); // $4200–$44FF writes
    uint8_t wramPortRead();
    void    wramPortWrite(uint8_t val);
    void    runDMA();

    Cartridge& cart;
};

} // namespace ding::snes
