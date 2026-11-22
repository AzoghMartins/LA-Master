#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "Loudness.h"
#include "Modules.h"

namespace lam
{
/** Modules whose order in the chain can be changed (the Maximizer is always last). */
enum class ModuleId { eq, stabilizer, dyneq, glue, dynamics, exciter, imager, numModules };
constexpr int kNumOrderable = (int) ModuleId::numModules;
using ModuleOrder = std::array<int, kNumOrderable>;

const char* moduleShortName (ModuleId m) noexcept;
ModuleOrder defaultModuleOrder() noexcept;
/** Packs / unpacks an order into a string such as "0123456" (invalid strings give the default). */
ModuleOrder orderFromString (const char* text) noexcept;
void orderToString (const ModuleOrder& order, char* out7plusNull) noexcept;

enum class MonitorMode { stereo = 0, mono, side, left, right, numModes };

struct EngineParams
{
    // Input
    float inputGainDb = 0.0f;
    float balance = 0.0f;      // -1 (left) .. +1 (right)
    float subsonicHz = 10.0f;  // <= 10.5 = off

    EqParams eq;
    StabilizerParams stab;
    DynEqParams dyneq;
    GlueParams glue;
    MultibandParams mb;
    ExciterParams exc;
    ImagerParams img;
    MaximizerParams max;
    ModuleOrder order = defaultModuleOrder();

    // Output
    float outputGainDb = 0.0f;
    DitherMode dither = DitherMode::off;
    MonitorMode monitor = MonitorMode::stereo;
    bool bypass = false;     // listen to the (delay compensated) input
    bool gainMatch = true;   // bypass and delta are level matched to the master
    bool delta = false;      // listen to what the chain changes
};

/** One 100 ms loudness frame for the history display. */
struct LoudnessFrame
{
    float momentary = kSilenceLufs, shortTerm = kSilenceLufs, inShortTerm = kSilenceLufs;
    float maxGrDb = 0.0f, truePeakDb = -100.0f;
};

struct LoudnessListener
{
    virtual ~LoudnessListener() = default;
    virtual void loudnessFrame (const LoudnessFrame&) = 0;
};

/** Meter values for the UI, valid after process(). */
struct MeterData
{
    float inPeak = 0.0f, outPeak = 0.0f;
    float momentary = kSilenceLufs, shortTerm = kSilenceLufs, integrated = kSilenceLufs, range = 0.0f;
    float inShortTerm = kSilenceLufs, inIntegrated = kSilenceLufs;
    float truePeakMaxDb = -100.0f;
    float glueDb = 0.0f, maxDb = 0.0f;
    std::array<float, kMbBands> mbDb {};
    std::array<float, kDynEqBands> dynEqDb {};
    BandCurve stab {};
    float correlation = 1.0f;
    float matchDb = 0.0f;
};

/**
    Complete mastering chain, independent of JUCE:
    Input (gain, balance, subsonic) -> re-orderable modules (EQ, Stabilizer, Dynamic EQ, Glue, Dynamics,
    Exciter, Imager) -> Maximizer -> Output gain -> gain-matched bypass / delta -> Dither -> Monitor.
*/
class MasterEngine
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    void setParams (const EngineParams& p) noexcept;

    /** Processes up to maxBlockSize samples in place. `channels` is 1 or 2. */
    void process (float* const* io, int channels, int numSamples, LoudnessListener* listener) noexcept;

    int getLatency() const noexcept { return totalLatency; }
    const MeterData& getMeters() const noexcept { return meters; }
    /** Restarts integrated loudness, loudness range and true-peak max. */
    void resetLoudness() noexcept;
    double getSampleRate() const noexcept { return sr; }

    // Analysis buffers (valid after process())
    const float* getInputAnalysis() const noexcept { return analysisIn.data(); }
    const float* getOutputAnalysis() const noexcept { return analysisOut.data(); }
    const float* getOutputLeft() const noexcept { return scopeL.data(); }
    const float* getOutputRight() const noexcept { return scopeR.data(); }

private:
    void runModule (ModuleId id, float* l, float* r, int n) noexcept;

    double sr = 48000.0;
    int maxBlock = 512;
    EngineParams params;

    std::array<Filter24, 2> subsonic;
    float lastSubsonic = -1.0f;
    Smoothed inGain, balanceSm, outGain, bypassSm, deltaSm, matchSm;

    EqModule eq;
    StabilizerModule stab;
    DynamicEqModule dyneq;
    GlueModule glue;
    MultibandModule mb;
    ExciterModule exc;
    ImagerModule img;
    MaximizerModule maximizer;
    Dither dither;

    LoudnessMeter inMeter, outMeter;
    std::array<TruePeakDetector, 2> outTp;
    float hopTruePeak = 0.0f;
    double corrLR = 0.0, corrLL = 0.0, corrRR = 0.0;
    float matchDb = 0.0f;

    std::vector<float> rawL, rawR, workL, workR, dryL, dryR, analysisIn, analysisOut, scopeL, scopeR;
    std::array<LatencyDelay, 2> dryDelay;
    int totalLatency = 0;

    MeterData meters;
};
} // namespace lam
