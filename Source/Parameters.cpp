#include "Parameters.h"

namespace lam
{
namespace
{
using APF = juce::AudioParameterFloat;
using APFA = juce::AudioParameterFloatAttributes;

juce::String formatDb (float v, int) { return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB"; }
juce::String formatMs (float v, int) { return (v < 10.0f ? juce::String (v, 1) : juce::String (juce::roundToInt (v))) + " ms"; }
juce::String formatPct (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; }
juce::String formatHz (float v, int = 0)
{
    return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
}
juce::String formatRatio (float v, int) { return juce::String (v, v < 10.0f ? 1 : 0) + ":1"; }

juce::NormalisableRange<float> skewed (float lo, float hi, float centre, float step = 0.0f)
{
    juce::NormalisableRange<float> r (lo, hi, step);
    r.setSkewForCentre (centre);
    return r;
}

std::unique_ptr<APF> floatParam (const char* id, const juce::String& name, juce::NormalisableRange<float> range, float def,
                                 std::function<juce::String (float, int)> fmt, const juce::String& label = {})
{
    return std::make_unique<APF> (juce::ParameterID { id, 1 }, name, range, def,
                                  APFA().withLabel (label).withStringFromValueFunction (std::move (fmt)));
}

std::unique_ptr<juce::AudioParameterBool> boolParam (const char* id, const juce::String& name, bool def, bool automatable = true)
{
    return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def,
                                                       juce::AudioParameterBoolAttributes().withAutomatable (automatable));
}

std::unique_ptr<juce::AudioParameterChoice> choiceParam (const char* id, const juce::String& name, const juce::StringArray& items,
                                                         int def, bool automatable = true)
{
    return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, items, def,
                                                         juce::AudioParameterChoiceAttributes().withAutomatable (automatable));
}

const juce::StringArray channelModes { "Stereo", "Mid", "Side" };
} // namespace

namespace BandIDs
{
    const char* const eq[kEqBands][4] = { { ParamID::eq1Freq, ParamID::eq1Gain, ParamID::eq1Q, ParamID::eq1Mode },
                                          { ParamID::eq2Freq, ParamID::eq2Gain, ParamID::eq2Q, ParamID::eq2Mode },
                                          { ParamID::eq3Freq, ParamID::eq3Gain, ParamID::eq3Q, ParamID::eq3Mode },
                                          { ParamID::eq4Freq, ParamID::eq4Gain, ParamID::eq4Q, ParamID::eq4Mode },
                                          { ParamID::eq5Freq, ParamID::eq5Gain, ParamID::eq5Q, ParamID::eq5Mode },
                                          { ParamID::eq6Freq, ParamID::eq6Gain, ParamID::eq6Q, ParamID::eq6Mode } };
    const char* const dynEq[kDynEqBands][5] = { { ParamID::deq1Freq, ParamID::deq1Q, ParamID::deq1Thr, ParamID::deq1Range, ParamID::deq1Mode },
                                                { ParamID::deq2Freq, ParamID::deq2Q, ParamID::deq2Thr, ParamID::deq2Range, ParamID::deq2Mode },
                                                { ParamID::deq3Freq, ParamID::deq3Q, ParamID::deq3Thr, ParamID::deq3Range, ParamID::deq3Mode },
                                                { ParamID::deq4Freq, ParamID::deq4Q, ParamID::deq4Thr, ParamID::deq4Range, ParamID::deq4Mode } };
    const char* const mb[kMbBands][6] = { { ParamID::mb1Thr, ParamID::mb1Ratio, ParamID::mb1Attack, ParamID::mb1Release, ParamID::mb1Gain, ParamID::mb1Solo },
                                          { ParamID::mb2Thr, ParamID::mb2Ratio, ParamID::mb2Attack, ParamID::mb2Release, ParamID::mb2Gain, ParamID::mb2Solo },
                                          { ParamID::mb3Thr, ParamID::mb3Ratio, ParamID::mb3Attack, ParamID::mb3Release, ParamID::mb3Gain, ParamID::mb3Solo } };
    const char* const exc[kMbBands][2] = { { ParamID::exc1Drive, ParamID::exc1Mix }, { ParamID::exc2Drive, ParamID::exc2Mix },
                                           { ParamID::exc3Drive, ParamID::exc3Mix } };
    const char* const img[kMbBands] = { ParamID::img1Width, ParamID::img2Width, ParamID::img3Width };
} // namespace BandIDs

const char* moduleEnableID (ModuleId m) noexcept
{
    switch (m)
    {
        case ModuleId::eq:         return ParamID::eqOn;
        case ModuleId::stabilizer: return ParamID::stabOn;
        case ModuleId::dyneq:      return ParamID::deqOn;
        case ModuleId::glue:       return ParamID::glueOn;
        case ModuleId::dynamics:   return ParamID::mbOn;
        case ModuleId::exciter:    return ParamID::excOn;
        case ModuleId::imager:     return ParamID::imgOn;
        case ModuleId::numModules: break;
    }
    return ParamID::eqOn;
}

float glueRatioValue (int i) noexcept { static const float v[] = { 1.5f, 2.0f, 4.0f, 10.0f }; return v[juce::jlimit (0, 3, i)]; }
float glueAttackValue (int i) noexcept { static const float v[] = { 0.1f, 0.3f, 1.0f, 3.0f, 10.0f, 30.0f }; return v[juce::jlimit (0, 5, i)]; }
float glueReleaseValue (int i) noexcept { static const float v[] = { 100.0f, 300.0f, 600.0f, 1200.0f, -1.0f }; return v[juce::jlimit (0, 4, i)]; }

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    auto group = [] (const char* id, const char* name) { return std::make_unique<juce::AudioProcessorParameterGroup> (id, name, "|"); };
    auto offBelow = [] (float limit) { return [limit] (float v, int) { return v <= limit ? juce::String ("Off") : formatHz (v); }; };

    // ---- Input / target / monitoring ---------------------------------------------------------------------
    auto input = group ("input", "Input");
    input->addChild (floatParam (ParamID::inGain, "Input Gain", { -24.0f, 24.0f, 0.1f }, 0.0f, formatDb, "dB"));
    input->addChild (floatParam (ParamID::balance, "Balance", { -100.0f, 100.0f, 1.0f }, 0.0f,
                                 [] (float v, int) { return std::abs (v) < 0.5f ? juce::String ("C") : juce::String (juce::roundToInt (std::abs (v))) + (v < 0 ? " L" : " R"); }, "%"));
    input->addChild (floatParam (ParamID::subsonic, "Subsonic Filter", skewed (10.0f, 60.0f, 25.0f, 1.0f), 10.0f, offBelow (10.5f), "Hz"));
    layout.add (std::move (input));

    auto target = group ("target", "Target");
    juce::StringArray styles;
    for (int s = 0; s < (int) TargetStyle::numStyles; ++s) styles.add (targetStyleName ((TargetStyle) s));
    target->addChild (choiceParam (ParamID::targetStyle, "Target Style", styles, 0));
    target->addChild (floatParam (ParamID::targetLufs, "Target Loudness", { -24.0f, -5.0f, 0.5f }, -14.0f,
                                  [] (float v, int) { return juce::String (v, 1) + " LUFS"; }, "LUFS"));
    layout.add (std::move (target));

    auto mon = group ("monitor", "Monitoring");
    mon->addChild (boolParam (ParamID::bypass, "Bypass (Compare)", false));
    mon->addChild (boolParam (ParamID::gainMatch, "Gain Match", true, false));
    mon->addChild (boolParam (ParamID::delta, "Delta", false, false));
    mon->addChild (choiceParam (ParamID::monitor, "Monitor", { "Stereo", "Mono", "Side", "Left", "Right" }, 0, false));
    layout.add (std::move (mon));

    // ---- EQ -------------------------------------------------------------------------------------------------
    auto eq = group ("eq", "EQ");
    eq->addChild (boolParam (ParamID::eqOn, "EQ", true));
    eq->addChild (floatParam (ParamID::eqHpf, "EQ Low Cut", skewed (10.0f, 300.0f, 40.0f, 1.0f), 10.0f, offBelow (10.5f), "Hz"));
    eq->addChild (floatParam (ParamID::eqLpf, "EQ High Cut", skewed (2000.0f, 22000.0f, 9000.0f, 10.0f), 22000.0f,
                              [] (float v, int) { return v >= 21500.0f ? juce::String ("Off") : formatHz (v); }, "Hz"));
    const EqParams eqDef;
    const char* eqNames[kEqBands] = { "EQ Low Shelf", "EQ Low Mid", "EQ Mid", "EQ Upper Mid", "EQ Presence", "EQ High Shelf" };
    for (int b = 0; b < kEqBands; ++b)
    {
        const auto k = (size_t) b;
        const juce::String n (eqNames[b]);
        const bool shelf = b == 0 || b == kEqBands - 1;
        eq->addChild (floatParam (BandIDs::eq[b][0], n + " Freq", skewed (20.0f, 20000.0f, 1000.0f, 1.0f), eqDef.freq[k], formatHz, "Hz"));
        eq->addChild (floatParam (BandIDs::eq[b][1], n + " Gain", { -12.0f, 12.0f, 0.1f }, 0.0f, formatDb, "dB"));
        eq->addChild (floatParam (BandIDs::eq[b][2], n + (shelf ? " Slope" : " Q"), skewed (0.2f, 6.0f, 1.0f, 0.01f), eqDef.q[k],
                                  [] (float v, int) { return juce::String (v, 2); }));
        eq->addChild (choiceParam (BandIDs::eq[b][3], n + " Channel", channelModes, 0));
    }
    layout.add (std::move (eq));

    // ---- Dynamic EQ --------------------------------------------------------------------------------------------
    auto deq = group ("dyneq", "Dynamic EQ");
    deq->addChild (boolParam (ParamID::deqOn, "Dynamic EQ", false));
    deq->addChild (floatParam (ParamID::deqAttack, "Dynamic EQ Attack", skewed (0.5f, 50.0f, 6.0f, 0.1f), 5.0f, formatMs, "ms"));
    deq->addChild (floatParam (ParamID::deqRelease, "Dynamic EQ Release", skewed (20.0f, 1000.0f, 150.0f, 1.0f), 120.0f, formatMs, "ms"));
    const DynEqParams deqDef;
    for (int b = 0; b < kDynEqBands; ++b)
    {
        const auto k = (size_t) b;
        const juce::String n = "Dynamic EQ " + juce::String (b + 1);
        deq->addChild (floatParam (BandIDs::dynEq[b][0], n + " Freq", skewed (20.0f, 20000.0f, 1000.0f, 1.0f), deqDef.freq[k], formatHz, "Hz"));
        deq->addChild (floatParam (BandIDs::dynEq[b][1], n + " Q", skewed (0.3f, 8.0f, 1.5f, 0.01f), deqDef.q[k],
                                   [] (float v, int) { return juce::String (v, 2); }));
        deq->addChild (floatParam (BandIDs::dynEq[b][2], n + " Threshold", { -60.0f, 0.0f, 0.1f }, deqDef.threshold[k], formatDb, "dB"));
        deq->addChild (floatParam (BandIDs::dynEq[b][3], n + " Range", { -12.0f, 12.0f, 0.1f }, 0.0f,
                                   [] (float v, int i) { return std::abs (v) < 0.05f ? juce::String ("Off") : formatDb (v, i); }, "dB"));
        deq->addChild (choiceParam (BandIDs::dynEq[b][4], n + " Channel", channelModes, 0));
    }
    layout.add (std::move (deq));

    // ---- Stabilizer ----------------------------------------------------------------------------------------------
    auto stab = group ("stab", "Stabilizer");
    stab->addChild (boolParam (ParamID::stabOn, "Stabilizer", false));
    stab->addChild (floatParam (ParamID::stabAmount, "Stabilizer Amount", { 0.0f, 100.0f, 1.0f }, 50.0f, formatPct, "%"));
    stab->addChild (floatParam (ParamID::stabSpeed, "Stabilizer Speed", skewed (200.0f, 5000.0f, 1200.0f, 1.0f), 1500.0f, formatMs, "ms"));
    stab->addChild (floatParam (ParamID::stabTilt, "Stabilizer Tilt", { -1.5f, 1.5f, 0.05f }, 0.0f,
                                [] (float v, int) { return (v > 0.02f ? "+" : "") + juce::String (v, 2) + " dB/oct"; }, "dB/oct"));
    layout.add (std::move (stab));

    // ---- Glue ------------------------------------------------------------------------------------------------------
    auto glue = group ("glue", "Glue");
    glue->addChild (boolParam (ParamID::glueOn, "Glue", false));
    glue->addChild (floatParam (ParamID::glueThreshold, "Glue Threshold", { -40.0f, 0.0f, 0.1f }, -18.0f, formatDb, "dB"));
    glue->addChild (choiceParam (ParamID::glueRatio, "Glue Ratio", { "1.5:1", "2:1", "4:1", "10:1" }, 1));
    glue->addChild (choiceParam (ParamID::glueAttack, "Glue Attack", { "0.1 ms", "0.3 ms", "1 ms", "3 ms", "10 ms", "30 ms" }, 4));
    glue->addChild (choiceParam (ParamID::glueRelease, "Glue Release", { "0.1 s", "0.3 s", "0.6 s", "1.2 s", "Auto" }, 4));
    glue->addChild (floatParam (ParamID::glueMakeup, "Glue Makeup", { 0.0f, 12.0f, 0.1f }, 0.0f, formatDb, "dB"));
    glue->addChild (floatParam (ParamID::glueScHpf, "Glue Side-Chain HPF", skewed (20.0f, 250.0f, 80.0f, 1.0f), 60.0f, offBelow (20.5f), "Hz"));
    glue->addChild (floatParam (ParamID::glueMix, "Glue Mix", { 0.0f, 100.0f, 1.0f }, 100.0f, formatPct, "%"));
    layout.add (std::move (glue));

    // ---- Multiband dynamics -----------------------------------------------------------------------------------------
    auto mb = group ("dynamics", "Dynamics");
    mb->addChild (boolParam (ParamID::mbOn, "Dynamics", false));
    mb->addChild (floatParam (ParamID::mbLowMid, "Dynamics Low/Mid Crossover", skewed (40.0f, 1000.0f, 200.0f, 1.0f), 150.0f, formatHz, "Hz"));
    mb->addChild (floatParam (ParamID::mbMidHigh, "Dynamics Mid/High Crossover", skewed (1000.0f, 12000.0f, 3500.0f, 1.0f), 3000.0f, formatHz, "Hz"));
    mb->addChild (floatParam (ParamID::mbMix, "Dynamics Mix", { 0.0f, 100.0f, 1.0f }, 100.0f, formatPct, "%"));
    const MultibandParams mbDef;
    const char* bandNames[kMbBands] = { "Low", "Mid", "High" };
    for (int b = 0; b < kMbBands; ++b)
    {
        const auto k = (size_t) b;
        const juce::String n = "Dynamics " + juce::String (bandNames[b]);
        mb->addChild (floatParam (BandIDs::mb[b][0], n + " Threshold", { -50.0f, 0.0f, 0.1f }, mbDef.threshold[k], formatDb, "dB"));
        mb->addChild (floatParam (BandIDs::mb[b][1], n + " Ratio", skewed (1.0f, 10.0f, 2.5f, 0.01f), mbDef.ratio[k], formatRatio));
        mb->addChild (floatParam (BandIDs::mb[b][2], n + " Attack", skewed (0.5f, 100.0f, 12.0f, 0.1f), mbDef.attackMs[k], formatMs, "ms"));
        mb->addChild (floatParam (BandIDs::mb[b][3], n + " Release", skewed (20.0f, 1000.0f, 160.0f, 1.0f), mbDef.releaseMs[k], formatMs, "ms"));
        mb->addChild (floatParam (BandIDs::mb[b][4], n + " Gain", { -12.0f, 12.0f, 0.1f }, 0.0f, formatDb, "dB"));
        mb->addChild (boolParam (BandIDs::mb[b][5], n + " Solo", false, false));
    }
    layout.add (std::move (mb));

    // ---- Exciter ------------------------------------------------------------------------------------------------------
    auto exc = group ("exciter", "Exciter");
    exc->addChild (boolParam (ParamID::excOn, "Exciter", false));
    exc->addChild (choiceParam (ParamID::excMode, "Exciter Mode", { "Tape", "Tube", "Warm", "Triode", "Retro" }, 0));
    exc->addChild (floatParam (ParamID::excLowMid, "Exciter Low/Mid Crossover", skewed (50.0f, 1000.0f, 250.0f, 1.0f), 200.0f, formatHz, "Hz"));
    exc->addChild (floatParam (ParamID::excMidHigh, "Exciter Mid/High Crossover", skewed (1000.0f, 12000.0f, 4000.0f, 1.0f), 4000.0f, formatHz, "Hz"));
    const ExciterParams excDef;
    for (int b = 0; b < kMbBands; ++b)
    {
        const juce::String n = "Exciter " + juce::String (bandNames[b]);
        exc->addChild (floatParam (BandIDs::exc[b][0], n + " Drive", { 0.0f, 24.0f, 0.1f }, excDef.driveDb[(size_t) b], formatDb, "dB"));
        exc->addChild (floatParam (BandIDs::exc[b][1], n + " Mix", { 0.0f, 100.0f, 1.0f }, 50.0f, formatPct, "%"));
    }
    layout.add (std::move (exc));

    // ---- Imager ---------------------------------------------------------------------------------------------------------
    auto img = group ("imager", "Imager");
    img->addChild (boolParam (ParamID::imgOn, "Imager", false));
    img->addChild (floatParam (ParamID::imgLowMid, "Imager Low/Mid Crossover", skewed (40.0f, 1000.0f, 250.0f, 1.0f), 250.0f, formatHz, "Hz"));
    img->addChild (floatParam (ParamID::imgMidHigh, "Imager Mid/High Crossover", skewed (1000.0f, 12000.0f, 4000.0f, 1.0f), 4000.0f, formatHz, "Hz"));
    for (int b = 0; b < kMbBands; ++b)
        img->addChild (floatParam (BandIDs::img[b], "Imager " + juce::String (bandNames[b]) + " Width", { 0.0f, 200.0f, 1.0f }, 100.0f, formatPct, "%"));
    img->addChild (floatParam (ParamID::imgStereoize, "Stereoize", { 0.0f, 100.0f, 1.0f }, 0.0f, formatPct, "%"));
    img->addChild (floatParam (ParamID::imgMonoBass, "Mono Bass", skewed (20.0f, 300.0f, 100.0f, 1.0f), 20.0f, offBelow (20.5f), "Hz"));
    layout.add (std::move (img));

    // ---- Maximizer --------------------------------------------------------------------------------------------------------
    auto mx = group ("maximizer", "Maximizer");
    mx->addChild (boolParam (ParamID::maxOn, "Maximizer", true));
    mx->addChild (floatParam (ParamID::maxGain, "Maximizer Gain", { 0.0f, 24.0f, 0.1f }, 0.0f, formatDb, "dB"));
    mx->addChild (floatParam (ParamID::maxCeiling, "Ceiling", { -3.0f, 0.0f, 0.1f }, -1.0f, formatDb, "dB"));
    mx->addChild (floatParam (ParamID::maxRelease, "Maximizer Release", skewed (1.0f, 1000.0f, 100.0f, 0.1f), 120.0f, formatMs, "ms"));
    mx->addChild (choiceParam (ParamID::maxMode, "Maximizer Character", { "Transparent", "Balanced", "Loud", "Clip" }, 1));
    mx->addChild (boolParam (ParamID::maxTruePeak, "True Peak", true));
    mx->addChild (floatParam (ParamID::maxLink, "Stereo Link", { 0.0f, 100.0f, 1.0f }, 100.0f, formatPct, "%"));
    layout.add (std::move (mx));

    auto out = group ("output", "Output");
    out->addChild (floatParam (ParamID::outGain, "Output Gain", { -24.0f, 6.0f, 0.1f }, 0.0f, formatDb, "dB"));
    out->addChild (choiceParam (ParamID::dither, "Dither", { "Off", "24-bit", "16-bit", "16-bit Shaped" }, 0));
    layout.add (std::move (out));

    return layout;
}

//==============================================================================
ParameterCache::ParameterCache (juce::AudioProcessorValueTreeState& s)
{
    const char* ids[] = {
#define LAM_ID_STRING(name, id) id,
        LAM_PARAMETERS (LAM_ID_STRING)
#undef LAM_ID_STRING
    };
    for (size_t i = 0; i < values.size(); ++i)
    {
        values[i] = s.getRawParameterValue (ids[i]);
        jassert (values[i] != nullptr);
    }
}

void ParameterCache::read (EngineParams& p) const noexcept
{
    auto f = [this] (P id) { return get (id); };
    auto b = [this] (P id) { return get (id) > 0.5f; };
    auto i = [this] (P id) { return (int) std::lround (get (id)); };
    auto at = [] (P first, int offset) { return (P) ((int) first + offset); };

    p.inputGainDb = f (P::inGain);
    p.balance = f (P::balance) * 0.01f;
    p.subsonicHz = f (P::subsonic);

    p.bypass = b (P::bypass);
    p.gainMatch = b (P::gainMatch);
    p.delta = b (P::delta);
    p.monitor = (MonitorMode) i (P::monitor);

    p.eq.enabled = b (P::eqOn);
    p.eq.hpfHz = f (P::eqHpf);
    p.eq.lpfHz = f (P::eqLpf);
    for (int k = 0; k < kEqBands; ++k)
    {
        const P base = at (P::eq1Freq, 4 * k);
        p.eq.freq[(size_t) k] = f (base);
        p.eq.gain[(size_t) k] = f (at (base, 1));
        p.eq.q[(size_t) k] = f (at (base, 2));
        p.eq.mode[(size_t) k] = i (at (base, 3));
    }

    p.dyneq.enabled = b (P::deqOn);
    p.dyneq.attackMs = f (P::deqAttack);
    p.dyneq.releaseMs = f (P::deqRelease);
    for (int k = 0; k < kDynEqBands; ++k)
    {
        const P base = at (P::deq1Freq, 5 * k);
        p.dyneq.freq[(size_t) k] = f (base);
        p.dyneq.q[(size_t) k] = f (at (base, 1));
        p.dyneq.threshold[(size_t) k] = f (at (base, 2));
        p.dyneq.range[(size_t) k] = f (at (base, 3));
        p.dyneq.mode[(size_t) k] = i (at (base, 4));
    }

    p.stab.enabled = b (P::stabOn);
    p.stab.amount = f (P::stabAmount) * 0.01f;
    p.stab.speedMs = f (P::stabSpeed);
    p.stab.tilt = f (P::stabTilt);

    p.glue.enabled = b (P::glueOn);
    p.glue.thresholdDb = f (P::glueThreshold);
    p.glue.ratio = glueRatioValue (i (P::glueRatio));
    p.glue.attackMs = glueAttackValue (i (P::glueAttack));
    p.glue.releaseMs = glueReleaseValue (i (P::glueRelease));
    p.glue.makeupDb = f (P::glueMakeup);
    p.glue.scHpfHz = f (P::glueScHpf);
    p.glue.mix = f (P::glueMix) * 0.01f;

    p.mb.enabled = b (P::mbOn);
    p.mb.lowMidHz = f (P::mbLowMid);
    p.mb.midHighHz = f (P::mbMidHigh);
    p.mb.mix = f (P::mbMix) * 0.01f;
    for (int k = 0; k < kMbBands; ++k)
    {
        const P base = at (P::mb1Thr, 6 * k);
        const auto bk = (size_t) k;
        p.mb.threshold[bk] = f (base);
        p.mb.ratio[bk] = f (at (base, 1));
        p.mb.attackMs[bk] = f (at (base, 2));
        p.mb.releaseMs[bk] = f (at (base, 3));
        p.mb.gainDb[bk] = f (at (base, 4));
        p.mb.solo[bk] = b (at (base, 5));
    }

    p.exc.enabled = b (P::excOn);
    p.exc.mode = (ExciterMode) i (P::excMode);
    p.exc.lowMidHz = f (P::excLowMid);
    p.exc.midHighHz = f (P::excMidHigh);
    for (int k = 0; k < kMbBands; ++k)
    {
        p.exc.driveDb[(size_t) k] = f (at (P::exc1Drive, 2 * k));
        p.exc.mix[(size_t) k] = f (at (P::exc1Mix, 2 * k)) * 0.01f;
    }

    p.img.enabled = b (P::imgOn);
    p.img.lowMidHz = f (P::imgLowMid);
    p.img.midHighHz = f (P::imgMidHigh);
    for (int k = 0; k < kMbBands; ++k)
        p.img.width[(size_t) k] = f (at (P::img1Width, k)) * 0.01f;
    p.img.stereoize = f (P::imgStereoize) * 0.01f;
    p.img.monoBassHz = f (P::imgMonoBass);

    p.max.enabled = b (P::maxOn);
    p.max.gainDb = f (P::maxGain);
    p.max.ceilingDb = f (P::maxCeiling);
    p.max.releaseMs = f (P::maxRelease);
    p.max.mode = (MaxMode) i (P::maxMode);
    p.max.truePeak = b (P::maxTruePeak);
    p.max.link = f (P::maxLink) * 0.01f;

    p.outputGainDb = f (P::outGain);
    p.dither = (DitherMode) i (P::dither);
}

//==============================================================================
const std::vector<FactoryPreset>& getFactoryPresets()
{
    using namespace ParamID;
    static const std::vector<FactoryPreset> presets {
        { "Default", {} },
        { "Streaming Balanced",
          { { targetLufs, -14 }, { stabOn, 1 }, { stabAmount, 35 }, { glueOn, 1 }, { glueThreshold, -20 }, { mbOn, 1 },
            { mb1Thr, -24 }, { mb2Thr, -24 }, { mb3Thr, -28 }, { mb1Ratio, 1.8f }, { mb2Ratio, 1.6f }, { mb3Ratio, 1.6f },
            { imgOn, 1 }, { imgMonoBass, 100 }, { maxGain, 6 }, { maxMode, 1 } } },
        { "Loud Pop",
          { { targetStyle, 1 }, { targetLufs, -9 }, { eq2Gain, -1 }, { eq6Gain, 1.5f }, { stabOn, 1 }, { stabAmount, 40 },
            { glueOn, 1 }, { glueThreshold, -22 }, { glueRatio, 2 }, { glueMakeup, 1 }, { mbOn, 1 }, { mb1Thr, -26 },
            { mb2Thr, -26 }, { mb3Thr, -30 }, { excOn, 1 }, { exc1Drive, 0 }, { exc2Drive, 2 }, { exc3Drive, 4 },
            { exc1Mix, 30 }, { exc2Mix, 30 }, { exc3Mix, 40 }, { imgOn, 1 }, { img2Width, 105 }, { img3Width, 115 },
            { imgMonoBass, 120 }, { maxGain, 10 }, { maxMode, 2 } } },
        { "EDM Club",
          { { targetStyle, 2 }, { targetLufs, -7 }, { eq1Gain, 1.5f }, { stabOn, 1 }, { stabAmount, 35 }, { glueOn, 1 },
            { glueThreshold, -20 }, { glueAttack, 5 }, { mbOn, 1 }, { mb1Ratio, 3 }, { mb1Thr, -24 }, { mb2Thr, -26 },
            { mb3Thr, -28 }, { excOn, 1 }, { excMode, 1 }, { exc3Drive, 6 }, { imgOn, 1 }, { imgMonoBass, 140 },
            { img3Width, 125 }, { maxGain, 13 }, { maxMode, 3 }, { maxRelease, 60 } } },
        { "Hip-Hop Punch",
          { { targetStyle, 3 }, { targetLufs, -9 }, { eq1Freq, 55 }, { eq1Gain, 2 }, { glueOn, 1 }, { glueThreshold, -20 },
            { glueAttack, 5 }, { mbOn, 1 }, { mb1Thr, -22 }, { mb1Ratio, 2.5f }, { mb2Thr, -26 }, { mb3Thr, -30 },
            { excOn, 1 }, { excMode, 3 }, { exc1Drive, 6 }, { exc1Mix, 40 }, { exc2Mix, 20 }, { exc3Mix, 20 },
            { imgOn, 1 }, { imgMonoBass, 120 }, { maxGain, 10 }, { maxMode, 2 } } },
        { "Rock Energy",
          { { targetStyle, 4 }, { targetLufs, -10 }, { eq4Gain, 1 }, { glueOn, 1 }, { glueThreshold, -22 }, { glueRatio, 2 },
            { glueAttack, 3 }, { glueRelease, 1 }, { mbOn, 1 }, { mb1Thr, -24 }, { mb2Thr, -24 }, { mb3Thr, -28 },
            { excOn, 1 }, { exc2Drive, 4 }, { exc3Drive, 5 }, { maxGain, 9 }, { maxMode, 1 } } },
        { "Acoustic Natural",
          { { targetStyle, 5 }, { targetLufs, -16 }, { eq6Gain, 1 }, { stabOn, 1 }, { stabAmount, 30 }, { glueOn, 1 },
            { glueThreshold, -24 }, { glueRatio, 0 }, { glueAttack, 5 }, { maxGain, 4 }, { maxMode, 0 }, { maxRelease, 250 } } },
        { "Orchestral Dynamic",
          { { targetStyle, 6 }, { targetLufs, -18 }, { stabOn, 1 }, { stabAmount, 20 }, { stabSpeed, 4000 }, { maxGain, 2 },
            { maxMode, 0 }, { maxRelease, 400 } } },
        { "Lo-Fi Warm",
          { { targetStyle, 7 }, { targetLufs, -13 }, { eqLpf, 9000 }, { eq6Gain, -3 }, { eq2Gain, 1.5f }, { excOn, 1 },
            { excMode, 4 }, { exc1Drive, 8 }, { exc2Drive, 10 }, { exc1Mix, 60 }, { exc2Mix, 60 }, { exc3Mix, 30 },
            { glueOn, 1 }, { glueThreshold, -24 }, { glueRatio, 2 }, { imgOn, 1 }, { img3Width, 80 }, { maxGain, 6 } } },
        { "Podcast / Spoken Word",
          { { targetLufs, -16 }, { eqHpf, 70 }, { deqOn, 1 }, { deq3Freq, 6500 }, { deq3Q, 2 }, { deq3Thr, -32 },
            { deq3Range, -6 }, { glueOn, 1 }, { glueThreshold, -26 }, { glueRatio, 2 }, { glueAttack, 3 }, { imgOn, 1 },
            { img1Width, 0 }, { img2Width, 0 }, { img3Width, 0 }, { maxGain, 8 }, { maxMode, 1 } } },
        { "Gentle Glue",
          { { glueOn, 1 }, { glueThreshold, -16 }, { glueRatio, 1 }, { glueAttack, 5 }, { glueRelease, 4 }, { glueMix, 80 },
            { maxGain, 3 }, { maxMode, 0 } } },
        { "Wide & Bright",
          { { eq6Gain, 2.5f }, { eq5Gain, 0.8f }, { excOn, 1 }, { exc1Mix, 0 }, { exc3Drive, 6 }, { exc3Mix, 40 }, { imgOn, 1 },
            { img2Width, 120 }, { img3Width, 135 }, { imgStereoize, 20 }, { imgMonoBass, 100 }, { maxGain, 6 } } },
        { "Vinyl Pre-Master",
          { { eqHpf, 25 }, { eqLpf, 16000 }, { deqOn, 1 }, { deq4Freq, 7500 }, { deq4Thr, -30 }, { deq4Range, -4 },
            { imgOn, 1 }, { imgMonoBass, 150 }, { img3Width, 90 }, { maxGain, 3 }, { maxMode, 0 } } },
        { "Bass Control",
          { { deqOn, 1 }, { deq1Freq, 110 }, { deq1Thr, -24 }, { deq1Range, -5 }, { mbOn, 1 }, { mb1Thr, -22 }, { mb1Ratio, 3 },
            { mb2Ratio, 1 }, { mb3Ratio, 1 }, { imgOn, 1 }, { imgMonoBass, 120 }, { maxGain, 5 } } },
        { "Limiter Only",
          { { eqOn, 0 }, { maxGain, 6 }, { maxMode, 0 } } },
    };
    return presets;
}
} // namespace lam
