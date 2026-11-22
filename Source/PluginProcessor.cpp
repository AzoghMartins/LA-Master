#include "PluginProcessor.h"
#include "gui/PluginEditor.h"

namespace lam
{
namespace
{
uint32_t packOrder (const ModuleOrder& o) noexcept
{
    uint32_t v = 0;
    for (int i = 0; i < kNumOrderable; ++i)
        v |= (uint32_t) (o[(size_t) i] & 7) << (3 * i);
    return v | 0x80000000u;
}

ModuleOrder unpackOrder (uint32_t v) noexcept
{
    if ((v & 0x80000000u) == 0)
        return defaultModuleOrder();
    ModuleOrder o {};
    for (int i = 0; i < kNumOrderable; ++i)
        o[(size_t) i] = (int) ((v >> (3 * i)) & 7);
    return o;
}

juce::String orderString (const ModuleOrder& o)
{
    char buf[kNumOrderable + 1];
    orderToString (o, buf);
    return buf;
}

constexpr double kMaxAssistSeconds = 30.0;
} // namespace

MasterProcessor::MasterProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "LAMaster", createParameterLayout()),
      params (apvts)
{
    listener.owner = this;
    packedOrder.store (packOrder (defaultModuleOrder()));
    for (auto& v : referenceCurve) v.store (0.0f);
}

MasterProcessor::~MasterProcessor()
{
    if (assistThread.joinable())
        assistThread.join();
}

bool MasterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    const auto& in = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo())
        return false;
    return in.size() <= out.size();
}

void MasterProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    preparedRate = sampleRate;
    preparedBlockSize = std::max (32, samplesPerBlock);
    params.read (engineParams);
    engineParams.order = unpackOrder (packedOrder.load());
    engineParams.stab.target = getTargetCurve();
    engine.setParams (engineParams);
    engine.prepare (preparedRate, preparedBlockSize);
    engineSampleRate.store (preparedRate);
    setLatencySamples (engine.getLatency());
}

BandCurve MasterProcessor::getTargetCurve() const noexcept
{
    const auto style = (TargetStyle) juce::roundToInt (params.get (P::targetStyle));
    if (style == TargetStyle::reference && referenceValid.load (std::memory_order_relaxed))
    {
        BandCurve c {};
        for (int b = 0; b < kNumBands; ++b) c[(size_t) b] = referenceCurve[(size_t) b].load (std::memory_order_relaxed);
        return c;
    }
    return targetCurve (style);
}

void MasterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear();

    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();
    if (numIn == 1 && numOut > 1)
        buffer.copyFrom (1, 0, buffer, 0, 0, numSamples); // mono in, stereo out
    else
        for (int ch = numIn; ch < numOut; ++ch)
            buffer.clear (ch, 0, numSamples);
    const int channels = std::min (2, numOut);

    // ---- Assistant: record the raw input -----------------------------------------------------------
    const int as = assistState.load();
    if (as == 1)
    {
        int pos = recordPos.load (std::memory_order_relaxed);
        const int n = std::min (numSamples, (int) recordL.size() - pos);
        if (n > 0)
        {
            std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + n, recordL.begin() + pos);
            std::copy (buffer.getReadPointer (channels - 1), buffer.getReadPointer (channels - 1) + n, recordR.begin() + pos);
            pos += n;
            recordPos.store (pos, std::memory_order_relaxed);
        }
        assistSeconds.store ((double) pos / std::max (1.0, preparedRate));
    }
    else if (as == 2)
        assistState.store (3);

    if (loudnessResetRequest.exchange (false))
        engine.resetLoudness();

    params.read (engineParams);
    engineParams.order = unpackOrder (packedOrder.load (std::memory_order_relaxed));
    engineParams.stab.target = getTargetCurve();
    engine.setParams (engineParams);

    for (int start = 0; start < numSamples; start += preparedBlockSize)
    {
        const int n = std::min (preparedBlockSize, numSamples - start);
        float* ptrs[2] = { buffer.getWritePointer (0, start), buffer.getWritePointer (channels > 1 ? 1 : 0, start) };
        engine.process (ptrs, channels, n, &listener);
        spectrumIn.push (engine.getInputAnalysis(), n);
        spectrumOut.push (engine.getOutputAnalysis(), n);
        scopeL.push (engine.getOutputLeft(), n);
        scopeR.push (engine.getOutputRight(), n);
    }

    const auto& m = engine.getMeters();
    auto peakMax = [] (std::atomic<float>& a, float v) { a.store (std::max (a.load (std::memory_order_relaxed), v), std::memory_order_relaxed); };
    peakMax (meters.inPeak, m.inPeak);
    peakMax (meters.outPeak, m.outPeak);
    meters.momentary.store (m.momentary, std::memory_order_relaxed);
    meters.shortTerm.store (m.shortTerm, std::memory_order_relaxed);
    meters.integrated.store (m.integrated, std::memory_order_relaxed);
    meters.range.store (m.range, std::memory_order_relaxed);
    meters.inShortTerm.store (m.inShortTerm, std::memory_order_relaxed);
    meters.inIntegrated.store (m.inIntegrated, std::memory_order_relaxed);
    meters.truePeakMax.store (m.truePeakMaxDb, std::memory_order_relaxed);
    meters.glue.store (m.glueDb, std::memory_order_relaxed);
    meters.maximizer.store (m.maxDb, std::memory_order_relaxed);
    meters.correlation.store (m.correlation, std::memory_order_relaxed);
    meters.match.store (m.matchDb, std::memory_order_relaxed);
    for (size_t b = 0; b < m.mbDb.size(); ++b) meters.mb[b].store (m.mbDb[b], std::memory_order_relaxed);
    for (size_t b = 0; b < m.dynEqDb.size(); ++b) meters.dynEq[b].store (m.dynEqDb[b], std::memory_order_relaxed);
    for (size_t b = 0; b < m.stab.size(); ++b) meters.stab[b].store (m.stab[b], std::memory_order_relaxed);
}

//==============================================================================
ModuleOrder MasterProcessor::getModuleOrder() const noexcept
{
    return unpackOrder (packedOrder.load());
}

void MasterProcessor::setModuleOrder (const ModuleOrder& order)
{
    packedOrder.store (packOrder (order));
    apvts.state.setProperty ("order", orderString (order), nullptr);
}

void MasterProcessor::setParam (const char* id, float value)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        p->endChangeGesture();
    }
}

//==============================================================================
void MasterProcessor::startAssist()
{
    if (assistBusy.load() || assistState.load() != 0)
        return;
    const double rate = preparedRate > 0.0 ? preparedRate : 48000.0;
    const size_t capacity = (size_t) (rate * kMaxAssistSeconds);
    recordL.assign (capacity, 0.0f);
    recordR.assign (capacity, 0.0f);
    recordRate = rate;
    recordPos.store (0);
    assistSeconds.store (0.0);
    assistState.store (1);
}

void MasterProcessor::cancelAssist()
{
    if (assistState.load() == 1)
    {
        assistState.store (2);
        for (int i = 0; i < 40 && assistState.load() != 3; ++i)
            juce::Thread::sleep (5);
    }
    assistState.store (0);
}

void MasterProcessor::finishAssist()
{
    if (assistState.load() != 1 || assistBusy.load())
        return;
    assistState.store (2);
    for (int i = 0; i < 40 && assistState.load() != 3; ++i) // wait for the audio thread to stop recording
        juce::Thread::sleep (5);
    assistState.store (0);

    EngineParams current;
    params.read (current);
    current.order = getModuleOrder();
    const auto target = getTargetCurve();
    const float targetLufs = params.get (P::targetLufs);
    const int n = recordPos.load();

    if (assistThread.joinable())
        assistThread.join();
    assistDone.store (false);
    assistBusy.store (true);
    assistThread = std::thread ([this, current, target, targetLufs, n]
    {
        assistResult = runAssistant (recordL.data(), recordR.data(), n, recordRate, current, target, targetLufs);
        assistDone.store (true);
    });
}

bool MasterProcessor::pollAssist (juce::String& summary)
{
    if (! assistBusy.load() || ! assistDone.load())
        return false;
    if (assistThread.joinable())
        assistThread.join();
    assistBusy.store (false);
    assistDone.store (false);
    recordL = {};
    recordR = {};
    if (! assistResult.ok)
    {
        summary = "Assistant: " + juce::String (assistResult.summary) + " - play the loudest part of the song for 10-30 s";
        return true;
    }
    applyAssistResult (assistResult.params);
    summary = "Assistant: " + juce::String (assistResult.summary);
    return true;
}

void MasterProcessor::applyAssistResult (const EngineParams& p)
{
    setParam (ParamID::inGain, p.inputGainDb);

    setParam (ParamID::eqOn, p.eq.enabled ? 1.0f : 0.0f);
    for (int b = 0; b < kEqBands; ++b)
    {
        const auto k = (size_t) b;
        setParam (BandIDs::eq[b][0], p.eq.freq[k]);
        setParam (BandIDs::eq[b][1], p.eq.gain[k]);
        setParam (BandIDs::eq[b][2], p.eq.q[k]);
        setParam (BandIDs::eq[b][3], (float) p.eq.mode[k]);
    }

    setParam (ParamID::stabOn, p.stab.enabled ? 1.0f : 0.0f);
    setParam (ParamID::stabAmount, p.stab.amount * 100.0f);
    setParam (ParamID::stabSpeed, p.stab.speedMs);
    setParam (ParamID::stabTilt, p.stab.tilt);

    auto nearestIndex = [] (float value, float (*fn) (int) noexcept, int count)
    {
        int best = 0;
        for (int i = 1; i < count; ++i)
            if (std::abs (fn (i) - value) < std::abs (fn (best) - value)) best = i;
        return (float) best;
    };
    setParam (ParamID::glueOn, p.glue.enabled ? 1.0f : 0.0f);
    setParam (ParamID::glueThreshold, p.glue.thresholdDb);
    setParam (ParamID::glueRatio, nearestIndex (p.glue.ratio, glueRatioValue, 4));
    setParam (ParamID::glueAttack, nearestIndex (p.glue.attackMs, glueAttackValue, 6));
    setParam (ParamID::glueRelease, p.glue.releaseMs <= 0.0f ? 4.0f : nearestIndex (p.glue.releaseMs, glueReleaseValue, 4));
    setParam (ParamID::glueMakeup, p.glue.makeupDb);
    setParam (ParamID::glueScHpf, p.glue.scHpfHz);
    setParam (ParamID::glueMix, p.glue.mix * 100.0f);

    setParam (ParamID::mbOn, p.mb.enabled ? 1.0f : 0.0f);
    setParam (ParamID::mbMix, p.mb.mix * 100.0f);
    for (int b = 0; b < kMbBands; ++b)
    {
        const auto k = (size_t) b;
        setParam (BandIDs::mb[b][0], p.mb.threshold[k]);
        setParam (BandIDs::mb[b][1], p.mb.ratio[k]);
        setParam (BandIDs::mb[b][2], p.mb.attackMs[k]);
        setParam (BandIDs::mb[b][3], p.mb.releaseMs[k]);
        setParam (BandIDs::mb[b][4], p.mb.gainDb[k]);
        setParam (BandIDs::mb[b][5], 0.0f);
    }

    setParam (ParamID::imgOn, p.img.enabled ? 1.0f : 0.0f);
    setParam (ParamID::imgMonoBass, p.img.monoBassHz);

    setParam (ParamID::maxOn, 1.0f);
    setParam (ParamID::maxGain, p.max.gainDb);
    setParam (ParamID::maxCeiling, p.max.ceilingDb);
    setParam (ParamID::maxRelease, p.max.releaseMs);
    setParam (ParamID::maxMode, (float) (int) p.max.mode);
    setParam (ParamID::maxTruePeak, p.max.truePeak ? 1.0f : 0.0f);
    setParam (ParamID::maxLink, p.max.link * 100.0f);
    requestLoudnessReset();
}

//==============================================================================
juce::String MasterProcessor::loadReference (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
        return "Could not read " + file.getFileName();

    const auto maxSamples = (juce::int64) (reader->sampleRate * 600.0); // first 10 minutes
    const int n = (int) std::min (reader->lengthInSamples, maxSamples);
    if (n < (int) (reader->sampleRate * 3.0))
        return "Reference is too short (at least 3 seconds)";
    juce::AudioBuffer<float> buf ((int) std::max (1u, std::min (2u, reader->numChannels)), n);
    reader->read (&buf, 0, n, 0, true, true);
    const float* r = buf.getReadPointer (buf.getNumChannels() - 1);
    const auto a = analyseTrack (buf.getReadPointer (0), r, n, reader->sampleRate);
    if (! a.valid)
        return "Reference is silent";

    for (int b = 0; b < kNumBands; ++b) referenceCurve[(size_t) b].store (a.curve[(size_t) b]);
    referenceLufs = a.integratedLufs;
    referenceName = file.getFileNameWithoutExtension();
    referenceValid.store (true);
    storeReferenceInState();
    setParam (ParamID::targetStyle, (float) (int) TargetStyle::reference);
    setParam (ParamID::targetLufs, juce::jlimit (-24.0f, -5.0f, std::round (a.integratedLufs * 2.0f) / 2.0f));
    return "Reference \"" + referenceName + "\": " + juce::String (a.integratedLufs, 1) + " LUFS, " + juce::String (a.truePeakDb, 1)
           + " dBTP, LRA " + juce::String (a.range, 1) + " LU";
}

void MasterProcessor::clearReference()
{
    referenceValid.store (false);
    referenceName = {};
    referenceLufs = kSilenceLufs;
    storeReferenceInState();
    if (juce::roundToInt (params.get (P::targetStyle)) == (int) TargetStyle::reference)
        setParam (ParamID::targetStyle, 0.0f);
}

void MasterProcessor::storeReferenceInState()
{
    auto& s = apvts.state;
    if (! referenceValid.load())
    {
        s.removeProperty ("refName", nullptr);
        s.removeProperty ("refLufs", nullptr);
        s.removeProperty ("refCurve", nullptr);
        return;
    }
    juce::StringArray curve;
    for (auto& v : referenceCurve) curve.add (juce::String (v.load(), 2));
    s.setProperty ("refName", referenceName, nullptr);
    s.setProperty ("refLufs", referenceLufs, nullptr);
    s.setProperty ("refCurve", curve.joinIntoString (","), nullptr);
}

void MasterProcessor::restoreReferenceFromState()
{
    const auto& s = apvts.state;
    juce::StringArray curve;
    curve.addTokens (s.getProperty ("refCurve").toString(), ",", {});
    if (curve.size() != kNumBands)
    {
        referenceValid.store (false);
        referenceName = {};
        return;
    }
    for (int b = 0; b < kNumBands; ++b) referenceCurve[(size_t) b].store (curve[b].getFloatValue());
    referenceName = s.getProperty ("refName").toString();
    referenceLufs = (float) s.getProperty ("refLufs", kSilenceLufs);
    referenceValid.store (true);
}

//==============================================================================
void MasterProcessor::resetToDefaults()
{
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            rp->setValueNotifyingHost (rp->getDefaultValue());
}

void MasterProcessor::setCurrentProgram (int index)
{
    const auto& presets = getFactoryPresets();
    if (! juce::isPositiveAndBelow (index, (int) presets.size()))
        return;
    currentProgram = index;
    currentPresetName = presets[(size_t) index].name;
    const bool keepReference = juce::roundToInt (params.get (P::targetStyle)) == (int) TargetStyle::reference;
    resetToDefaults();
    for (const auto& [id, value] : presets[(size_t) index].values)
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (value));
    if (keepReference && hasReference())
        setParam (ParamID::targetStyle, (float) (int) TargetStyle::reference);
    setModuleOrder (presets[(size_t) index].order.isEmpty() ? defaultModuleOrder()
                                                           : orderFromString (presets[(size_t) index].order.toRawUTF8()));
    requestLoudnessReset();
}

const juce::String MasterProcessor::getProgramName (int index)
{
    const auto& presets = getFactoryPresets();
    return juce::isPositiveAndBelow (index, (int) presets.size()) ? presets[(size_t) index].name : juce::String();
}

juce::File MasterProcessor::getUserPresetFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("LA Audio").getChildFile ("LA-Master").getChildFile ("Presets");
}

bool MasterProcessor::saveUserPreset (const juce::String& name)
{
    const auto clean = juce::File::createLegalFileName (name.trim());
    if (clean.isEmpty())
        return false;
    auto folder = getUserPresetFolder();
    if (! folder.createDirectory())
        return false;
    auto state = apvts.copyState();
    state.setProperty ("order", orderString (getModuleOrder()), nullptr);
    state.setProperty ("presetName", name.trim(), nullptr);
    auto xml = state.createXml();
    if (xml == nullptr || ! xml->writeTo (folder.getChildFile (clean + ".lamaster")))
        return false;
    currentPresetName = name.trim();
    return true;
}

bool MasterProcessor::loadUserPreset (const juce::File& file)
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return false;
    auto tree = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (tree);
    setModuleOrder (orderFromString (tree.getProperty ("order").toString().toRawUTF8()));
    restoreReferenceFromState();
    currentPresetName = tree.getProperty ("presetName", file.getFileNameWithoutExtension()).toString();
    currentProgram = -1;
    requestLoudnessReset();
    return true;
}

void MasterProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("order", orderString (getModuleOrder()), nullptr);
    state.setProperty ("program", currentProgram, nullptr);
    state.setProperty ("presetName", currentPresetName, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MasterProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;
    auto tree = juce::ValueTree::fromXml (*xml);
    currentProgram = (int) tree.getProperty ("program", 0);
    currentPresetName = tree.getProperty ("presetName", "Default").toString();
    apvts.replaceState (tree);
    setModuleOrder (orderFromString (tree.getProperty ("order").toString().toRawUTF8()));
    restoreReferenceFromState();
}

juce::AudioProcessorEditor* MasterProcessor::createEditor()
{
    return new MasterEditor (*this);
}
} // namespace lam

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new lam::MasterProcessor();
}
