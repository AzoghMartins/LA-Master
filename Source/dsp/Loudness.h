#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "DspUtils.h"

namespace lam
{
constexpr float kSilenceLufs = -100.0f;

/** ITU-R BS.1770 K-weighting filter (pre-filter shelf + RLB high-pass), any sample rate. */
class KWeighting
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept { shelf.reset(); hp.reset(); }
    inline float process (float x) noexcept { return hp.process (shelf.process (x)); }

private:
    Biquad shelf, hp;
};

/**
    Loudness meter following EBU R128 / ITU-R BS.1770-4 for a stereo signal:
    momentary (400 ms), short-term (3 s), gated integrated loudness and loudness range (LRA).
    Audio is analysed in 100 ms hops; call samplesToNextHop() to split processing at hop boundaries.
*/
class LoudnessMeter
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    /** Clears integrated loudness and loudness range only. */
    void resetIntegrated() noexcept;

    /** Returns true when a 100 ms hop completed inside this call (only the last one is reported). */
    bool process (const float* l, const float* r, int n) noexcept;
    int samplesToNextHop() const noexcept { return hopSize - hopCount; }

    float getMomentary() const noexcept { return momentary; }
    float getShortTerm() const noexcept { return shortTerm; }
    float getIntegrated() const noexcept { return integrated; }
    float getRange() const noexcept { return range; }
    double getSecondsMeasured() const noexcept { return (double) hops * 0.1; }

    static float powerToLufs (double p) noexcept { return p <= 1.0e-12 ? kSilenceLufs : (float) (-0.691 + 10.0 * std::log10 (p)); }

private:
    void finishHop() noexcept;
    void updateIntegrated() noexcept;

    static constexpr int kBins = 900;            // 0.1 LU bins from -70 to +20 LUFS
    static constexpr float kBinFloor = -70.0f;

    KWeighting kl, kr;
    int hopSize = 4800, hopCount = 0;
    double acc = 0.0;
    std::array<double, 30> sub {};
    int subPos = 0;
    int64_t hops = 0;

    std::vector<double> blockEnergy;   // integrated: energy sum per bin
    std::vector<int64_t> blockCount;   // integrated: blocks per bin
    std::vector<int64_t> stCount;      // LRA: short-term values per bin
    int64_t stTotal = 0;

    float momentary = kSilenceLufs, shortTerm = kSilenceLufs, integrated = kSilenceLufs, range = 0.0f;
};

/** Inter-sample (true) peak estimate with 4x polyphase oversampling, as in BS.1770 annex 2. */
class TruePeakDetector
{
public:
    TruePeakDetector();
    void reset() noexcept { hist.fill (0.0f); pos = 0; }
    /** Feeds one sample and returns the largest absolute value of the 4 interpolated points. */
    inline float process (float x) noexcept
    {
        hist[(size_t) pos] = x;
        hist[(size_t) (pos + kTaps)] = x;
        pos = (pos + 1) % kTaps;
        const float* h = &hist[(size_t) pos]; // oldest .. newest
        float peak = 0.0f;
        for (int p = 0; p < 4; ++p)
        {
            float acc = 0.0f;
            const float* c = coeffs[(size_t) p].data();
            for (int k = 0; k < kTaps; ++k)
                acc += c[k] * h[k];
            peak = std::max (peak, std::abs (acc));
        }
        return peak;
    }
    /** Delay (in samples) between the input and the detected peak positions. */
    static constexpr int delay() { return kTaps / 2; }

private:
    static constexpr int kTaps = 12;
    std::array<std::array<float, kTaps>, 4> coeffs {};
    std::array<float, kTaps * 2> hist {};
    int pos = 0;
};

/** Octave bands used for the tonal balance (Stabilizer, Assistant, reference matching). */
constexpr int kNumBands = 10;
inline constexpr std::array<float, kNumBands> kBandFreqs { 31.5f, 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f };
using BandCurve = std::array<float, kNumBands>;

/** Accumulates the long-term energy of a signal in octave bands (offline analysis). */
class BandAnalyzer
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    void process (const float* l, const float* r, int n) noexcept;
    /** Band levels in dB (relative to the band average when `normalise`). */
    BandCurve getLevels (bool normalise) const noexcept;
    bool hasData() const noexcept { return samples > 0; }

private:
    double sr = 48000.0;
    std::array<std::array<Biquad, 2>, kNumBands> filters {};
    std::array<double, kNumBands> energy {};
    int64_t samples = 0;
};

/** Removes the mean of the middle bands (125 Hz - 4 kHz) so curves can be compared by shape. */
BandCurve normaliseCurve (const BandCurve& c) noexcept;
} // namespace lam
