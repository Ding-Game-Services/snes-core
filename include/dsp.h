// ── DSP.h ────────────────────────────────────────────────────────────────────
// D!NG SNES core — S-DSP (SPC700's audio mixer/synth chip).
// Milestone A: BRR sample decode + 8-voice playback + flat-gain mixing.
// Milestone B: full ADSR + GAIN envelope path (Attack/Decay/Sustain/
// Release, plus all 4 GAIN modes), Gaussian-shaped 4-tap interpolation
// (dynamically normalized approximation of the hardware LUT — see
// gaussianInterp() in dsp.cpp), PMON pitch modulation, and NON noise
// generation + FLG soft-reset/mute/noise-clock. KOFF ramps via Release
// instead of hard-muting.
// NOT YET IMPLEMENTED: echo/FIR (EVOL/EON/ESA/EDL/EFB, FLG bit5 no-op).
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

// ── ADSR/GAIN envelope (Milestone B) ──────────────────────────────
        // envelope is the 11-bit (0-0x7FF) internal value; VxENVX exposes
        // its upper 7 bits. envState (Attack/Decay/Sustain/Release) only
        // drives playback when VxADSR1 bit7 is set; otherwise VxGAIN's
        // direct/linear/bent-line/exponential modes take over — see
        // DSP::tickEnvelope. Release (from KOFF) overrides either path.
        enum class EnvState { Attack, Decay, Sustain, Release };
        EnvState envState = EnvState::Release;
int      envelope = 0;      // 0-0x7FF
        int      envCounter = 0;    // samples remaining until next envelope tick

        // Last 4 raw decoded samples in playback order (index 3 = most
        // recently reached), used for Gaussian interpolation. Persists
        // across BRR block boundaries same as hist1/hist2 above.
        int16_t interpHist[4] = {0, 0, 0, 0};
    };

    // Anomie's S-DSP rate-to-period table (samples per envelope tick, index
    // 0-31). Index 0 = never ticks. Shared by ADSR attack/decay/sustain and
    // (later) GAIN's non-fixed modes. Public hardware-timing constants, not
    // derived from any emulator's source.
    static constexpr int kEnvRatePeriod[32] = {
        0, 2048, 1536, 1280, 1024, 768, 640, 512,
        384, 320, 256, 192, 160, 128, 96, 80,
        64, 48, 40, 32, 24, 20, 16, 12,
        10, 8, 6, 5, 4, 3, 2, 1
    };

std::array<Voice, 8> voices{};

    // Shared 15-bit noise LFSR (S-DSP has one noise generator, not one per
    // voice — NON just routes it into whichever voices request it).
    // Seeded nonzero: an all-zero LFSR would lock up and never produce
    // noise again.
uint16_t noiseLfsr = 0x4000;
    int      noiseCounter = 0;

    // KON/KOFF ($4C/$5C) are only sampled once every 2 output samples on
    // real hardware (SNESdev Errata, S-SMP section) — writes to those regs
    // just update the byte; actual key-on/key-off happens in mixSample()
    // at the next poll boundary. See mixSample() for why this matters.
    uint8_t lastPolledKon = 0, lastPolledKoff = 0;
    int     konPollCounter = 0;

    std::array<uint8_t, 128>&      regs; // SPC700::dspRegs
    std::array<uint8_t, 0x10000>&  aram; // SPC700::ram (DSP sees the full 64KB address space)

    uint8_t  rd(uint16_t addr) const { return aram[addr & 0xFFFF]; }

 // Decodes the 9-byte BRR block at `addr` into v.decoded[], updating
    // v.hist1/hist2. Returns true if this block's END flag was set.
    bool decodeBrrBlock(Voice& v, uint32_t addr);

// Advances voice `i`'s ADSR envelope by one sample and writes the
    // resulting upper-7-bits value to VxENVX ($x8). Called once per sample
    // from mixSample(), before the envelope is applied to the decoded PCM.
    void tickEnvelope(int voiceIdx, Voice& v, int base);

    // 4-tap interpolation between the last 4 raw decoded samples, using the
    // sub-sample fractional position (0-255) derived from the pitch
    // counter. See dsp.cpp for why this uses a Gaussian-shaped kernel with
    // dynamic per-sample normalization rather than the exact hardware LUT.
 static int16_t gaussianInterp(const int16_t hist[4], int frac);
    static void    pushInterpHist(Voice& v, int16_t sample);

    // Advances the shared noise LFSR by one step when FLG's noise-clock
    // field ($6C bits 0-4, same rate table as envelopes) says it's due.
    // Best-effort reconstruction of the documented S-DSP noise algorithm
    // (15-bit Fibonacci LFSR, taps at bit0/bit1) rather than a verified
    // hardware trace — produces correctly-shaped pseudorandom noise even
    // if the exact bit sequence doesn't match real silicon cycle-for-cycle.
    void tickNoise();

    // Converts the current 15-bit LFSR value to a signed 16-bit sample,
    // same scale as a decoded BRR sample, so it can substitute for `cur`
    // in mixSample without special-casing downstream envelope/VOL math.
    static int16_t noiseSample(uint16_t lfsr);

    // Reads this voice's 4-byte entry from the sample directory (DIR reg
    // $5D * 0x100 + srcn*4) -> {startAddr, loopAddr}.
    void sourceDirEntry(uint8_t srcn, uint16_t& startAddr, uint16_t& loopAddr) const;

static int16_t clamp16(int32_t v);

public:
    // Read-only per-voice snapshot for diagnostics.
    struct VoiceDiag {
        bool active; int envState; int envelope; bool loopFlag; uint32_t blockAddr;
    };
    VoiceDiag voiceDiag(int i) const {
        const Voice& v = voices[i];
        return { v.active, static_cast<int>(v.envState), v.envelope, v.loopFlag, v.blockAddr };
    }
};

} // namespace ding::snes