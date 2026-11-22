#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "../dsp/MasterEngine.h"
#include "Theme.h"

namespace lam
{
class MasterProcessor;

/** Colour used for each orderable module. */
juce::Colour moduleColour (ModuleId m);

//==============================================================================
/** Input / output spectrum with the EQ curve (draggable), dynamic EQ bands, Stabilizer curve and target balance. */
class SpectrumDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit SpectrumDisplay (MasterProcessor&);

    void pullSpectrum();
    void setSelectedEqBand (int b) { selectedEq = b; repaint(); }
    void setSelectedDynBand (int b) { selectedDyn = b; repaint(); }
    std::function<void (int)> onEqBandSelected, onDynBandSelected;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Hit { int kind = -1; int band = -1; }; // kind 0 = EQ, 1 = dynamic EQ

    juce::Rectangle<float> plotArea() const;
    float freqToX (float f, juce::Rectangle<float> a) const;
    float xToFreq (float x, juce::Rectangle<float> a) const;
    float dbToY (float db, juce::Rectangle<float> a) const;
    float yToDb (float y, juce::Rectangle<float> a) const;
    Hit hitNode (juce::Point<float> p) const;
    juce::Point<float> nodePosition (Hit h, juce::Rectangle<float> a) const;
    juce::RangedAudioParameter* param (Hit h, int which) const;
    float value (const char* id) const;
    void analyse (std::vector<float>& fifo, int& fifoPos, std::vector<float>& smoothed);

    MasterProcessor& processor;
    juce::AudioProcessorValueTreeState& state;

    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> fifoIn, fifoOut, fftData, window, specIn, specOut;
    int posIn = 0, posOut = 0;

    int selectedEq = 2, selectedDyn = 0;
    Hit dragging, hover;
    static constexpr float minDb = -15.0f, maxDb = 15.0f;
};

//==============================================================================
/** Scrolling loudness history: momentary and short-term output, short-term input, maximizer gain reduction and target. */
class LoudnessHistory : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit LoudnessHistory (MasterProcessor&);
    void addFrame (const LoudnessFrame& f);
    void paint (juce::Graphics&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    MasterProcessor& processor;
    static constexpr int capacity = 1200; // 120 s at 10 frames per second
    std::vector<LoudnessFrame> frames;
    int64_t count = 0;
    double spanSeconds = 30.0;
    static constexpr float lo = -42.0f, hi = 0.0f;
};

//==============================================================================
/** Vectorscope (goniometer) and correlation meter. */
class StereoScope : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit StereoScope (MasterProcessor&);
    void pull();
    void paint (juce::Graphics&) override;

private:
    MasterProcessor& processor;
    static constexpr int points = 2048;
    std::vector<float> l, r;
    int pos = 0;
    float correlation = 1.0f, width = 0.0f;
};

//==============================================================================
/** Big loudness numbers: integrated, short-term, momentary, range, true peak, PLR. Click to reset. */
class LoudnessPanel : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit LoudnessPanel (MasterProcessor&);
    void tick();
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    MasterProcessor& processor;
    float integrated = kSilenceLufs, shortTerm = kSilenceLufs, momentary = kSilenceLufs, range = 0.0f, truePeak = -100.0f;
    float inIntegrated = kSilenceLufs;
};

//==============================================================================
/** Signal-flow strip: every stage as a tile. The module tiles can be dragged to change their order. */
class ChainStrip : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum Page { pageTone = 0, pageDynamics = 1, pageStereo = 2 };

    explicit ChainStrip (MasterProcessor& p);
    void tick();
    void paint (juce::Graphics&) override;
    void resized() override { layoutTiles(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    /** Called with (page, sectionKey) when a tile is clicked (page -1 = stay on the current page). */
    std::function<void (int, const juce::String&)> onTileClicked;
    std::function<void()> onOrderChanged;

    static int pageOf (ModuleId m);

private:
    struct Tile
    {
        juce::String key, name, enableId;
        juce::Colour colour;
        int page = 0;
        int module = -1; // orderable ModuleId, -1 = fixed
        juce::Rectangle<float> bounds;
    };

    void rebuildTiles();
    void layoutTiles();
    juce::String tileValue (const Tile& t) const;
    bool tileEnabled (const Tile& t) const;
    juce::Rectangle<float> powerRect (const Tile& t) const;

    MasterProcessor& processor;
    juce::AudioProcessorValueTreeState& state;
    std::vector<Tile> tiles;
    ModuleOrder order;
    int dragTile = -1, dragTarget = -1, hoverTile = -1;
    float dragX = 0.0f, dragOffset = 0.0f;
    bool dragMoved = false;
};
} // namespace lam
