#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "dsp/Assistant.h"
#include "dsp/MasterEngine.h"

namespace lam
{
// Every parameter: X (enum name, id string)
#define LAM_PARAMETERS(X)                                                                                         \
    X (inGain, "inGain") X (balance, "balance") X (subsonic, "subsonic")                                           \
    X (targetStyle, "targetStyle") X (targetLufs, "targetLufs")                                                    \
    X (bypass, "bypass") X (gainMatch, "gainMatch") X (delta, "delta") X (monitor, "monitor")                     \
    X (eqOn, "eqOn") X (eqHpf, "eqHpf") X (eqLpf, "eqLpf")                                                         \
    X (eq1Freq, "eq1Freq") X (eq1Gain, "eq1Gain") X (eq1Q, "eq1Q") X (eq1Mode, "eq1Mode")                           \
    X (eq2Freq, "eq2Freq") X (eq2Gain, "eq2Gain") X (eq2Q, "eq2Q") X (eq2Mode, "eq2Mode")                           \
    X (eq3Freq, "eq3Freq") X (eq3Gain, "eq3Gain") X (eq3Q, "eq3Q") X (eq3Mode, "eq3Mode")                           \
    X (eq4Freq, "eq4Freq") X (eq4Gain, "eq4Gain") X (eq4Q, "eq4Q") X (eq4Mode, "eq4Mode")                           \
    X (eq5Freq, "eq5Freq") X (eq5Gain, "eq5Gain") X (eq5Q, "eq5Q") X (eq5Mode, "eq5Mode")                           \
    X (eq6Freq, "eq6Freq") X (eq6Gain, "eq6Gain") X (eq6Q, "eq6Q") X (eq6Mode, "eq6Mode")                           \
    X (deqOn, "deqOn") X (deqAttack, "deqAttack") X (deqRelease, "deqRelease")                                     \
    X (deq1Freq, "deq1Freq") X (deq1Q, "deq1Q") X (deq1Thr, "deq1Thr") X (deq1Range, "deq1Range") X (deq1Mode, "deq1Mode") \
    X (deq2Freq, "deq2Freq") X (deq2Q, "deq2Q") X (deq2Thr, "deq2Thr") X (deq2Range, "deq2Range") X (deq2Mode, "deq2Mode") \
    X (deq3Freq, "deq3Freq") X (deq3Q, "deq3Q") X (deq3Thr, "deq3Thr") X (deq3Range, "deq3Range") X (deq3Mode, "deq3Mode") \
    X (deq4Freq, "deq4Freq") X (deq4Q, "deq4Q") X (deq4Thr, "deq4Thr") X (deq4Range, "deq4Range") X (deq4Mode, "deq4Mode") \
    X (stabOn, "stabOn") X (stabAmount, "stabAmount") X (stabSpeed, "stabSpeed") X (stabTilt, "stabTilt")          \
    X (glueOn, "glueOn") X (glueThreshold, "glueThreshold") X (glueRatio, "glueRatio") X (glueAttack, "glueAttack") \
    X (glueRelease, "glueRelease") X (glueMakeup, "glueMakeup") X (glueScHpf, "glueScHpf") X (glueMix, "glueMix")   \
    X (mbOn, "mbOn") X (mbLowMid, "mbLowMid") X (mbMidHigh, "mbMidHigh") X (mbMix, "mbMix")                         \
    X (mb1Thr, "mb1Thr") X (mb1Ratio, "mb1Ratio") X (mb1Attack, "mb1Attack") X (mb1Release, "mb1Release") X (mb1Gain, "mb1Gain") X (mb1Solo, "mb1Solo") \
    X (mb2Thr, "mb2Thr") X (mb2Ratio, "mb2Ratio") X (mb2Attack, "mb2Attack") X (mb2Release, "mb2Release") X (mb2Gain, "mb2Gain") X (mb2Solo, "mb2Solo") \
    X (mb3Thr, "mb3Thr") X (mb3Ratio, "mb3Ratio") X (mb3Attack, "mb3Attack") X (mb3Release, "mb3Release") X (mb3Gain, "mb3Gain") X (mb3Solo, "mb3Solo") \
    X (excOn, "excOn") X (excMode, "excMode") X (excLowMid, "excLowMid") X (excMidHigh, "excMidHigh")               \
    X (exc1Drive, "exc1Drive") X (exc1Mix, "exc1Mix") X (exc2Drive, "exc2Drive") X (exc2Mix, "exc2Mix")             \
    X (exc3Drive, "exc3Drive") X (exc3Mix, "exc3Mix")                                                               \
    X (imgOn, "imgOn") X (imgLowMid, "imgLowMid") X (imgMidHigh, "imgMidHigh")                                      \
    X (img1Width, "img1Width") X (img2Width, "img2Width") X (img3Width, "img3Width")                                \
    X (imgStereoize, "imgStereoize") X (imgMonoBass, "imgMonoBass")                                                 \
    X (maxOn, "maxOn") X (maxGain, "maxGain") X (maxCeiling, "maxCeiling") X (maxRelease, "maxRelease")             \
    X (maxMode, "maxMode") X (maxTruePeak, "maxTruePeak") X (maxLink, "maxLink")                                    \
    X (outGain, "outGain") X (dither, "dither")

namespace ParamID
{
#define LAM_DECLARE_ID(name, id) inline constexpr auto name = id;
LAM_PARAMETERS (LAM_DECLARE_ID)
#undef LAM_DECLARE_ID
} // namespace ParamID

enum class P : int
{
#define LAM_DECLARE_ENUM(name, id) name,
    LAM_PARAMETERS (LAM_DECLARE_ENUM)
#undef LAM_DECLARE_ENUM
    count
};

/** Parameter IDs per band, for loops in the editor and displays. */
namespace BandIDs
{
    extern const char* const eq[kEqBands][4];        // freq, gain, q, mode
    extern const char* const dynEq[kDynEqBands][5];  // freq, q, threshold, range, mode
    extern const char* const mb[kMbBands][6];        // threshold, ratio, attack, release, gain, solo
    extern const char* const exc[kMbBands][2];       // drive, mix
    extern const char* const img[kMbBands];          // width
}

/** IDs of the enable switch of each orderable module (indexed by ModuleId). */
const char* moduleEnableID (ModuleId m) noexcept;

/** Values behind the stepped Glue choices. */
float glueRatioValue (int index) noexcept;
float glueAttackValue (int index) noexcept;
float glueReleaseValue (int index) noexcept; // <= 0 = auto

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/** Cached raw parameter pointers so the audio thread can build EngineParams cheaply. */
class ParameterCache
{
public:
    explicit ParameterCache (juce::AudioProcessorValueTreeState& state);
    /** Fills everything except the module order and the Stabilizer target curve. */
    void read (EngineParams& p) const noexcept;
    float get (P p) const noexcept { return values[(size_t) p]->load (std::memory_order_relaxed); }

private:
    std::array<std::atomic<float>*, (size_t) P::count> values {};
};

struct FactoryPreset
{
    FactoryPreset (juce::String n, std::vector<std::pair<const char*, float>> v, juce::String o = {})
        : name (std::move (n)), values (std::move (v)), order (std::move (o)) {}

    juce::String name;
    std::vector<std::pair<const char*, float>> values;
    juce::String order; // empty = default order
};

const std::vector<FactoryPreset>& getFactoryPresets();
} // namespace lam
