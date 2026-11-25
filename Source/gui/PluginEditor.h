#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "Displays.h"
#include "Theme.h"
#include "Widgets.h"

namespace lam
{
/** One band of a multiband module: a titled column of controls with an optional gain-reduction bar. */
class BandStrip : public juce::Component
{
public:
    BandStrip (const juce::String& title, juce::Colour colour, std::atomic<float>* reductionDb = nullptr);
    void paint (juce::Graphics&) override;
    void resized() override;
    void setItems (std::vector<juce::Component*> items, int columns) { content = std::move (items); cols = columns; resized(); }
    void setTitle (const juce::String& t) { if (t != title) { title = t; repaint(); } }

private:
    juce::String title;
    juce::Colour colour;
    std::atomic<float>* reduction = nullptr;
    std::vector<juce::Component*> content;
    int cols = 2;
    float shownGr = 0.0f;
};

/** All UI content, laid out at a fixed base size and scaled by the editor. */
class MainPanel : public juce::Component
{
public:
    static constexpr int baseWidth = 1440, baseHeight = 780;

    explicit MainPanel (MasterProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void tick();

    void setPage (int page);
    int getPage() const { return currentPage; }
    void showToast (const juce::String& text);
    static void grid (std::vector<juce::Component*> items, int columns, juce::Rectangle<int> area);

private:
    Knob* knob (juce::Component& parent, const char* id, const juce::String& name, juce::Colour c, bool bipolar = false);
    Choice* choice (juce::Component& parent, const char* id, const juce::String& caption = {});
    void selectEqBand (int band);
    void selectDynBand (int band);
    void refreshPresetList();
    void refreshTargetBox();
    void focusSection (const juce::String& key);
    void showReferenceMenu();
    void moveGainToTarget();

    MasterProcessor& processor;
    juce::AudioProcessorValueTreeState& state;
    std::vector<std::unique_ptr<juce::Component>> owned;

    // Header
    MenuBox presetBox, targetBox;
    std::unique_ptr<Choice> styleChoice;
    PillButton saveButton { "SAVE" }, assistButton { "ASSIST", Colours::learn }, refButton { "REF", Colours::learn };
    std::unique_ptr<ParamPill> comparePill, matchPill, deltaPill;
    juce::File userPresetFiles[256];
    int numUserPresets = 0;
    std::unique_ptr<juce::FileChooser> chooser;

    // Displays
    ChainStrip chain;
    SpectrumDisplay spectrum;
    LoudnessHistory history;
    StereoScope scope;
    LoudnessPanel loudness;
    LevelMeters meters;

    // Pages
    PillButton tabTone { "TONE" }, tabDynamics { "DYNAMICS" }, tabStereo { "STEREO & CHARACTER" };
    int currentPage = 0;

    Section inputSection, outputSection;
    Section eqSection, dynEqSection, stabSection;
    Section glueSection, mbSection, maxSection;
    Section excSection, imgSection;
    std::map<juce::String, Section*> sectionByKey;

    std::vector<Knob*> inputKnobs, outputKnobs, stabKnobs, glueKnobs, maxKnobs;
    Choice *ditherChoice = nullptr, *monitorChoice = nullptr, *stabStyle = nullptr;
    Choice *glueRatio = nullptr, *glueAttack = nullptr, *glueRelease = nullptr, *maxMode = nullptr, *excMode = nullptr;

    std::array<std::array<Knob*, 3>, kEqBands> eqKnobs {};
    std::array<Choice*, kEqBands> eqMode {};
    std::array<std::unique_ptr<PillButton>, kEqBands> eqBandButtons;
    Knob *eqHpf = nullptr, *eqLpf = nullptr;
    int eqBand = 2;

    std::array<std::array<Knob*, 4>, kDynEqBands> dynKnobs {};
    std::array<Choice*, kDynEqBands> dynMode {};
    std::array<std::unique_ptr<PillButton>, kDynEqBands> dynBandButtons;
    Knob *dynAttack = nullptr, *dynRelease = nullptr;
    int dynBand = 0;

    std::array<std::unique_ptr<BandStrip>, kMbBands> mbStrips, excStrips, imgStrips;
    std::array<std::unique_ptr<ParamPill>, kMbBands> soloPills;
    std::vector<Knob*> mbTop, excTop, imgTop;
    std::unique_ptr<ParamPill> truePeakPill;
    PillButton toTargetButton { "TO TARGET", Colours::learn };
    juce::Label referenceLabel;

    int blink = 0, toastTicks = 0;
    juce::String infoText, toastText;
    juce::String highlightKey;
    float highlightAmount = 0.0f;
};

class MasterEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MasterEditor (MasterProcessor&);
    ~MasterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Pulls new data from the audio thread and refreshes the displays (called at 30 Hz). */
    void updateDisplays() { panel.tick(); }
    MainPanel& getPanel() { return panel; }

private:
    void timerCallback() override { updateDisplays(); }

    MasterLookAndFeel lookAndFeel;
    MainPanel panel;
    juce::TooltipWindow tooltips { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterEditor)
};
} // namespace lam
