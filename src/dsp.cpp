// ── DSP.cpp ──────────────────────────────────────────────────────────────────
#include "DSP.h"

#include <algorithm>
#include <cmath>

namespace ding::snes {

namespace {
// DSP register offsets (global, not per-voice)
constexpr int kMVOLL = 0x0C, kMVOLR = 0x1C;
constexpr int kEVOLL = 0x2C, kEVOLR = 0x3C; // echo — unused until Milestone B
constexpr int kKON   = 0x4C;
constexpr int kKOFF  = 0x5C;
constexpr int kFLG   = 0x6C;
constexpr int kENDX  = 0x7C;
constexpr int kDIR   = 0x5D;
constexpr int kESA   = 0x6D; // echo start — unused until Milestone B
constexpr int kEDL   = 0x7D; // echo delay — unused until Milestone B

// Per-voice register offsets, add voice*0x10
constexpr int vVOLL = 0x00, vVOLR = 0x01, vPITCHL = 0x02, vPITCHH = 0x03;
constexpr int vSRCN = 0x04, vADSR1 = 0x05, vADSR2 = 0x06, vGAIN = 0x07;
constexpr int vENVX = 0x08, vOUTX = 0x09;
}

DSP::DSP(std::array<uint8_t, 128>& regs_, std::array<uint8_t, 0x10000>& aram_)
    : regs(regs_), aram(aram_) {}

int16_t DSP::clamp16(int32_t v) {
    return static_cast<int16_t>(std::clamp(v, -32768, 32767));
}

void DSP::sourceDirEntry(uint8_t srcn, uint16_t& startAddr, uint16_t& loopAddr) const {
    uint32_t base = (static_cast<uint32_t>(regs[kDIR]) << 8) + srcn * 4u;
    startAddr = static_cast<uint16_t>(rd(base) | (rd(base + 1) << 8));
    loopAddr  = static_cast<uint16_t>(rd(base + 2) | (rd(base + 3) << 8));
}

bool DSP::decodeBrrBlock(Voice& v, uint32_t addr) {
    uint8_t header = rd(addr);
    int shift  = (header >> 4) & 0xF;
    int filter = (header >> 2) & 0x3;
    bool loop  = (header & 0x2) != 0;
    bool end   = (header & 0x1) != 0;
    v.loopFlag = loop;

    for (int i = 0; i < 16; i++) {
        uint8_t byte = rd(addr + 1 + (i / 2));
        int nibble = (i & 1) ? (byte & 0xF) : (byte >> 4);
        int32_t s = static_cast<int8_t>(nibble << 4) >> 4; // sign-extend 4-bit -> int

        // Real hardware clamps shift to 12; shifts 13-15 are documented as
        // producing degenerate/near-silent output. We just clamp for safety.
        int useShift = std::min(shift, 12);
        s = (shift <= 12) ? (s << useShift) : ((s < 0) ? -2048 : 0);

        int32_t pred = 0;
        // Prediction formulas per S-DSP filter mode (Nintendo's published
        // BRR spec — integer approximations of the analog gain constants).
        switch (filter) {
            case 0: pred = 0; break;
            case 1: pred = v.hist1 + ((-v.hist1) >> 4); break;
            case 2: pred = v.hist1 * 2 + ((-(v.hist1 * 3)) >> 5) - v.hist2 + (v.hist2 >> 4); break;
            case 3: pred = v.hist1 * 2 + ((-(v.hist1 * 13)) >> 6) - v.hist2 + ((v.hist2 * 3) >> 4); break;
        }

        int16_t sample = clamp16(s + pred);
        v.decoded[i] = sample;
        v.hist2 = v.hist1;
        v.hist1 = sample;
    }
    return end;
}

void DSP::keyOn(int voice) {
    if (voice < 0 || voice >= 8) return;
    Voice& v = voices[voice];
    int base = voice * 0x10;

    uint16_t startAddr, loopAddr;
    sourceDirEntry(regs[base + vSRCN], startAddr, loopAddr);

    v.active = true;
    v.blockAddr = startAddr;
    v.posInBlock = 0;
    v.hist1 = 0;
    v.hist2 = 0;
    v.pitchCounter = 0;
    decodeBrrBlock(v, v.blockAddr);

    // Clear this voice's END flag in ENDX on key-on (matches hardware).
    regs[kENDX] &= ~(1 << voice);
}

void DSP::keyOff(int voice) {
    if (voice < 0 || voice >= 8) return;
    // TODO(Milestone B): real ADSR release ramp instead of a hard cut.
    voices[voice].active = false;
}

void DSP::mixSample(float& outL, float& outR) {
    int32_t mixL = 0, mixR = 0;

    for (int i = 0; i < 8; i++) {
        Voice& v = voices[i];
        if (!v.active) continue;

        int base = i * 0x10;
        int16_t cur = v.decoded[v.posInBlock];

        // TODO(Milestone B): Gaussian 4-tap interpolation instead of
        // nearest-neighbor. Audible as slightly gritty pitch-shifted notes
        // until that lands — correct pitch/timing either way.
        int8_t volL = static_cast<int8_t>(regs[base + vVOLL]);
        int8_t volR = static_cast<int8_t>(regs[base + vVOLR]);

        // TODO(Milestone B): ADSR/GAIN envelope shaping. Flat full-scale
        // playback for now, so expect clicky note-on/off transitions.
        int32_t sL = (cur * volL) >> 7;
        int32_t sR = (cur * volR) >> 7;
        mixL += sL;
        mixR += sR;

        regs[base + vOUTX] = static_cast<uint8_t>((cur >> 8) & 0xFF);

        // Advance playback position. PITCH is a 14-bit value where 0x1000
        // represents "1 source sample per output sample" (i.e. no pitch
        // shift), matching real S-DSP semantics.
        uint16_t pitch = (static_cast<uint16_t>(regs[base + vPITCHH] & 0x3F) << 8) |
                          regs[base + vPITCHL];
        v.pitchCounter += pitch;
        while (v.pitchCounter >= 0x1000) {
            v.pitchCounter -= 0x1000;
            v.posInBlock++;
            if (v.posInBlock >= 16) {
                v.posInBlock = 0;

                // The block we just finished playing is still the one
                // decodeBrrBlock last decoded into v.decoded[]/v.hist —
                // re-read its header here (cheap: 1 byte) to get END/LOOP,
                // since Voice doesn't cache them past decode.
                uint8_t finishedHeader = rd(v.blockAddr);
                bool finishedEnd  = (finishedHeader & 0x1) != 0;
                bool finishedLoop = (finishedHeader & 0x2) != 0;

                if (finishedEnd) {
                    if (finishedLoop) {
                        uint16_t startAddr, loopAddr;
                        sourceDirEntry(regs[base + vSRCN], startAddr, loopAddr);
                        v.blockAddr = loopAddr;
                    } else {
                        v.active = false;
                        regs[kENDX] |= (1 << i);
                        break; // stop advancing a now-inactive voice
                    }
                } else {
                    v.blockAddr = (v.blockAddr + 9) & 0xFFFF;
                }
                decodeBrrBlock(v, v.blockAddr);
            }
        }
    }

    int8_t mvolL = static_cast<int8_t>(regs[kMVOLL]);
    int8_t mvolR = static_cast<int8_t>(regs[kMVOLR]);
    mixL = (mixL * mvolL) >> 7;
    mixR = (mixR * mvolR) >> 7;

    outL = std::clamp(mixL, -32768, 32767) / 32768.0f;
    outR = std::clamp(mixR, -32768, 32767) / 32768.0f;
}

} // namespace ding::snes