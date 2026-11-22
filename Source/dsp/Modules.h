#pragma once

#include <array>
#include <vector>

#include "DspUtils.h"
#include "Loudness.h"

namespace lam
{
/** Which part of the stereo signal a band works on. */
enum class ChannelMode { stereo = 0, mid, side };

//==============================================================================
constexpr int kEqBands = 6;

struct EqParams
{
    bool enabled = true;
    float hpfHz = 10.0f;    // <= 10.5 = off
    float lpfHz = 22000.0f; // >= 21500 = off
    std::array<float, kEqBands> freq { 60.0f, 180.0f, 500.0f, 1800.0f, 5000.0f, 12000.0f };
    std::array<float, kEqBands> gain {};
    std::array<float, kEqBands> q { 0.7f, 0.8f, 0.8f, 0.8f, 0.8f, 0.7f };
    std::array<int, kEqBands> mode {};
};

/** Six band mastering EQ (low shelf, 4 bells, high shelf), each band stereo / mid / side, plus 24 dB/oct cuts. */
class EqModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const EqParams& p) noexcept;
    void process (float* l, float* r, int n) noexcept;

    /** Designs band `band` (0 = low shelf, last = high shelf, others bells). Shelves use `q` as slope. */
    static void design (Biquad& b, double sr, int band, float freq, float gainDb, float q) noexcept;

private:
    double sr = 48000.0;
    EqParams params;
    bool first = true;
    std::array<std::array<Biquad, 2>, kEqBands> filters {};
    std::array<bool, kEqBands> active {};
    std::array<Filter24, 2> hpf, lpf;
    bool hpfOn = false, lpfOn = false;
};

//==============================================================================
constexpr int kDynEqBands = 4;

struct DynEqParams
{
    bool enabled = false;
    float attackMs = 5.0f, releaseMs = 120.0f;
    std::array<float, kDynEqBands> freq { 120.0f, 400.0f, 3000.0f, 7500.0f };
    std::array<float, kDynEqBands> q { 1.2f, 1.2f, 1.5f, 2.0f };
    std::array<float, kDynEqBands> threshold { -30.0f, -30.0f, -30.0f, -36.0f };
    std::array<float, kDynEqBands> range {};       // dB applied when the band is above threshold (0 = band off)
    std::array<int, kDynEqBands> mode {};
};

/** Dynamic EQ: each band cuts (or boosts) only while its own frequency range is above the threshold. */
class DynamicEqModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const DynEqParams& p) noexcept;
    void process (float* l, float* r, int n) noexcept;
    /** Current dynamic gain of each band in dB. */
    const std::array<float, kDynEqBands>& getGains() const noexcept { return gainDb; }

private:
    double sr = 48000.0;
    DynEqParams params;
    std::array<std::array<Biquad, 2>, kDynEqBands> detect {}, filters {};
    std::array<float, kDynEqBands> env {}, gainDb {}, appliedDb {};
    float att = 0.0f, rel = 0.0f;
    int counter = 0;
    bool needDesign = true;
};

//==============================================================================
struct StabilizerParams
{
    bool enabled = false;
    float amount = 0.5f;      // 0..1
    float speedMs = 1500.0f;  // analysis time constant
    float tilt = 0.0f;        // dB per octave around 1 kHz added to the target
    BandCurve target {};      // normalised target band curve (dB)
};

/** Adaptive tonal balance: continuously steers the octave-band balance towards a target curve. */
class StabilizerModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const StabilizerParams& p) noexcept;
    void process (float* l, float* r, int n) noexcept;
    const BandCurve& getGains() const noexcept { return gains; }
    static constexpr float maxBoost = 4.0f, maxCut = 6.0f;

private:
    void updateFilters() noexcept;

    double sr = 48000.0;
    StabilizerParams params;
    std::array<std::array<Biquad, 2>, kNumBands> analysis {};
    std::array<std::array<Biquad, 2>, kNumBands> eq {};
    std::array<double, kNumBands> power {};
    BandCurve gains {}, applied {};
    float coeff = 0.0f, gainCoeff = 0.0f;
    int counter = 0;
};

//==============================================================================
struct GlueParams
{
    bool enabled = false;
    float thresholdDb = -18.0f;
    float ratio = 2.0f;
    float attackMs = 10.0f;
    float releaseMs = -1.0f; // <= 0 = auto
    float makeupDb = 0.0f;
    float scHpfHz = 60.0f;   // <= 20.5 = off
    float mix = 1.0f;
};

/** Stereo-linked VCA bus compressor ("glue") with side-chain high-pass and auto release. */
class GlueModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const GlueParams& p) noexcept;
    void process (float* l, float* r, int n) noexcept;
    float getReductionDb() const noexcept { return grDb; }

private:
    double sr = 48000.0;
    GlueParams params;
    std::array<Biquad, 2> scHp {};
    float grFast = 0.0f, grSlow = 0.0f, grDb = 0.0f;
    float att = 0.0f, rel = 0.0f, slowUp = 0.0f, slowDown = 0.0f;
    Smoothed makeup, mix;
};

//==============================================================================
constexpr int kMbBands = 3;

struct MultibandParams
{
    bool enabled = false;
    float lowMidHz = 150.0f, midHighHz = 3000.0f;
    std::array<float, kMbBands> threshold { -20.0f, -20.0f, -20.0f };
    std::array<float, kMbBands> ratio { 2.0f, 2.0f, 2.0f };
    std::array<float, kMbBands> attackMs { 30.0f, 15.0f, 5.0f };
    std::array<float, kMbBands> releaseMs { 200.0f, 140.0f, 90.0f };
    std::array<float, kMbBands> gainDb {};
    std::array<bool, kMbBands> solo {};
    float mix = 1.0f;
};

/** Three band compressor with Linkwitz-Riley crossovers, band solo and parallel mix. */
class MultibandModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const MultibandParams& p) noexcept;
    void process (float* l, float* r, int n) noexcept;
    const std::array<float, kMbBands>& getReductionDb() const noexcept { return grOut; }

    /** Static gain reduction (dB, >= 0) of a soft-knee compressor. */
    static float staticReduction (float levelDb, float thresholdDb, float ratio, float kneeDb) noexcept;

private:
    double sr = 48000.0;
    MultibandParams params;
    std::array<ThreeBandSplitter, 2> split;
    std::array<float, kMbBands> gr {}, grOut {}, att {}, rel {};
    std::array<Smoothed, kMbBands> bandGain;
    Smoothed mix;
    float lastLow = -1.0f, lastHigh = -1.0f;
};

//==============================================================================
enum class ExciterMode { tape = 0, tube, warm, triode, retro, numModes };

struct ExciterParams
{
    bool enabled = false;
    ExciterMode mode = ExciterMode::tape;
    float lowMidHz = 200.0f, midHighHz = 4000.0f;
    std::array<float, kMbBands> driveDb { 0.0f, 3.0f, 6.0f };
    std::array<float, kMbBands> mix { 0.5f, 0.5f, 0.5f };
};

/** Multiband harmonic exciter, 2x oversampled. Always adds the same latency (on or off). */
class ExciterModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const ExciterParams& p) noexcept;
    void process (float* l, float* r, int n) noexcept;
    static constexpr int getLatency() { return Oversampler2x::latency(); }
    static float shape (ExciterMode mode, float x) noexcept;

private:
    double sr = 48000.0;
    ExciterParams params;
    std::array<Oversampler2x, 2> os;
    std::array<ThreeBandSplitter, 2> split;
    std::array<LatencyDelay, 2> bypassDelay;
    std::array<Biquad, 2> dcBlock {};
    std::array<Smoothed, kMbBands> drive, mix;
    float lastLow = -1.0f, lastHigh = -1.0f;
};

//==============================================================================
struct ImagerParams
{
    bool enabled = false;
    float lowMidHz = 250.0f, midHighHz = 4000.0f;
    std::array<float, kMbBands> width { 1.0f, 1.0f, 1.0f }; // 0 = mono, 1 = unchanged, 2 = double side
    float stereoize = 0.0f;   // 0..1
    float monoBassHz = 20.0f; // <= 20.5 = off
};

/** Multiband stereo width, mono-bass (phase coherent) and a mono-compatible stereoizer. */
class ImagerModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const ImagerParams& p) noexcept;
    void process (float* l, float* r, int n) noexcept;

private:
    double sr = 48000.0;
    ImagerParams params;
    std::array<ThreeBandSplitter, 2> split;
    std::array<Smoothed, kMbBands> width;
    Smoothed stereoize;
    Biquad monoLp1, monoLp2, monoApM, monoApS, stHp;
    DelayLine stDelay;
    float lastLow = -1.0f, lastHigh = -1.0f, lastMono = -1.0f;
};

//==============================================================================
enum class MaxMode { transparent = 0, balanced, loud, clip, numModes };

struct MaximizerParams
{
    bool enabled = true;
    float gainDb = 0.0f;
    float ceilingDb = -1.0f;
    float releaseMs = 120.0f;
    MaxMode mode = MaxMode::balanced;
    bool truePeak = true;
    float link = 1.0f; // stereo link 0..1
};

/** Monotonic-deque sliding window minimum (O(1) per sample). */
class SlidingMin
{
public:
    void prepare (int windowLength);
    void reset() noexcept;
    float push (float v) noexcept;

private:
    std::vector<float> vals;
    std::vector<int64_t> idx;
    int head = 0, tail = 0, size = 0, cap = 1;
    int64_t t = 0;
};

/**
    Look-ahead brickwall maximizer with optional true-peak detection, a 2x oversampled soft clipper
    (Loud / Clip modes) and per-mode release behaviour. Latency is constant.
*/
class MaximizerModule
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (const MaximizerParams& p) noexcept;
    void process (float* l, float* r, int n) noexcept;
    int getLatency() const noexcept { return Oversampler2x::latency() + lookahead + TruePeakDetector::delay(); }
    float getReductionDb() const noexcept { return grDb; }

private:
    float clipSample (float x, float threshold, float kneeStart) const noexcept;

    double sr = 48000.0;
    MaximizerParams params;
    int lookahead = 96;
    std::array<Oversampler2x, 2> os;
    std::array<LatencyDelay, 2> clipBypass;
    std::array<TruePeakDetector, 2> tp;
    std::array<DelayLine, 2> peakDelay, audioDelay;
    std::array<SlidingMin, 2> windowMin;
    std::array<std::vector<double>, 2> box;
    std::array<double, 2> boxSum {};
    int boxPos = 0;
    std::array<float, 2> env {};
    Smoothed gain;
    float grDb = 0.0f;
};

//==============================================================================
enum class DitherMode { off = 0, bits24, bits16, bits16Shaped, numModes };

/** TPDF dither with optional noise shaping. */
class Dither
{
public:
    void reset() noexcept { err = {}; }
    void process (float* l, float* r, int n, DitherMode mode) noexcept;

private:
    Random rng { 0x9e3779b9u };
    std::array<std::array<float, 5>, 2> err {};
};
} // namespace lam
