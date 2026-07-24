// ── ding_core_snes.cpp ─────────────────────────────────────────────────────
// D!NG SNES core — ding_core.h implementation.
//
// This is the ONLY file that talks to the outside world (Hydra, Cockpit,
// Ding Engine, headless test harness). Everything in SNES.h/.cpp etc. is an
// internal implementation detail; consumers only ever see the C API below.
//
// Known gaps, left as TODOs rather than guessed at:
//   - No SPC700 DSP/mixer yet, so audio output is silence. Once the DSP is
//     implemented, wire its output into a DingAudioBuffer here.
//   - Save states cover CPU/Bus/PPU/SPC700 register + memory state, but not
//     yet any co-processor state (SA-1/DSP-1/etc) since those aren't emulated.
//   - ding_set_region() is a no-op — the core only currently runs NTSC timing.
// ─────────────────────────────────────────────────────────────────────────────
#include "ding_core.h"
#include "ding_md5.h"
#include "ding_savestate.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "SNES.h"

using namespace ding::snes;

// ── Global core state ─────────────────────────────────────────────────────
// One core instance per loaded module (.dll/.so/.wasm), per the ding_core.h
// contract — there is no instance handle in the C API.
namespace {

std::unique_ptr<Cartridge> g_cart;
std::unique_ptr<SNES>      g_snes;
std::vector<uint8_t>       g_romFileCopy; // Cartridge doesn't own its input; we do.

DingRomIdentity   g_identity{};
DingSaveStateInfo g_saveInfo{ DING_SAVE_FULL, 0, 0 };
std::string       g_lastError;
bool              g_hasError = false;

constexpr uint32_t kNumPorts = 2;

struct ButtonDef { const char* name; };
constexpr ButtonDef kButtons[12] = {
    {"B"}, {"Y"}, {"Select"}, {"Start"}, {"Up"}, {"Down"}, {"Left"}, {"Right"},
    {"A"}, {"X"}, {"L"}, {"R"},
};
// index i -> bit (15-i) in Bus::joypad[port], matching standard SNES
// controller serial read order (B,Y,Select,Start,Up,Down,Left,Right,A,X,L,R,
// then 4 signature bits which we never set).

void setError(const std::string& msg) {
    g_lastError = msg;
    g_hasError = true;
}

} // namespace

extern "C" {

// ── Lifecycle ────────────────────────────────────────────────────────────
void ding_init() {
    g_cart.reset();
    g_snes.reset();
    g_hasError = false;
    g_lastError.clear();
}

void ding_destroy() {
    g_snes.reset();
    g_cart.reset();
}

void ding_reset() {
    if (!g_cart) return;
    // Rebuild on top of the same cartridge object — cheapest correct way to
    // get back to power-on state without duplicating Cartridge's parsing.
    g_snes = std::make_unique<SNES>(*g_cart);
}

void ding_run_frame() {
    if (!g_snes) return;
    g_snes->runFrame();
}

// ── ROM loading ──────────────────────────────────────────────────────────
DingResult ding_load_rom(const uint8_t* data, size_t len) {
    if (!data || len == 0) { setError("ding_load_rom: null/empty data"); return DING_ERR_BAD_ROM; }

    g_romFileCopy.assign(data, data + len);
    g_cart = std::make_unique<Cartridge>(g_romFileCopy);
    g_snes = std::make_unique<SNES>(*g_cart);

    // ── ROM identity (RetroAchievements SNES rule) ──────────────────────
    // A copier header is present iff file size == 8KB*n + 512. Cartridge
    // already strips it under the same rule, so romBytes() is exactly what
    // RA hashes — no separate header-stripping logic needed here.
    g_identity = DingRomIdentity{};
    g_identity.method = DING_ID_MD5_STRIPPED;
    const auto& bytes = g_cart->romBytes();
    ding_md5(bytes.data(), bytes.size(), g_identity.hash);
    // SNES carts have no serial/disc_id analogue in the header we parse;
    // leave those fields zeroed (display-only, not used for identity).

    g_hasError = false;
    g_lastError.clear();
    return DING_OK;
}

DingResult ding_load_disc(DingDiscImage* /*disc*/) {
    return DING_ERR_NO_DISC; // SNES is cartridge-only
}

DingResult ding_load_bios(uint32_t /*bios_index*/, const uint8_t* /*data*/, size_t /*len*/) {
    return DING_ERR_GENERIC; // no BIOS files declared — see ding_get_bios_count
}

uint8_t ding_is_disc_swap_pending() { return 0; }
void    ding_swap_disc(DingDiscImage* /*disc*/) {}

// ── Capability queries ───────────────────────────────────────────────────
const DingCoreInfo* ding_get_core_info() {
    static const DingCoreInfo info{
        "D!NG SNES Core",
        "Super Nintendo Entertainment System / Super Famicom",
        "0.1.0-dev",
        DING_CORE_API_VERSION_MAJOR,
        DING_CORE_API_VERSION_MINOR,
    };
    return &info;
}

const DingVideoInfo* ding_get_video_info() {
    static const DingVideoInfo info{
        kScreenW, kScreenH, kScreenW, kScreenH,
        DING_PIXFMT_RGBA8,
        0, // dynamic — fixed resolution in this core
    };
    return &info;
}

const DingAudioInfo* ding_get_audio_info() {
    // SPC700 DSP/mixer isn't implemented yet (see file header). Report the
    // real hardware rate so the frontend allocates correctly once audio lands.
    static const DingAudioInfo info{ 32000, 2 };
    return &info;
}

const DingRomIdentity* ding_get_rom_identity() {
    return &g_identity;
}

const DingSaveStateInfo* ding_get_savestate_info() {
    g_saveInfo.method    = DING_SAVE_FULL;
    g_saveInfo.supported = g_snes ? 1 : 0;
    g_saveInfo.max_size  = 0x40000; // generous fixed upper bound; see ding_save_state
    return &g_saveInfo;
}

uint32_t ding_get_memory_region_count() {
    if (!g_snes) return 0;
    uint32_t n = 6; // WRAM, VRAM, CGRAM, OAM, PPU Regs, SPC RAM
    if (!g_cart->sramBytes().empty()) n++;
    return n;
}

void ding_get_memory_region(uint32_t index, DingMemoryRegion* out) {
    if (!out || !g_snes) return;
    std::memset(out, 0, sizeof(DingMemoryRegion));

    switch (index) {
        case 0:
            out->name = "WRAM"; out->base_addr = 0x7E0000;
            out->size = g_snes->bus.wram.size();
            out->access = DING_MEM_DIRECT; out->ptr = g_snes->bus.wram.data();
            out->writable = 1;
            break;
        case 1:
            out->name = "VRAM"; out->base_addr = 0;
            out->size = g_snes->ppu.vram.size();
            out->access = DING_MEM_DIRECT; out->ptr = g_snes->ppu.vram.data();
            out->writable = 1;
            break;
        case 2:
            out->name = "CGRAM"; out->base_addr = 0;
            out->size = g_snes->ppu.cgram.size() * sizeof(uint16_t);
            out->access = DING_MEM_DIRECT;
            out->ptr = reinterpret_cast<uint8_t*>(g_snes->ppu.cgram.data());
            out->writable = 1;
            break;
        case 3:
            out->name = "OAM"; out->base_addr = 0;
            out->size = g_snes->ppu.oam.size();
            out->access = DING_MEM_DIRECT; out->ptr = g_snes->ppu.oam.data();
            out->writable = 1;
            break;
        case 4:
            // Raw $2100-$213F register file — BGnSC/NBA/BGnHOFS etc. live here.
            // Read-only: writing through this pointer skips regWrite()'s side
            // effects (VRAM port latching, CGRAM auto-increment, etc), so it's
            // diagnostic-only, not a real write path.
            out->name = "PPU Regs ($2100-213F)"; out->base_addr = 0x2100;
            out->size = g_snes->ppu.regs.size();
            out->access = DING_MEM_DIRECT; out->ptr = g_snes->ppu.regs.data();
            out->writable = 0;
            break;
case 5:
            out->name = "SPC RAM"; out->base_addr = 0;
            out->size = g_snes->spc.ram.size();
            out->access = DING_MEM_DIRECT; out->ptr = g_snes->spc.ram.data();
            out->writable = 1;
            break;
        case 6:
            if (g_cart && !g_cart->sramBytes().empty()) {
                out->name = "Cart SRAM"; out->base_addr = 0;
                out->size = g_cart->sramBytes().size();
                out->access = DING_MEM_DIRECT; out->ptr = g_cart->sramBytes().data();
                out->writable = 1;
            }
            break;
        default: break;
    }
}

uint32_t ding_get_bios_count() { return 0; } // SNES cartridges need no BIOS
void ding_get_bios_descriptor(uint32_t /*index*/, DingBiosDescriptor* out) {
    if (out) std::memset(out, 0, sizeof(DingBiosDescriptor));
}

uint32_t ding_get_input_descriptor_count() { return kNumPorts * 12; }

void ding_get_input_descriptor(uint32_t index, DingInputDescriptor* out) {
    if (!out || index >= kNumPorts * 12) return;
    uint32_t port = index / 12, btn = index % 12;
    out->name  = kButtons[btn].name;
    out->type  = DING_INPUT_BUTTON;
    out->port  = static_cast<uint8_t>(port);
    out->index = static_cast<uint8_t>(btn);
}

// ── Video output ─────────────────────────────────────────────────────────
const uint8_t* ding_get_framebuffer() {
    if (!g_snes) return nullptr;
    // PPU::pixels is packed 0xFF|B|G|R per uint32; in memory byte order on
    // our x86_64-only targets that's R,G,B,A — i.e. already RGBA8. No copy.
    return reinterpret_cast<const uint8_t*>(g_snes->ppu.pixels.data());
}

void ding_get_current_dimensions(uint32_t* width, uint32_t* height) {
    if (width)  *width  = kScreenW;
    if (height) *height = kScreenH;
}

// ── Audio output (stub — no DSP yet, see file header) ─────────────────────
uint32_t ding_get_audio_sample_count() { return 0; }

uint32_t ding_read_audio_samples(float* buf, uint32_t count) {
    if (buf && count) std::memset(buf, 0, sizeof(float) * count * 2); // silence, stereo
    return 0;
}

// ── Input ────────────────────────────────────────────────────────────────
void ding_set_button(uint8_t port, uint8_t index, uint8_t pressed) {
    if (!g_snes || port >= kNumPorts || index >= 12) return;
    uint16_t bit = 15 - index;
    if (pressed) g_snes->bus.joypad[port] |= (1u << bit);
    else         g_snes->bus.joypad[port] &= ~(1u << bit);
}

void ding_set_axis(uint8_t /*port*/, uint8_t /*index*/, int16_t /*value*/) {
    // Stock SNES controller has no analog inputs.
}

// ── Save states ──────────────────────────────────────────────────────────
namespace {

// Local POD packs for register-file blocks. Only used for round-tripping
// within this file — never exposed, so layout only needs to stay stable
// within one compiled build (not across core versions).
struct CpuRegs {
    uint16_t pc, a, x, y, sp, dp;
    uint8_t  pbr, dbr, p;
    uint8_t  e, pendingNMI, pendingIRQ, stopped, waiting;
    uint64_t cycles;
};
struct BusRegs {
    uint8_t  nmitimen, wrio, memsel, nmiFlag, irqFlag;
    uint8_t  wrmpya, wrmpyb;
    uint16_t mpyResult;
    uint8_t  wrdivl, wrdivh, wrdivb;
    uint16_t divResult, modResult, htime, vtime;
    uint32_t wmaddr;
    uint8_t  mdmaen, hdmaen;
    uint8_t  joyStrobe, joyBit1, joyBit2;
    uint16_t joypad[2];
};
struct PpuRegs {
    uint32_t clk; int32_t scanline; uint8_t vblank, hblank; uint32_t frame;
    uint16_t vramAddr; uint8_t vramInc, vramIncOnHi; uint16_t vramRdBuf;
    uint8_t cgramAddr, cgramBuf, cgramLatch;
    uint16_t oamByteAddr; uint8_t oamLow;
    uint16_t bgH[4], bgV[4];
    uint8_t m7prev, bgPrev;
    int16_t m7a, m7b, m7c, m7d, m7cx, m7cy;
    uint16_t fixedColor;
};
struct SpcRegs {
    uint8_t a, x, y, sp;
    uint16_t pc;
    uint8_t n, v, p, b, h, i, z, c;
    uint8_t inPorts[4], outPorts[4];
    uint8_t timerEn[3]; uint16_t timerDiv[3], timerTarget[3], timerCycles[3];
    uint8_t timerOut[3];
    uint8_t dspRegs[128];
    uint8_t dspAddr, ctrlReg;
    uint64_t cycles;
};

CpuRegs packCpu(const CPU65816& c) {
    return { c.PC, static_cast<uint16_t>(c.A), static_cast<uint16_t>(c.X), static_cast<uint16_t>(c.Y),
             c.SP, c.DP, c.PBR, c.DBR, c.P,
             static_cast<uint8_t>(c.E), static_cast<uint8_t>(c.pendingNMI), static_cast<uint8_t>(c.pendingIRQ),
             static_cast<uint8_t>(c.stopped), static_cast<uint8_t>(c.waiting), c.cycles };
}
void unpackCpu(CPU65816& c, const CpuRegs& r) {
    c.PC = r.pc; c.A = r.a; c.X = r.x; c.Y = r.y; c.SP = r.sp; c.DP = r.dp;
    c.PBR = r.pbr; c.DBR = r.dbr; c.P = r.p;
    c.E = r.e; c.pendingNMI = r.pendingNMI; c.pendingIRQ = r.pendingIRQ;
    c.stopped = r.stopped; c.waiting = r.waiting; c.cycles = r.cycles;
}

BusRegs packBus(const Bus& b) {
    BusRegs r{};
    r.nmitimen = b.nmitimen; r.wrio = b.wrio; r.memsel = b.memsel;
    r.nmiFlag = b.nmiFlag; r.irqFlag = b.irqFlag;
    r.wrmpya = b.wrmpya; r.wrmpyb = b.wrmpyb; r.mpyResult = b.mpyResult;
    r.wrdivl = b.wrdivl; r.wrdivh = b.wrdivh; r.wrdivb = b.wrdivb;
    r.divResult = b.divResult; r.modResult = b.modResult;
    r.htime = b.htime; r.vtime = b.vtime; r.wmaddr = b.wmaddr;
    r.mdmaen = b.mdmaen; r.hdmaen = b.hdmaen;
    r.joyStrobe = b.joyStrobe; r.joyBit1 = b.joyBit1; r.joyBit2 = b.joyBit2;
    r.joypad[0] = b.joypad[0]; r.joypad[1] = b.joypad[1];
    return r;
}
void unpackBus(Bus& b, const BusRegs& r) {
    b.nmitimen = r.nmitimen; b.wrio = r.wrio; b.memsel = r.memsel;
    b.nmiFlag = r.nmiFlag; b.irqFlag = r.irqFlag;
    b.wrmpya = r.wrmpya; b.wrmpyb = r.wrmpyb; b.mpyResult = r.mpyResult;
    b.wrdivl = r.wrdivl; b.wrdivh = r.wrdivh; b.wrdivb = r.wrdivb;
    b.divResult = r.divResult; b.modResult = r.modResult;
    b.htime = r.htime; b.vtime = r.vtime; b.wmaddr = r.wmaddr;
    b.mdmaen = r.mdmaen; b.hdmaen = r.hdmaen;
    b.joyStrobe = r.joyStrobe; b.joyBit1 = r.joyBit1; b.joyBit2 = r.joyBit2;
    b.joypad[0] = r.joypad[0]; b.joypad[1] = r.joypad[1];
}

PpuRegs packPpu(const PPU& p) {
    PpuRegs r{};
    r.clk = p.clk; r.scanline = p.scanline; r.vblank = p.vblank; r.hblank = p.hblank; r.frame = p.frame;
    r.vramAddr = p.vramAddr; r.vramInc = p.vramInc; r.vramIncOnHi = p.vramIncOnHi; r.vramRdBuf = p.vramRdBuf;
    r.cgramAddr = p.cgramAddr; r.cgramBuf = p.cgramBuf; r.cgramLatch = p.cgramLatch;
    r.oamByteAddr = p.oamByteAddr; r.oamLow = p.oamLow;
    for (int i = 0; i < 4; i++) { r.bgH[i] = p.bgH[i]; r.bgV[i] = p.bgV[i]; }
    r.m7prev = p.m7prev; r.bgPrev = p.bgPrev;
    r.m7a = p.m7a; r.m7b = p.m7b; r.m7c = p.m7c; r.m7d = p.m7d; r.m7cx = p.m7cx; r.m7cy = p.m7cy;
    r.fixedColor = p.fixedColor;
    return r;
}
void unpackPpu(PPU& p, const PpuRegs& r) {
    p.clk = r.clk; p.scanline = r.scanline; p.vblank = r.vblank; p.hblank = r.hblank; p.frame = r.frame;
    p.vramAddr = r.vramAddr; p.vramInc = r.vramInc; p.vramIncOnHi = r.vramIncOnHi; p.vramRdBuf = r.vramRdBuf;
    p.cgramAddr = r.cgramAddr; p.cgramBuf = r.cgramBuf; p.cgramLatch = r.cgramLatch;
    p.oamByteAddr = r.oamByteAddr; p.oamLow = r.oamLow;
    for (int i = 0; i < 4; i++) { p.bgH[i] = r.bgH[i]; p.bgV[i] = r.bgV[i]; }
    p.m7prev = r.m7prev; p.bgPrev = r.bgPrev;
    p.m7a = r.m7a; p.m7b = r.m7b; p.m7c = r.m7c; p.m7d = r.m7d; p.m7cx = r.m7cx; p.m7cy = r.m7cy;
    p.fixedColor = r.fixedColor;
}

SpcRegs packSpc(const SPC700& s) {
    SpcRegs r{};
    r.a = s.A; r.x = s.X; r.y = s.Y; r.sp = s.SP; r.pc = s.PC;
    r.n = s.N; r.v = s.V; r.p = s.P; r.b = s.B; r.h = s.H; r.i = s.I; r.z = s.Z; r.c = s.C;
    for (int i = 0; i < 4; i++) { r.inPorts[i] = s.inPorts[i]; r.outPorts[i] = s.outPorts[i]; }
    for (int i = 0; i < 3; i++) {
        r.timerEn[i] = s.timerEn[i]; r.timerDiv[i] = s.timerDiv[i];
        r.timerTarget[i] = s.timerTarget[i]; r.timerCycles[i] = s.timerCycles[i];
        r.timerOut[i] = s.timerOut[i];
    }
    std::memcpy(r.dspRegs, s.dspRegs.data(), s.dspRegs.size());
    r.dspAddr = s.dspAddr; r.ctrlReg = s.ctrlReg; r.cycles = s.cycles;
    return r;
}
void unpackSpc(SPC700& s, const SpcRegs& r) {
    s.A = r.a; s.X = r.x; s.Y = r.y; s.SP = r.sp; s.PC = r.pc;
    s.N = r.n; s.V = r.v; s.P = r.p; s.B = r.b; s.H = r.h; s.I = r.i; s.Z = r.z; s.C = r.c;
    for (int i = 0; i < 4; i++) { s.inPorts[i] = r.inPorts[i]; s.outPorts[i] = r.outPorts[i]; }
    for (int i = 0; i < 3; i++) {
        s.timerEn[i] = r.timerEn[i]; s.timerDiv[i] = r.timerDiv[i];
        s.timerTarget[i] = r.timerTarget[i]; s.timerCycles[i] = r.timerCycles[i];
        s.timerOut[i] = r.timerOut[i];
    }
    std::memcpy(s.dspRegs.data(), r.dspRegs, s.dspRegs.size());
    s.dspAddr = r.dspAddr; s.ctrlReg = r.ctrlReg; s.cycles = r.cycles;
}

} // namespace

size_t ding_save_state(uint8_t* buf, size_t buf_size) {
    if (!g_snes || !buf) return 0;

    DingSaveWriter w{};
    if (ding_save_writer_init(&w, buf, buf_size, "Super Nintendo Entertainment System") != DING_SS_OK)
        return 0;

    CpuRegs cpu = packCpu(g_snes->cpu);
    BusRegs bus = packBus(g_snes->bus);
    PpuRegs ppu = packPpu(g_snes->ppu);
    SpcRegs spc = packSpc(g_snes->spc);

    bool ok = true;
    ok &= ding_save_write_block(&w, "CPU",    &cpu, sizeof(cpu)) == DING_SS_OK;
    ok &= ding_save_write_block(&w, "BUS",    &bus, sizeof(bus)) == DING_SS_OK;
    ok &= ding_save_write_block(&w, "PPU",    &ppu, sizeof(ppu)) == DING_SS_OK;
    ok &= ding_save_write_block(&w, "SPC",    &spc, sizeof(spc)) == DING_SS_OK;
    ok &= ding_save_write_block(&w, "WRAM",   g_snes->bus.wram.data(), g_snes->bus.wram.size()) == DING_SS_OK;
    ok &= ding_save_write_block(&w, "VRAM",   g_snes->ppu.vram.data(), g_snes->ppu.vram.size()) == DING_SS_OK;
    ok &= ding_save_write_block(&w, "CGRAM",  g_snes->ppu.cgram.data(), g_snes->ppu.cgram.size() * sizeof(uint16_t)) == DING_SS_OK;
    ok &= ding_save_write_block(&w, "OAM",    g_snes->ppu.oam.data(), g_snes->ppu.oam.size()) == DING_SS_OK;
    ok &= ding_save_write_block(&w, "SPCRAM", g_snes->spc.ram.data(), g_snes->spc.ram.size()) == DING_SS_OK;
    ok &= ding_save_write_block(&w, "DMACH",  g_snes->bus.dmaChannels.data(),
                                 g_snes->bus.dmaChannels.size() * sizeof(DmaChannel)) == DING_SS_OK;
    if (g_cart && !g_cart->sramBytes().empty())
        ok &= ding_save_write_block(&w, "SRAM", g_cart->sramBytes().data(), g_cart->sramBytes().size()) == DING_SS_OK;

    if (!ok) { setError("ding_save_state: block write failed (buffer too small?)"); return 0; }

    size_t outSize = 0;
    if (ding_save_writer_finish(&w, &outSize) != DING_SS_OK) {
        setError("ding_save_state: writer_finish failed");
        return 0;
    }
    return outSize;
}

DingResult ding_load_state(const uint8_t* buf, size_t len) {
    if (!g_snes || !buf) return DING_ERR_BAD_STATE;

    DingSaveReader r{};
    if (ding_save_reader_init(&r, buf, len) != DING_SS_OK) {
        setError("ding_load_state: invalid .ding buffer");
        return DING_ERR_BAD_STATE;
    }

    CpuRegs cpu{}; BusRegs bus{}; PpuRegs ppu{}; SpcRegs spc{};
    ding_save_read_block(&r, "CPU", &cpu, sizeof(cpu), nullptr);
    ding_save_read_block(&r, "BUS", &bus, sizeof(bus), nullptr);
    ding_save_read_block(&r, "PPU", &ppu, sizeof(ppu), nullptr);
    ding_save_read_block(&r, "SPC", &spc, sizeof(spc), nullptr);
    unpackCpu(g_snes->cpu, cpu);
    unpackBus(g_snes->bus, bus);
    unpackPpu(g_snes->ppu, ppu);
    unpackSpc(g_snes->spc, spc);

    ding_save_read_block(&r, "WRAM",   g_snes->bus.wram.data(), g_snes->bus.wram.size(), nullptr);
    ding_save_read_block(&r, "VRAM",   g_snes->ppu.vram.data(), g_snes->ppu.vram.size(), nullptr);
    ding_save_read_block(&r, "CGRAM",  g_snes->ppu.cgram.data(), g_snes->ppu.cgram.size() * sizeof(uint16_t), nullptr);
    ding_save_read_block(&r, "OAM",    g_snes->ppu.oam.data(), g_snes->ppu.oam.size(), nullptr);
    ding_save_read_block(&r, "SPCRAM", g_snes->spc.ram.data(), g_snes->spc.ram.size(), nullptr);
    ding_save_read_block(&r, "DMACH",  g_snes->bus.dmaChannels.data(),
                          g_snes->bus.dmaChannels.size() * sizeof(DmaChannel), nullptr);
    if (g_cart && !g_cart->sramBytes().empty())
        ding_save_read_block(&r, "SRAM", g_cart->sramBytes().data(), g_cart->sramBytes().size(), nullptr);

    return DING_OK;
}

// ── Region ───────────────────────────────────────────────────────────────
void ding_set_region(const char* /*region*/) {
    // TODO: no PAL timing / 50Hz path implemented yet — NTSC only for now.
}

// ── Diagnostics ──────────────────────────────────────────────────────────
size_t ding_diag_cpu_state(char* buf, size_t buf_size) {
    if (!buf || buf_size == 0 || !g_snes) return 0;
    const CPU65816& c = g_snes->cpu;
    int n = std::snprintf(buf, buf_size,
        "PC=%02X:%04X A=%04X X=%04X Y=%04X SP=%04X DP=%04X DBR=%02X P=%02X E=%d "
        "NMI=%d IRQ=%d STOP=%d WAIT=%d cyc=%llu",
        c.PBR, c.PC, c.A, c.X, c.Y, c.SP, c.DP, c.DBR, c.P, c.E ? 1 : 0,
        c.pendingNMI ? 1 : 0, c.pendingIRQ ? 1 : 0, c.stopped ? 1 : 0, c.waiting ? 1 : 0,
        static_cast<unsigned long long>(c.cycles));
    if (n < 0) return 0;
    size_t used = static_cast<size_t>(n);

    // Append the last few distinct PCs so a frozen/looping CPU is visible
    // directly in diag output instead of requiring step-by-step guesswork.
    if (!c.pcTrace.empty() && used < buf_size) {
        int written = std::snprintf(buf + used, buf_size - used, " | trace:");
        if (written > 0) used += static_cast<size_t>(written);
        size_t start = c.pcTrace.size() > 8 ? c.pcTrace.size() - 8 : 0;
        for (size_t i = start; i < c.pcTrace.size() && used < buf_size; i++) {
            const auto& t = c.pcTrace[i];
            written = std::snprintf(buf + used, buf_size - used, " %02X:%04X(%02X)", t.bank, t.pc, t.op);
            if (written > 0) used += static_cast<size_t>(written);
        }
    }
    return used < buf_size ? used : buf_size - 1;
}

size_t ding_diag_video_state(char* buf, size_t buf_size) {
    if (!buf || buf_size == 0 || !g_snes) return 0;
    const PPU& p = g_snes->ppu;
    int n = std::snprintf(buf, buf_size,
        "scanline=%d frame=%u vblank=%d mode=%d tm=%02X ts=%02X inidisp=%02X",
        p.scanline, p.frame, p.vblank ? 1 : 0, p.regs[0x05] & 7, p.regs[0x2C], p.regs[0x2D], p.regs[0x00]);
    return n < 0 ? 0 : static_cast<size_t>(n);
}

size_t ding_diag_apu_state(char* buf, size_t buf_size) {
    if (!buf || buf_size == 0 || !g_snes) return 0;
    const SPC700& s = g_snes->spc;
    const Bus& b = g_snes->bus;
    size_t used = 0;
int n = std::snprintf(buf, buf_size,
        "spcPC=%04X A=%02X X=%02X Y=%02X SP=%02X cyc=%llu | out=%02X,%02X,%02X,%02X in=%02X,%02X,%02X,%02X",
        s.PC, s.A, s.X, s.Y, s.SP, static_cast<unsigned long long>(s.cycles),
        s.outPorts[0], s.outPorts[1], s.outPorts[2], s.outPorts[3],
        s.inPorts[0], s.inPorts[1], s.inPorts[2], s.inPorts[3]);
    if (n > 0) used = static_cast<size_t>(n);

    int written = std::snprintf(buf + used, buf_size - used, " | pcTrace:");
    if (written > 0) used += static_cast<size_t>(written);
    for (size_t i = 0; i < s.pcTrace.size() && used < buf_size; i++) {
        written = std::snprintf(buf + used, buf_size - used, " %04X", s.pcTrace[i]);
        if (written > 0) used += static_cast<size_t>(written);
    }

    written = std::snprintf(buf + used, buf_size - used, " | log:");
    if (written > 0) used += static_cast<size_t>(written);

    size_t start = b.apuLog.size() > 20 ? b.apuLog.size() - 20 : 0;
    for (size_t i = start; i < b.apuLog.size() && used < buf_size; i++) {
        const auto& e = b.apuLog[i];
written = std::snprintf(buf + used, buf_size - used, " %c%d=%02X(A=%02X,pc=%06X,x%d)",
            e.dir, e.port, e.val, e.a, e.pc, e.rep);
        if (written > 0) used += static_cast<size_t>(written);
    }
    return used < buf_size ? used : buf_size - 1;
}

uint8_t ding_has_error() { return g_hasError ? 1 : 0; }

const char* ding_diag_last_error() {
    return g_hasError ? g_lastError.c_str() : nullptr;
}

} // extern "C"
