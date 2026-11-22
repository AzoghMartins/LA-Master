// Integration test: drives the real LA-Master processor + editor the way a host would.
//  - checks bus layouts, latency, state save/restore (module order, reference), presets, user presets,
//    reference loading, the Assistant, compare/delta, and renders the editor:  lam_integration [screenshot.png [page]]
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../Source/PluginProcessor.h"
#include "../Source/gui/PluginEditor.h"

#include <random>

using namespace lam;

namespace
{
int failures = 0;
void check (bool ok, const juce::String& what)
{
    std::printf ("%s %s\n", ok ? "  ok  " : "  FAIL", what.toRawUTF8());
    if (! ok)
        ++failures;
}

constexpr double sr = 48000.0;

/** A small "song": kick, bass, chords, hats; slightly wide pads and a wide sub. */
juce::AudioBuffer<float> makeMix (double seconds, float amp = 0.3f, unsigned seed = 1)
{
    juce::AudioBuffer<float> buf (2, (int) (seconds * sr));
    std::mt19937 rng (seed);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    const double pi = juce::MathConstants<double>::pi, beat = 0.5;
    const float chord[] = { 220.0f, 261.63f, 329.63f, 392.0f };
    float hpL = 0.0f, hpR = 0.0f, prevL = 0.0f, prevR = 0.0f;
    for (int i = 0; i < buf.getNumSamples(); ++i)
    {
        const double t = i / sr, tb = std::fmod (t, beat);
        const float kick = (float) (std::exp (-tb * 16.0) * std::sin (2.0 * pi * (48.0 + 90.0 * std::exp (-tb * 28.0)) * tb));
        const float bassL = 0.3f * (float) std::sin (2.0 * pi * 55.0 * t), bassR = 0.3f * (float) std::sin (2.0 * pi * 55.0 * t + 0.6);
        float padL = 0.0f, padR = 0.0f;
        for (size_t k = 0; k < 4; ++k)
        {
            padL += 0.06f * (float) std::sin (2.0 * pi * chord[k] * t);
            padR += 0.06f * (float) std::sin (2.0 * pi * chord[k] * 1.003 * t);
        }
        const double th = std::fmod (t + 0.25, beat);
        const float env = (float) std::exp (-th * 50.0);
        const float nl = nd (rng), nr = nd (rng);
        hpL = 0.6f * (hpL + nl - prevL); prevL = nl;
        hpR = 0.6f * (hpR + nr - prevR); prevR = nr;
        buf.setSample (0, i, amp * (0.9f * kick + bassL + padL + 0.2f * env * hpL));
        buf.setSample (1, i, amp * (0.9f * kick + bassR + padR + 0.2f * env * hpR));
    }
    return buf;
}

void run (MasterProcessor& proc, juce::AudioBuffer<float>& audio, int block, MasterEditor* editor = nullptr)
{
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    for (int start = 0; start + block <= audio.getNumSamples(); start += block)
    {
        for (int ch = 0; ch < 2; ++ch)
            buf.copyFrom (ch, 0, audio, ch, start, block);
        proc.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch)
            audio.copyFrom (ch, start, buf, ch, 0, block);
        if (editor != nullptr && (start / block) % 8 == 0)
            editor->updateDisplays();
    }
}

void setParam (MasterProcessor& p, const char* id, float value)
{
    auto* param = p.getState().getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

float param (MasterProcessor& p, const char* id) { return p.getState().getRawParameterValue (id)->load(); }

float integratedLufs (const juce::AudioBuffer<float>& b, int from)
{
    LoudnessMeter m;
    m.prepare (sr);
    m.process (b.getReadPointer (0) + from, b.getReadPointer (1) + from, b.getNumSamples() - from);
    return m.getIntegrated();
}

float truePeak (const juce::AudioBuffer<float>& b)
{
    TruePeakDetector l, r;
    float p = 0.0f;
    for (int i = 0; i < b.getNumSamples(); ++i)
        p = std::max ({ p, l.process (b.getSample (0, i)), r.process (b.getSample (1, i)) });
    return gainToDb (p);
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("LA-Master plugin integration test\n");

    auto proc = std::make_unique<MasterProcessor>();
    {
        juce::AudioProcessor::BusesLayout mono, stereo, monoToStereo, bad;
        mono.inputBuses.add (juce::AudioChannelSet::mono()); mono.outputBuses.add (juce::AudioChannelSet::mono());
        stereo.inputBuses.add (juce::AudioChannelSet::stereo()); stereo.outputBuses.add (juce::AudioChannelSet::stereo());
        monoToStereo.inputBuses.add (juce::AudioChannelSet::mono()); monoToStereo.outputBuses.add (juce::AudioChannelSet::stereo());
        bad.inputBuses.add (juce::AudioChannelSet::create5point1()); bad.outputBuses.add (juce::AudioChannelSet::create5point1());
        check (proc->checkBusesLayoutSupported (mono) && proc->checkBusesLayoutSupported (stereo)
                   && proc->checkBusesLayoutSupported (monoToStereo) && ! proc->checkBusesLayoutSupported (bad),
               "supports mono, stereo and mono->stereo; rejects 5.1");
    }

    proc->setPlayConfigDetails (2, 2, sr, 512);
    proc->prepareToPlay (sr, 512);
    const int latency = proc->getLatencySamples();
    check (latency > 0 && latency < sr * 0.005, "reports latency " + juce::String (1000.0 * latency / sr, 2) + " ms");

    // ---- Presets -------------------------------------------------------------------------------------
    bool presetsOk = proc->getNumPrograms() >= 14;
    for (int i = 0; i < proc->getNumPrograms(); ++i)
    {
        proc->setCurrentProgram (i);
        proc->prepareToPlay (sr, 512);
        auto audio = makeMix (3.0);
        run (*proc, audio, 512);
        bool finite = true;
        for (int ch = 0; ch < 2; ++ch)
            for (int s = 0; s < audio.getNumSamples(); ++s)
                finite &= std::isfinite (audio.getSample (ch, s));
        const float peak = audio.getMagnitude (0, audio.getNumSamples());
        const float ceiling = juce::Decibels::decibelsToGain (param (*proc, ParamID::maxCeiling));
        const bool ok = finite && peak <= ceiling + 1.0e-4f && peak > 0.05f;
        presetsOk &= ok;
        if (! ok)
            std::printf ("        preset %s: peak %.3f finite %d\n", proc->getProgramName (i).toRawUTF8(), peak, (int) finite);
    }
    check (presetsOk, juce::String (proc->getNumPrograms()) + " factory presets all produce clean output under the ceiling");
    proc->setCurrentProgram (2);
    check (proc->getProgramName (2) == "Loud Pop" && std::abs (param (*proc, ParamID::targetLufs) + 9.0f) < 0.01f && param (*proc, ParamID::excOn) > 0.5f,
           "factory preset applies (Loud Pop: -9 LUFS target, exciter on)");
    proc->setCurrentProgram (0);
    proc->prepareToPlay (sr, 512);

    // ---- State round trip ----------------------------------------------------------------------------------
    {
        setParam (*proc, ParamID::eq3Gain, -2.5f);
        setParam (*proc, ParamID::deq2Range, -4.0f);
        setParam (*proc, ParamID::mb2Ratio, 3.3f);
        ModuleOrder o = defaultModuleOrder();
        std::reverse (o.begin(), o.end());
        proc->setModuleOrder (o);
        juce::MemoryBlock data;
        proc->getStateInformation (data);
        MasterProcessor copy;
        copy.setStateInformation (data.getData(), (int) data.getSize());
        bool same = true;
        for (auto* p : proc->getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                same &= std::abs (rp->getValue() - copy.getState().getParameter (rp->paramID)->getValue()) < 1.0e-5f;
        check (same, "state save/restore round trip (" + juce::String (proc->getParameters().size()) + " parameters)");
        check (copy.getModuleOrder() == o, "module order is saved with the session");
        proc->setModuleOrder (defaultModuleOrder());
        proc->setCurrentProgram (0);
    }

    // ---- User preset save / load ----------------------------------------------------------------------------
    {
        setParam (*proc, ParamID::glueMix, 37.0f);
        const bool saved = proc->saveUserPreset ("Integration Test Preset");
        setParam (*proc, ParamID::glueMix, 100.0f);
        const auto file = MasterProcessor::getUserPresetFolder().getChildFile ("Integration Test Preset.lamaster");
        const bool loaded = proc->loadUserPreset (file);
        check (saved && loaded && std::abs (param (*proc, ParamID::glueMix) - 37.0f) < 0.01f, "user preset save and load");
        file.deleteFile();
        proc->setCurrentProgram (0);
    }

    // ---- Reference track ---------------------------------------------------------------------------------------
    {
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("lam_reference_test.wav");
        tmp.deleteFile();
        auto ref = makeMix (8.0, 0.5f, 9);
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (tmp), sr, 2, 24, {}, 0));
            w->writeFromAudioSampleBuffer (ref, 0, ref.getNumSamples());
        }
        const auto msg = proc->loadReference (tmp);
        std::printf ("        %s\n", msg.toRawUTF8());
        const float refLufs = integratedLufs (ref, 0);
        check (proc->hasReference() && juce::roundToInt (param (*proc, ParamID::targetStyle)) == (int) TargetStyle::reference
                   && std::abs (param (*proc, ParamID::targetLufs) - std::round (refLufs * 2.0f) / 2.0f) < 0.01f,
               "reference loads: target style = Reference, target loudness = its loudness");

        juce::MemoryBlock data;
        proc->getStateInformation (data);
        MasterProcessor copy;
        copy.setStateInformation (data.getData(), (int) data.getSize());
        const auto a = proc->getTargetCurve(), b = copy.getTargetCurve();
        float diff = 0.0f;
        for (int k = 0; k < kNumBands; ++k) diff = std::max (diff, std::abs (a[(size_t) k] - b[(size_t) k]));
        check (copy.hasReference() && copy.getReferenceName() == "lam_reference_test" && diff < 0.02f, "reference is saved with the session");
        proc->clearReference();
        check (! proc->hasReference() && juce::roundToInt (param (*proc, ParamID::targetStyle)) == 0, "reference can be cleared");
        tmp.deleteFile();
        proc->setCurrentProgram (0);
    }

    // ---- Assistant ----------------------------------------------------------------------------------------------
    {
        proc->prepareToPlay (sr, 512);
        setParam (*proc, ParamID::targetLufs, -11.0f);
        auto audio = makeMix (15.0, 0.12f, 3);
        proc->startAssist();
        run (*proc, audio, 512);
        check (proc->isAssistListening() && proc->getAssistSeconds() > 14.0, "assistant records while listening");
        proc->finishAssist();
        juce::String summary;
        bool done = false;
        for (int i = 0; i < 600 && ! done; ++i)
        {
            done = proc->pollAssist (summary);
            if (! done) juce::Thread::sleep (10);
        }
        std::printf ("        %s\n", summary.toRawUTF8());
        check (done && param (*proc, ParamID::maxGain) > 1.0f && param (*proc, ParamID::glueOn) > 0.5f && param (*proc, ParamID::stabOn) > 0.5f,
               "assistant finishes on its worker thread and applies its settings");

        proc->prepareToPlay (sr, 512);
        auto song = makeMix (15.0, 0.12f, 3);
        run (*proc, song, 512);
        const float lufs = integratedLufs (song, (int) sr * 3);
        check (std::abs (lufs + 11.0f) < 1.0f && truePeak (song) < -0.8f,
               "assistant result hits the -11 LUFS target (" + juce::String (lufs, 2) + " LUFS, " + juce::String (truePeak (song), 2) + " dBTP)");
        check (std::abs (proc->meters.integrated.load() - integratedLufs (song, 0)) < 0.5f, "plugin loudness meter agrees with an offline measurement");

        // Compare: level-matched input
        setParam (*proc, ParamID::bypass, 1.0f);
        auto cmp = makeMix (15.0, 0.12f, 3);
        run (*proc, cmp, 512);
        const float cmpLufs = integratedLufs (cmp, (int) sr * 5);
        check (std::abs (cmpLufs - lufs) < 1.5f, "COMPARE with MATCH plays the input at the master's loudness (" + juce::String (cmpLufs, 2) + " LUFS)");
        setParam (*proc, ParamID::gainMatch, 0.0f);
        auto raw = makeMix (6.0, 0.12f, 3);
        auto rawCopy = raw;
        run (*proc, raw, 512);
        float err = 0.0f;
        const int processed = raw.getNumSamples() / 512 * 512; // run() only processes whole blocks
        for (int i = latency + 512; i < processed; ++i)
            err = std::max (err, std::abs (raw.getSample (0, i) - rawCopy.getSample (0, i - latency)));
        check (err < 1.0e-4f, "COMPARE without MATCH is the untouched, latency-aligned input (max error " + juce::String (err, 6) + ")");
        setParam (*proc, ParamID::bypass, 0.0f);
        setParam (*proc, ParamID::gainMatch, 1.0f);
        proc->setCurrentProgram (0);
    }

    // ---- Silence assistant ----------------------------------------------------------------------------------------
    {
        juce::AudioBuffer<float> silence (2, (int) sr * 3);
        silence.clear();
        proc->startAssist();
        run (*proc, silence, 512);
        proc->finishAssist();
        juce::String summary;
        bool done = false;
        for (int i = 0; i < 300 && ! done; ++i) { done = proc->pollAssist (summary); if (! done) juce::Thread::sleep (10); }
        check (done && summary.contains ("not enough audio") && std::abs (param (*proc, ParamID::maxGain)) < 0.01f,
               "assistant leaves the settings alone when it hears nothing");
    }

    // ---- Editor ---------------------------------------------------------------------------------------------------
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditorIfNeeded());
    auto* editor = dynamic_cast<MasterEditor*> (ed.get());
    check (editor != nullptr, "editor created");
    {
        proc->setCurrentProgram (1);
        proc->prepareToPlay (sr, 512);
        for (int page = 0; page < 3; ++page)
        {
            editor->getPanel().setPage (page);
            auto audio = makeMix (2.0);
            run (*proc, audio, 480, editor);
        }
        check (true, "all three pages render with audio running");
    }

    if (argc > 1 && editor != nullptr)
    {
        proc->setCurrentProgram (2); // Loud Pop
        setParam (*proc, ParamID::deqOn, 1.0f);
        setParam (*proc, ParamID::deq3Range, -4.0f);
        setParam (*proc, ParamID::deq3Thr, -40.0f);
        setParam (*proc, ParamID::eq3Gain, -1.5f);
        setParam (*proc, ParamID::eq5Gain, 1.2f);
        setParam (*proc, ParamID::targetStyle, 1.0f);
        proc->prepareToPlay (sr, 512);
        editor->getPanel().setPage (argc > 2 ? juce::String (argv[2]).getIntValue() : 0);
        auto audio = makeMix (14.0, 0.3f, 4);
        run (*proc, audio, 480, editor);
        editor->setSize (MainPanel::baseWidth, MainPanel::baseHeight);
        for (int i = 0; i < 10; ++i)
            editor->updateDisplays();
        auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
        out.deleteFile();
        juce::FileOutputStream stream (out);
        juce::PNGImageFormat png;
        check (stream.openedOk() && png.writeImageToStream (image, stream), "screenshot written to " + out.getFullPathName());
    }

    ed.reset();
    proc.reset();
    std::printf (failures == 0 ? "All integration checks passed\n" : "%d integration check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
