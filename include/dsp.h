// ── DSP.h ────────────────────────────────────────────────────────────────────
// D!NG SNES core — S-DSP (SPC700's audio mixer/synth chip).
// Milestone A: BRR sample decode + 8-voice playback + flat-gain mixing.
// NOT YET IMPLEMENTED (Milestone B): ADSR/GAIN envelopes, Gaussian
// interpolation (currently nearest-neighbor), echo/FIR, noise generator,
// pitch modulation (PMON). KOFF currently hard-mutes instead of releasing.
// No GPL code — BRR decode formulas are from Nintendo's own published S-DSP
// documentation (widely mirrored on hardware reference wikis), not lifted
// from any existing emulator's source.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <array>
#include <cstdint>

namespace ding::snes {

class DSP {
public:
    // regs/aram are owned by SPC700; DSP just reads/writes through references
    // so save-state code keeps working against SPC700's existing members.
    DSP(std::array<uint8_t, 128>& regs, std::array<uint8_t, 0x10000>& aram);

    // Called once per output sample (32000 Hz, matching SPC700::genAudio's
    // existing sample clock). Writes one interleaved L/R pair, range ~[-1,1].
    void mixSample(float& outL, float& outR);

    // Called whenever $4C (KON) is written with a newly-set bit — starts
    // that voice's BRR playback from its source directory entry.
    void keyOn(int voice);

    // Called whenever $5C (KOFF) is written with a newly-set bit.
    // Milestone A: immediately silences the voice (hard cut, no release
    // ramp). Milestone B will replace this with real ADSR release.
    void keyOff(int voice);

private:
    struct Voice {
        bool active = false;

        uint32_t sampleDirEntry = 0; // byte address of this voice's 4-byte dir entry
        uint32_t blockAddr = 0;      // ARAM address of current 9-byte BRR block
        int      posInBlock = 0;     // 0-15, which decoded sample we're on

        int16_t decoded[16] = {};    // current block's 16 decoded PCM samples
        int16_t hist1 = 0, hist2 = 0; // BRR predictor history (persists across blocks)

        uint32_t pitchCounter = 0;   // 12.12-ish fixed point accumulator, see mixSample

        bool loopFlag = false;       // this block is flagged as the loop point
    };

    std::array<Voice, 8> voices{};

    std::array<uint8_t, 128>&      regs; // SPC700::dspRegs
    std::array<uint8_t, 0x10000>&  aram; // SPC700::ram (DSP sees the full 64KB address space)

    uint8_t  rd(uint16_t addr) const { return aram[addr & 0xFFFF]; }

    // Decodes the 9-byte BRR block at `addr` into v.decoded[], updating
    // v.hist1/hist2. Returns true if this block's END flag was set.
    bool decodeBrrBlock(Voice& v, uint32_t addr);

    // Reads this voice's 4-byte entry from the sample directory (DIR reg
    // $5D * 0x100 + srcn*4) -> {startAddr, loopAddr}.
    void sourceDirEntry(uint8_t srcn, uint16_t& startAddr, uint16_t& loopAddr) const;

    static int16_t clamp16(int32_t v);
};

} // namespace ding::snes