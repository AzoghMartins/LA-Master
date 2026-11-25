#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <thread>

#include "Parameters.h"
#include "dsp/Assistant.h"
#include "dsp/MasterEngine.h"

namespace lam
{
/** Lock-free single-producer / single-consumer queue (audio thread -> UI). */
template <typename T, int Capacity>
class SpscQueue
{
public:
    bool push (const T& item) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 + n2 == 0)
            return false;
        data[(size_t) (n1 > 0 ? s1 : s2)] = item;
        fifo.finishedWrite (1);
        return true;
    }

    bool pop (T& item) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (1, s1, n1, s2, n2);
        if (n1 + n2 == 0)
            return false;
        item = data[(size_t) (n1 > 0 ? s1 : s2)];
        fifo.finishedRead (1);
        return true;
    }

private:
    juce::AbstractFifo fifo { Capacity };
    std::array<T, (size_t) Capacity> data {};
};

/** Block-based float FIFO for the analysers. */
class SampleFifo
{
public:
    void push (const float* data, int n) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (n, s1, n1, s2, n2);
        std::copy (data, data + n1, buffer.begin() + s1);
        std::copy (data + n1, data + n1 + n2, buffer.begin() + s2);
        fifo.finishedWrite (n1 + n2);
    }
    int pop (float* dest, int maxN) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (maxN, s1, n1, s2, n2);
        std::copy (buffer.begin() + s1, buffer.begin() + s1 + n1, dest);
        std::copy (buffer.begin() + s2, buffer.begin() + s2 + n2, dest + n1);
        fifo.finishedRead (n1 + n2);
        return n1 + n2;
    }

private:
    static constexpr int size = 1 << 15;
    juce::AbstractFifo fifo { size };
    std::array<float, size> buffer {};
};

/** Meter values published by the audio thread. */
struct AtomicMeters
{
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };
    std::atomic<float> momentary { kSilenceLufs }, shortTerm { kSilenceLufs }, integrated { kSilenceLufs }, range { 0.0f };
    std::atomic<float> inShortTerm { kSilenceLufs }, inIntegrated { kSilenceLufs }, truePeakMax { -100.0f };
    std::atomic<float> glue { 0.0f }, maximizer { 0.0f }, correlation { 1.0f }, match { 0.0f };
    std::array<std::atomic<float>, kMbBands> mb {};
    std::array<std::atomic<float>, kDynEqBands> dynEq {};
    std::array<std::atomic<float>, kNumBands> stab {};
};

class MasterProcessor : public juce::AudioProcessor
{
public:
    MasterProcessor();
    ~MasterProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }

    int getNumPrograms() override { return (int) getFactoryPresets().size(); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- UI interface (message thread) ---------------------------------------------------------
    juce::AudioProcessorValueTreeState& getState() noexcept { return apvts; }

    ModuleOrder getModuleOrder() const noexcept;
    void setModuleOrder (const ModuleOrder& order);

    /** Master Assistant: start listening, then finishAssist() starts the analysis on a worker thread. */
    void startAssist();
    void finishAssist();
    void cancelAssist();
    bool isAssistListening() const noexcept { return assistState.load() == 1; }
    bool isAssistAnalysing() const noexcept { return assistBusy.load(); }
    double getAssistSeconds() const noexcept { return assistSeconds.load(); }
    /** Call regularly on the message thread: applies a finished analysis and returns true with its summary. */
    bool pollAssist (juce::String& summary);

    /** Reference track: its tonal balance becomes the "Reference" target and its loudness the target loudness. */
    juce::String loadReference (const juce::File& file);
    void clearReference();
    bool hasReference() const noexcept { return referenceValid.load(); }
    juce::String getReferenceName() const { return referenceName; }
    float getReferenceLufs() const noexcept { return referenceLufs; }

    /** Target curve currently used by the Stabilizer and the Assistant. */
    BandCurve getTargetCurve() const noexcept;

    void requestLoudnessReset() noexcept { loudnessResetRequest.store (true); }

    // User presets
    static juce::File getUserPresetFolder();
    bool saveUserPreset (const juce::String& name);
    bool loadUserPreset (const juce::File& file);
    juce::String getCurrentPresetName() const { return currentPresetName; }

    double getEngineSampleRate() const noexcept { return engineSampleRate.load(); }

    SpscQueue<LoudnessFrame, 2048> loudnessQueue;
    SampleFifo spectrumIn, spectrumOut, scopeL, scopeR;
    AtomicMeters meters;

private:
    struct Listener : LoudnessListener
    {
        MasterProcessor* owner = nullptr;
        void loudnessFrame (const LoudnessFrame& f) override { owner->loudnessQueue.push (f); }
    };

    void setParam (const char* id, float value);
    /** Sets every parameter in one pass: listed ones to their value, the rest to their default. */
    void applyParameterValues (const std::vector<std::pair<const char*, float>>& values);
    void applyAssistResult (const EngineParams& p);
    void storeReferenceInState();
    void restoreReferenceFromState();

    juce::AudioProcessorValueTreeState apvts;
    ParameterCache params;
    MasterEngine engine;
    EngineParams engineParams;
    Listener listener;

    std::atomic<uint32_t> packedOrder { 0 };
    std::atomic<bool> loudnessResetRequest { false };

    // Reference
    std::array<std::atomic<float>, kNumBands> referenceCurve {};
    std::atomic<bool> referenceValid { false };
    juce::String referenceName;
    float referenceLufs = kSilenceLufs;

    // Assistant
    std::vector<float> recordL, recordR;
    std::atomic<int> recordPos { 0 };
    std::atomic<int> assistState { 0 }; // 0 idle, 1 listening, 2 stop requested, 3 stopped
    std::atomic<double> assistSeconds { 0.0 };
    std::atomic<bool> assistBusy { false }, assistDone { false };
    std::thread assistThread;
    AssistResult assistResult;
    double recordRate = 48000.0;

    std::atomic<double> engineSampleRate { 48000.0 };
    int preparedBlockSize = 512;
    double preparedRate = 0.0;
    int currentProgram = 0;
    juce::String currentPresetName { "Default" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterProcessor)
};
} // namespace lam
