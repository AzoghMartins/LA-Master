#pragma once

#include <string>

#include "MasterEngine.h"

namespace lam
{
/** Target tonal balance styles (octave band curves relative to pink noise). */
enum class TargetStyle { balanced = 0, pop, edm, hiphop, rock, acoustic, orchestral, lofi, reference, numStyles };

const char* targetStyleName (TargetStyle s) noexcept;
/** Normalised target curve of a style (the reference style falls back to Balanced here). */
BandCurve targetCurve (TargetStyle s) noexcept;

/** Analysis of a piece of audio (used for reference tracks and by the Assistant). */
struct TrackAnalysis
{
    bool valid = false;
    float integratedLufs = kSilenceLufs;
    float truePeakDb = -100.0f;
    float range = 0.0f;
    BandCurve curve {}; // normalised octave band balance
    double seconds = 0.0;
};

TrackAnalysis analyseTrack (const float* l, const float* r, int numSamples, double sampleRate);

struct AssistResult
{
    bool ok = false;
    EngineParams params;          // the suggested settings (start from the current ones)
    float inputLufs = kSilenceLufs;
    float resultLufs = kSilenceLufs;  // integrated loudness of the analysed audio with the new settings
    float resultTruePeak = -100.0f;
    std::string summary;
};

/**
    Master Assistant: listens to a section of the mix and suggests a starting master for a tonal target
    and loudness target. It sets the EQ, Stabilizer, Glue, multiband Dynamics, mono bass and the
    Maximizer gain (found by rendering the audio through the new chain, so the loudness target is met).
*/
AssistResult runAssistant (const float* l, const float* r, int numSamples, double sampleRate, const EngineParams& current,
                           const BandCurve& target, float targetLufs);
} // namespace lam
