// ── DSP.cpp ──────────────────────────────────────────────────────────────────
#include "dsp.h"

#include <algorithm>
#include <cmath>

namespace ding::snes {

namespace {
// DSP register offsets (global, not per-voice)
constexpr int kMVOLL = 0x0C, kMVOLR = 0x1C;
constexpr int kEVOLL = 0x2C, kEVOLR = 0x3C; // echo volume L/R
constexpr int kKON   = 0x4C;
constexpr int kKOFF  = 0x5C;
constexpr int kFLG   = 0x6C;
constexpr int kPMON  = 0x2D;
constexpr int kNON   = 0x3D;
constexpr int kEON   = 0x4D; // echo-enable per voice
constexpr int kENDX  = 0x7C;
constexpr int kDIR   = 0x5D;
constexpr int kEFB   = 0x0D; // echo feedback (signed)
constexpr int kESA   = 0x6D; // echo buffer start (ARAM addr = ESA<<8)
constexpr int kEDL   = 0x7D; // echo delay, 0-15 -> buffer len = EDL*2KB

// Per-voice register offsets, add voice*0x10
constexpr int vVOLL = 0x00, vVOLR = 0x01, vPITCHL = 0x02, vPITCHH = 0x03;
constexpr int vSRCN = 0x04, vADSR1 = 0x05, vADSR2 = 0x06, vGAIN = 0x07;
constexpr uint8_t kAdsrEnable = 0x80; // VxADSR1 bit 7
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

    // KON resets the envelope to 0 and, if ADSR is enabled, starts the
    // Attack phase. GAIN-mode voices (ADSR1 bit7 clear) just sit at 0 until
    // the GAIN follow-up patch lands.
 v.envelope = 0;
    v.envCounter = 0;
    v.envState = Voice::EnvState::Attack;

    // Prime interpolation history so the first few output samples aren't
    // pulled toward zero by stale/zeroed taps.
    v.interpHist[0] = v.interpHist[1] = v.interpHist[2] = v.interpHist[3] = v.decoded[0];

    // Clear this voice's END flag in ENDX on key-on (matches hardware).
    regs[kENDX] &= ~(1 << voice);
}

void DSP::keyOff(int voice) {
    if (voice < 0 || voice >= 8) return;
    // Per S-DSP spec, KOFF moves the voice to the Release envelope state
    // (linear -8/sample) regardless of ADSR-enable/GAIN mode. The voice
    // stays active — and keeps decoding/playing BRR — until tickEnvelope
    // drives its envelope to 0, at which point mixSample deactivates it.
    voices[voice].envState = Voice::EnvState::Release;
}

void DSP::pushInterpHist(Voice& v, int16_t sample) {
    v.interpHist[0] = v.interpHist[1];
    v.interpHist[1] = v.interpHist[2];
    v.interpHist[2] = v.interpHist[3];
    v.interpHist[3] = sample;
}

int16_t DSP::gaussianInterp(const int16_t hist[4], int frac) {
    // Lazily-built 512-entry symmetric Gaussian-shaped kernel, indexed the
    // same way the documented hardware algorithm indexes its ROM LUT (mirror
    // pairs around the center for the 4 taps). This is a smooth analytic
    // approximation, NOT a byte-for-byte copy of the real S-DSP table — see
    // the header comment. To avoid amplitude drift from that imprecision,
    // the 4 tap weights below are normalized against their own sum at
    // runtime rather than trusting a fixed >>11 shift.
    static int16_t table[512];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < 512; i++) {
            double x = (i - 255.5) / 90.0;
            double g = std::exp(-0.5 * x * x);
            table[i] = static_cast<int16_t>(g * 2048.0 + 0.5);
        }
        built = true;
    }

    frac &= 0xFF;
    int32_t c0 = table[255 - frac];
    int32_t c1 = table[511 - frac];
    int32_t c2 = table[256 + frac];
    int32_t c3 = table[frac];
    int32_t sum = c0 + c1 + c2 + c3;
    if (sum <= 0) return hist[3];

    int64_t acc = static_cast<int64_t>(hist[0]) * c0 + static_cast<int64_t>(hist[1]) * c1 +
                  static_cast<int64_t>(hist[2]) * c2 + static_cast<int64_t>(hist[3]) * c3;
    return clamp16(static_cast<int32_t>(acc / sum));
}

void DSP::tickEnvelope(int voiceIdx, Voice& v, int base) {
    // Release overrides everything else, ADSR-enabled or not: linear -8
    // every sample, no rate table involved.
    if (v.envState == Voice::EnvState::Release) {
        v.envelope -= 8;
        if (v.envelope <= 0) { v.envelope = 0; v.active = false; }
        regs[base + vENVX] = static_cast<uint8_t>((v.envelope >> 4) & 0x7F);
        return;
    }

 uint8_t adsr1 = regs[base + vADSR1];
    if (!(adsr1 & kAdsrEnable)) {
        uint8_t gain = regs[base + vGAIN];
        if (!(gain & 0x80)) {
            // Direct/fixed gain: bits 0-6 set the envelope immediately,
            // no rate/counter involved. 7-bit value -> 11-bit envelope
            // (same upper-bits relationship VxENVX exposes).
            v.envelope = (gain & 0x7F) << 4;
        } else {
            int mode = (gain >> 5) & 0x3; // 0=lin dec, 1=exp dec, 2=lin inc, 3=bent-line inc
            int rate = gain & 0x1F;
            bool tick = false;
            if (rate > 0 && kEnvRatePeriod[rate] > 0) {
                if (++v.envCounter >= kEnvRatePeriod[rate]) { v.envCounter = 0; tick = true; }
            }
            if (tick) {
                switch (mode) {
                    case 0: // Linear decrease
                        v.envelope -= 32;
                        break;
                    case 1: // Exponential decrease
                        v.envelope -= ((v.envelope - 1) >> 8) + 1;
                        break;
                    case 2: // Linear increase
                        v.envelope += 32;
                        break;
                    case 3: // Bent-line increase: +32/tick below 0x600, +8/tick above
                        v.envelope += (v.envelope < 0x600) ? 32 : 8;
                        break;
                }
                if (v.envelope < 0) v.envelope = 0;
                if (v.envelope > 0x7FF) v.envelope = 0x7FF;
            }
        }
        regs[base + vENVX] = static_cast<uint8_t>((v.envelope >> 4) & 0x7F);
        return;
    }

    uint8_t adsr2 = regs[base + vADSR2];
    int attackRate = adsr1 & 0x0F;
    int decayRate  = (adsr1 >> 4) & 0x07;
    int sustainLvl = (adsr2 >> 5) & 0x07;
    int sustainRate = adsr2 & 0x1F;

    int rateIndex = 0; // 0 = table entry meaning "never ticks"
    switch (v.envState) {
        case Voice::EnvState::Attack:
            rateIndex = (attackRate == 15) ? 31 : (attackRate * 2 + 1);
            break;
        case Voice::EnvState::Decay:
            rateIndex = 0x10 | (decayRate << 1);
            break;
        case Voice::EnvState::Sustain:
            rateIndex = sustainRate;
            break;
        default: break;
    }

    bool tick = false;
    if (rateIndex > 0 && kEnvRatePeriod[rateIndex] > 0) {
        if (++v.envCounter >= kEnvRatePeriod[rateIndex]) { v.envCounter = 0; tick = true; }
    }

    if (tick) {
        if (v.envState == Voice::EnvState::Attack) {
            v.envelope += (attackRate == 15) ? 1024 : 32;
            if (v.envelope >= 0x7FF) { v.envelope = 0x7FF; v.envState = Voice::EnvState::Decay; }
        } else if (v.envState == Voice::EnvState::Decay) {
            // Exponential decrease: env -= ((env - 1) >> 8) + 1
            v.envelope -= ((v.envelope - 1) >> 8) + 1;
            if (v.envelope < 0) v.envelope = 0;
            if ((v.envelope >> 8) <= sustainLvl) v.envState = Voice::EnvState::Sustain;
        } else if (v.envState == Voice::EnvState::Sustain) {
            v.envelope -= ((v.envelope - 1) >> 8) + 1;
            if (v.envelope < 0) v.envelope = 0;
        }
    }

    regs[base + vENVX] = static_cast<uint8_t>((v.envelope >> 4) & 0x7F);
}

void DSP::tickNoise() {
    int rate = regs[kFLG] & 0x1F;
    if (rate == 0 || kEnvRatePeriod[rate] == 0) return;
    if (++noiseCounter < kEnvRatePeriod[rate]) return;
    noiseCounter = 0;

    // 15-bit Fibonacci LFSR: new top bit = XOR of the two lowest bits,
    // shifted in from the top as the register shifts right.
    uint16_t feedback = static_cast<uint16_t>((noiseLfsr ^ (noiseLfsr >> 1)) & 1) << 14;
    noiseLfsr = static_cast<uint16_t>(((noiseLfsr >> 1) | feedback) & 0x7FFF);
}

int16_t DSP::noiseSample(uint16_t lfsr) {
    // Treat the 15-bit LFSR value as signed (bit14 = sign), then scale up
    // to the same ~16-bit range decoded BRR samples use so it drops into
    // the envelope/VOL math in mixSample without special-casing.
    int32_t signed15 = (lfsr & 0x4000) ? (static_cast<int32_t>(lfsr) - 0x8000) : static_cast<int32_t>(lfsr);
    return clamp16(signed15 * 2);
}

void DSP::tickEcho(int32_t inL, int32_t inR, int32_t& outL, int32_t& outR) {
    uint8_t flg = regs[kFLG];
    uint32_t esa = static_cast<uint32_t>(regs[kESA]) << 8;
    int edl = regs[kEDL] & 0xF;
    // EDL=0 -> 1-sample minimum so the pointer still has a valid slot;
    // real hardware gives a near-zero delay in this case.
    uint32_t lenSamples = edl == 0 ? 1u : static_cast<uint32_t>(edl) * 512u;

    if (echoPos >= lenSamples) echoPos = 0;
    uint32_t addr = (esa + echoPos * 4u) & 0xFFFF;

    int16_t readL = static_cast<int16_t>(rd(addr) | (rd((addr + 1) & 0xFFFF) << 8));
    int16_t readR = static_cast<int16_t>(rd((addr + 2) & 0xFFFF) | (rd((addr + 3) & 0xFFFF) << 8));

    // 8-tap FIR: push this sample's raw (pre-feedback) buffer read into the
    // rolling history, then filter across the last 8 reads. COEF0 weights
    // the newest sample, COEF7 the sample from 7 echo-steps back.
    echoHistPos = (echoHistPos + 1) & 7;
    echoHistL[echoHistPos] = readL;
    echoHistR[echoHistPos] = readR;

    int32_t firL = 0, firR = 0;
    for (int k = 0; k < 8; k++) {
        int8_t coef = static_cast<int8_t>(regs[0x0F + k * 0x10]);
        int idx = (echoHistPos - k) & 7;
        firL += static_cast<int32_t>(echoHistL[idx]) * coef;
        firR += static_cast<int32_t>(echoHistR[idx]) * coef;
    }
int16_t filtL = clamp16(firL >> 7);
    int16_t filtR = clamp16(firR >> 7);

    // Output: echo volume scales the FIR-filtered sample.
    int8_t evolL = static_cast<int8_t>(regs[kEVOLL]);
    int8_t evolR = static_cast<int8_t>(regs[kEVOLR]);
    outL = (static_cast<int32_t>(filtL) * evolL) >> 7;
    outR = (static_cast<int32_t>(filtR) * evolR) >> 7;

    // Write: feedback-scaled FIR output + this sample's EON-enabled voice
    // input, unless FLG bit5 freezes the buffer (reads/output still
    // happen, writes don't).
    if (!(flg & 0x20)) {
        int8_t efb = static_cast<int8_t>(regs[kEFB]);
        int32_t fbL = (static_cast<int32_t>(filtL) * efb) >> 7;
        int32_t fbR = (static_cast<int32_t>(filtR) * efb) >> 7;
        int16_t newL = clamp16(inL + fbL);
        int16_t newR = clamp16(inR + fbR);
        aram[addr]               = static_cast<uint8_t>(newL & 0xFF);
        aram[(addr + 1) & 0xFFFF] = static_cast<uint8_t>((newL >> 8) & 0xFF);
        aram[(addr + 2) & 0xFFFF] = static_cast<uint8_t>(newR & 0xFF);
        aram[(addr + 3) & 0xFFFF] = static_cast<uint8_t>((newR >> 8) & 0xFF);
    }

    echoPos++;
}

void DSP::mixSample(float& outL, float& outR) {
    // KON/KOFF poll — every 2nd sample, matching real hardware. Applying
    // these instantly at write-time (old behavior) meant a driver writing
    // KOFF then KON for the same voice within a sample or two — a common
    // way to retrigger a note — could get silenced or skipped instead of
    // the note simply continuing, since we'd process each edge separately
    // rather than as hardware's coalesced once-per-2-samples snapshot.
    if ((konPollCounter++ & 1) == 0) {
        uint8_t kon = regs[kKON];
        uint8_t koff = regs[kKOFF];
        uint8_t newKon = kon & static_cast<uint8_t>(~lastPolledKon);
        uint8_t newKoff = koff & static_cast<uint8_t>(~lastPolledKoff);
        for (int i = 0; i < 8; i++) {
            if (newKon & (1 << i)) keyOn(i);
            if (newKoff & (1 << i)) keyOff(i);
        }
        lastPolledKon = kon;
        lastPolledKoff = koff;
    }

    uint8_t flg = regs[kFLG];

    // FLG bit7: soft reset. Hardware also resets various internal DSP
    // state on this; we take the audible/practical effect (hard silence,
    // no voice processing this sample) since a byte-for-byte trace of
    // everything real SOFT RESET touches wasn't available to verify against.
    if (flg & 0x80) { outL = 0.0f; outR = 0.0f; return; }

 tickNoise();

    int32_t mixL = 0, mixR = 0;
    int32_t echoInL = 0, echoInR = 0;

    // Previous voice's enveloped output (pre-VOL), used by PMON pitch
    // modulation for the *next* voice in processing order. A silent/inactive
    // voice contributes 0, matching hardware (no modulation from silence).
    int32_t prevVoiceOut = 0;

    for (int i = 0; i < 8; i++) {
        Voice& v = voices[i];
        if (!v.active) { prevVoiceOut = 0; continue; }

 int base = i * 0x10;
        int frac = (v.pitchCounter >> 4) & 0xFF; // sub-sample phase, 0-255
        bool useNoise = (regs[kNON] & (1 << i)) != 0;
        int16_t cur = useNoise ? noiseSample(noiseLfsr) : gaussianInterp(v.interpHist, frac);

        tickEnvelope(i, v, base);
        if (!v.active) { prevVoiceOut = 0; continue; } // Release ramp just hit 0 — drop this sample

        // Envelope (0-0x7FF, 11-bit) scales the sample before VOL, matching
        // "OUTX ... after the envelope has been applied ... before VOL".
        int32_t enveloped = (static_cast<int32_t>(cur) * v.envelope) >> 11;
        int32_t thisVoiceOut = enveloped; // captured before VOL, for next voice's PMON

        int8_t volL = static_cast<int8_t>(regs[base + vVOLL]);
        int8_t volR = static_cast<int8_t>(regs[base + vVOLR]);

 int32_t sL = (enveloped * volL) >> 7;
        int32_t sR = (enveloped * volR) >> 7;
        mixL += sL;
        mixR += sR;
        if (regs[kEON] & (1 << i)) { echoInL += sL; echoInR += sR; }

        regs[base + vOUTX] = static_cast<uint8_t>((enveloped >> 8) & 0xFF);

// Advance playback position. PITCH is a 14-bit value where 0x1000
        // represents "1 source sample per output sample" (i.e. no pitch
        // shift), matching real S-DSP semantics.
        uint16_t pitch = (static_cast<uint16_t>(regs[base + vPITCHH] & 0x3F) << 8) |
                          regs[base + vPITCHL];

        // PMON pitch modulation: voice i (i>0) has its pitch nudged by the
        // previous voice's amplitude this sample, when PMON bit i is set.
        // Classic FM-ish trick (bells, breathy leads) — voice 0 can't be
        // modulated since there's no voice -1.
        uint8_t pmon = regs[kPMON];
        if (i > 0 && (pmon & (1 << i))) {
            int32_t factor = prevVoiceOut >> 5; // scale to a workable modulation range
            int32_t modPitch = static_cast<int32_t>(pitch) +
                                ((static_cast<int32_t>(pitch) * factor) >> 10);
            pitch = static_cast<uint16_t>(std::clamp(modPitch, 0, 0x3FFF));
        }

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
            pushInterpHist(v, v.decoded[v.posInBlock]);
        }

        prevVoiceOut = thisVoiceOut;
    }

 int8_t mvolL = static_cast<int8_t>(regs[kMVOLL]);
    int8_t mvolR = static_cast<int8_t>(regs[kMVOLR]);
    mixL = (mixL * mvolL) >> 7;
    mixR = (mixR * mvolR) >> 7;

    int32_t echoOutL = 0, echoOutR = 0;
    tickEcho(echoInL, echoInR, echoOutL, echoOutR);

    int32_t finalL = std::clamp(std::clamp(mixL, -32768, 32767) + echoOutL, -32768, 32767);
    int32_t finalR = std::clamp(std::clamp(mixR, -32768, 32767) + echoOutR, -32768, 32767);

    outL = finalL / 32768.0f;
    outR = finalR / 32768.0f;

    // FLG bit6: mute. Forces the DAC output to 0 without stopping voice
    // processing — envelopes/noise/BRR playback above all keep running so
    // un-muting mid-note resumes cleanly instead of restarting voices.
    if (flg & 0x40) { outL = 0.0f; outR = 0.0f; }
}

} // namespace ding::snes