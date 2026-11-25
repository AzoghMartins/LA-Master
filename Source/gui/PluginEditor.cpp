#include "PluginEditor.h"

namespace lam
{
namespace
{
const char* eqBandNames[kEqBands] = { "LOW", "L-MID", "MID", "U-MID", "PRES", "HIGH" };
const char* bandNames[kMbBands] = { "LOW", "MID", "HIGH" };

struct TargetPreset { float lufs; const char* label; };
const TargetPreset targetPresets[] = { { -23.0f, "Broadcast (EBU R128)" }, { -16.0f, "Apple Music / Podcast" },
                                       { -14.0f, "Spotify / YouTube / Tidal" }, { -11.0f, "Loud streaming" },
                                       { -9.0f, "Loud master (CD)" }, { -7.0f, "Club / EDM" } };

juce::String hzText (float v)
{
    return v >= 1000.0f ? juce::String (v / 1000.0f, 1) + "k" : juce::String (juce::roundToInt (v));
}
} // namespace

//==============================================================================
BandStrip::BandStrip (const juce::String& t, juce::Colour c, std::atomic<float>* gr) : title (t), colour (c), reduction (gr) {}

void BandStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f, 0.0f);
    g.setColour (juce::Colours::black.withAlpha (0.28f));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawLine (r.getX() + 5.0f, r.getBottom() - 0.5f, r.getRight() - 5.0f, r.getBottom() - 0.5f, 1.0f);
    Hardware::drawLed (g, { r.getX() + 7.0f, 10.0f, 6.0f, 6.0f }, colour, true);
    g.setColour (Colours::text);
    g.setFont (uiFont (11.0f, true));
    g.drawText (title, 20, 4, getWidth() - 24, 18, juce::Justification::centredLeft);
    if (reduction != nullptr)
    {
        shownGr = juce::jmax (reduction->load(), shownGr - 0.4f);
        auto bar = juce::Rectangle<float> (r.getRight() - 64.0f, 8.0f, 56.0f, 9.0f);
        Hardware::drawRecess (g, bar, 2.0f);
        g.setColour (Colours::danger);
        g.fillRoundedRectangle (bar.reduced (1.0f).withWidth ((bar.getWidth() - 2.0f) * juce::jlimit (0.0f, 1.0f, shownGr / 12.0f)), 1.5f);
        g.setFont (uiFont (9.0f));
        g.setColour (Colours::textDim);
        g.drawText (juce::String (shownGr, 1), bar.translated (-34.0f, 0.0f).withWidth (30.0f), juce::Justification::centredRight);
    }
}

void BandStrip::resized()
{
    auto r = getLocalBounds().reduced (2, 0);
    r.removeFromTop (24);
    MainPanel::grid (content, cols, r);
}

//==============================================================================
MainPanel::MainPanel (MasterProcessor& p)
    : processor (p),
      state (p.getState()),
      chain (p),
      spectrum (p),
      history (p),
      scope (p),
      loudness (p),
      inputSection ("Input"),
      outputSection ("Output"),
      eqSection ("EQ", &state, ParamID::eqOn, moduleColour (ModuleId::eq)),
      dynEqSection ("Dynamic EQ", &state, ParamID::deqOn, moduleColour (ModuleId::dyneq)),
      stabSection ("Stabilizer", &state, ParamID::stabOn, moduleColour (ModuleId::stabilizer)),
      glueSection ("Glue", &state, ParamID::glueOn, moduleColour (ModuleId::glue)),
      mbSection ("Dynamics", &state, ParamID::mbOn, moduleColour (ModuleId::dynamics)),
      maxSection ("Maximizer", &state, ParamID::maxOn, Colours::danger),
      excSection ("Exciter", &state, ParamID::excOn, moduleColour (ModuleId::exciter)),
      imgSection ("Imager", &state, ParamID::imgOn, moduleColour (ModuleId::imager))
{
    // ---- Header ----------------------------------------------------------------------------------
    refreshPresetList();
    presetBox.setTooltip ("Factory and user presets");
    presetBox.onItemChosen = [this] (int id)
    {
        if (id > 0 && id < 1000)
            processor.setCurrentProgram (id - 1);
        else if (id >= 1000 && id - 1000 < numUserPresets)
            processor.loadUserPreset (userPresetFiles[id - 1000]);
    };
    addAndMakeVisible (presetBox);

    saveButton.setTooltip ("Save the current settings as a user preset");
    saveButton.onClick = [this]
    {
        auto* w = new juce::AlertWindow ("Save preset", "Name for the new preset:", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", processor.getCurrentPresetName());
        w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int r)
        {
            if (r == 1)
            {
                const auto name = w->getTextEditorContents ("name");
                showToast (processor.saveUserPreset (name) ? "Saved preset \"" + name + "\"" : juce::String ("Could not save the preset"));
                refreshPresetList();
            }
        }), true);
    };
    addAndMakeVisible (saveButton);

    assistButton.setTooltip ("Master Assistant: press, play the loudest part of the song for 10-30 seconds, press again. "
                             "Sets gain staging, EQ, Stabilizer, Glue, Dynamics, mono bass and the Maximizer for the target style and loudness");
    assistButton.onClick = [this]
    {
        if (processor.isAssistAnalysing())
            return;
        if (processor.isAssistListening())
            processor.finishAssist();
        else
            processor.startAssist();
    };
    addAndMakeVisible (assistButton);

    styleChoice = std::make_unique<Choice> (state, ParamID::targetStyle);
    styleChoice->box.setTooltip ("Target tonal balance for the Stabilizer and the Assistant (Reference = the loaded reference track)");
    addAndMakeVisible (*styleChoice);

    refreshTargetBox();
    targetBox.setTooltip ("Target loudness (integrated LUFS) for the Assistant, the TO TARGET button and the displays");
    targetBox.onItemChosen = [this] (int id)
    {
        if (id <= 0)
            return;
        if (id == 99)
        {
            auto* w = new juce::AlertWindow ("Target loudness", "Integrated loudness target (LUFS, -24 to -5):", juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("lufs", juce::String (state.getRawParameterValue (ParamID::targetLufs)->load(), 1));
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int r)
            {
                if (r == 1)
                    if (auto* prm = state.getParameter (ParamID::targetLufs))
                        prm->setValueNotifyingHost (prm->convertTo0to1 (juce::jlimit (-24.0f, -5.0f, w->getTextEditorContents ("lufs").getFloatValue())));
                refreshTargetBox();
            }), true);
            return;
        }
        if (auto* prm = state.getParameter (ParamID::targetLufs))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->convertTo0to1 (targetPresets[id - 1].lufs));
            prm->endChangeGesture();
        }
        refreshTargetBox();
    };
    addAndMakeVisible (targetBox);

    refButton.setTooltip ("Reference track: load a song whose tonal balance and loudness you want to match");
    refButton.onClick = [this] { showReferenceMenu(); };
    addAndMakeVisible (refButton);

    comparePill = std::make_unique<ParamPill> (state, ParamID::bypass, "COMPARE", Colours::detection);
    comparePill->setTooltip ("Listen to the unprocessed input (with MATCH: at the same loudness as the master)");
    matchPill = std::make_unique<ParamPill> (state, ParamID::gainMatch, "MATCH", Colours::detection);
    matchPill->setTooltip ("Gain match: COMPARE and DELTA play at the master's loudness, so you judge tone and dynamics, not level");
    deltaPill = std::make_unique<ParamPill> (state, ParamID::delta, "DELTA", Colours::pink);
    deltaPill->setTooltip ("Listen only to what the chain changes (master minus level-matched input)");
    for (auto* b : { comparePill.get(), matchPill.get(), deltaPill.get() })
        addAndMakeVisible (*b);

    // ---- Displays -----------------------------------------------------------------------------------
    addAndMakeVisible (chain);
    chain.onTileClicked = [this] (int page, const juce::String& key)
    {
        if (page >= 0) setPage (page);
        focusSection (key);
    };
    addAndMakeVisible (spectrum);
    spectrum.onEqBandSelected = [this] (int b) { setPage (0); selectEqBand (b); };
    spectrum.onDynBandSelected = [this] (int b) { setPage (0); selectDynBand (b); };
    addAndMakeVisible (history);
    addAndMakeVisible (scope);
    addAndMakeVisible (loudness);
    addAndMakeVisible (meters);

    // ---- Tabs -------------------------------------------------------------------------------------------
    int tabIndex = 0;
    for (auto* t : { &tabTone, &tabDynamics, &tabStereo })
    {
        t->setRadioGroupId (77);
        t->setClickingTogglesState (true);
        const int page = tabIndex++;
        t->onClick = [this, page] { setPage (page); };
        addAndMakeVisible (*t);
    }

    for (auto* s : { &inputSection, &outputSection, &eqSection, &dynEqSection, &stabSection, &glueSection, &mbSection, &maxSection,
                     &excSection, &imgSection })
        addChildComponent (*s);
    inputSection.setVisible (true);
    outputSection.setVisible (true);

    // ---- Input / output ------------------------------------------------------------------------------
    inputKnobs = { knob (inputSection, ParamID::inGain, "Gain", Colours::accent, true),
                   knob (inputSection, ParamID::balance, "Balance", Colours::accent, true),
                   knob (inputSection, ParamID::subsonic, "Subsonic", Colours::threshold) };
    outputKnobs = { knob (outputSection, ParamID::outGain, "Gain", Colours::accent, true) };
    ditherChoice = choice (outputSection, ParamID::dither, "Dither");
    ditherChoice->box.setTooltip ("Dither for the final bit depth: use 16-bit (Shaped) when you export a 16-bit file");
    monitorChoice = choice (outputSection, ParamID::monitor, "Monitor");
    monitorChoice->box.setTooltip ("Listen in mono, to the side signal only, or to one channel (also affects the output!)");

    // ---- TONE: EQ ----------------------------------------------------------------------------------------
    const auto ce = moduleColour (ModuleId::eq);
    for (int b = 0; b < kEqBands; ++b)
    {
        const bool shelf = b == 0 || b == kEqBands - 1;
        eqKnobs[(size_t) b] = { knob (eqSection, BandIDs::eq[b][0], "Freq", ce), knob (eqSection, BandIDs::eq[b][1], "Gain", ce, true),
                                knob (eqSection, BandIDs::eq[b][2], shelf ? "Slope" : "Q", ce) };
        eqMode[(size_t) b] = choice (eqSection, BandIDs::eq[b][3], "Channel");
        eqMode[(size_t) b]->box.setTooltip ("Process the left and right channels (Stereo), only the centre (Mid) or only the sides (Side)");
        eqBandButtons[(size_t) b] = std::make_unique<PillButton> (eqBandNames[b], ce);
        eqBandButtons[(size_t) b]->setClickingTogglesState (false);
        eqBandButtons[(size_t) b]->onClick = [this, b] { selectEqBand (b); };
        eqSection.addAndMakeVisible (*eqBandButtons[(size_t) b]);
    }
    eqHpf = knob (eqSection, ParamID::eqHpf, "Low Cut", ce);
    eqLpf = knob (eqSection, ParamID::eqLpf, "High Cut", ce);

    // ---- TONE: Dynamic EQ ---------------------------------------------------------------------------------
    const auto cd = moduleColour (ModuleId::dyneq);
    for (int b = 0; b < kDynEqBands; ++b)
    {
        dynKnobs[(size_t) b] = { knob (dynEqSection, BandIDs::dynEq[b][0], "Freq", cd), knob (dynEqSection, BandIDs::dynEq[b][1], "Q", cd),
                                 knob (dynEqSection, BandIDs::dynEq[b][2], "Threshold", Colours::threshold),
                                 knob (dynEqSection, BandIDs::dynEq[b][3], "Range", cd, true) };
        dynKnobs[(size_t) b][3]->slider.setTooltip ("How much the band cuts (negative) or boosts (positive) while it is above the threshold. 0 = band off");
        dynMode[(size_t) b] = choice (dynEqSection, BandIDs::dynEq[b][4], "Channel");
        dynBandButtons[(size_t) b] = std::make_unique<PillButton> ("D" + juce::String (b + 1), cd);
        dynBandButtons[(size_t) b]->setClickingTogglesState (false);
        dynBandButtons[(size_t) b]->onClick = [this, b] { selectDynBand (b); };
        dynEqSection.addAndMakeVisible (*dynBandButtons[(size_t) b]);
    }
    dynAttack = knob (dynEqSection, ParamID::deqAttack, "Attack", cd);
    dynRelease = knob (dynEqSection, ParamID::deqRelease, "Release", cd);

    // ---- TONE: Stabilizer -----------------------------------------------------------------------------------
    const auto cs = moduleColour (ModuleId::stabilizer);
    stabStyle = choice (stabSection, ParamID::targetStyle, "Target");
    stabKnobs = { knob (stabSection, ParamID::stabAmount, "Amount", cs), knob (stabSection, ParamID::stabSpeed, "Speed", cs),
                  knob (stabSection, ParamID::stabTilt, "Tilt", cs, true) };
    stabKnobs[0]->slider.setTooltip ("How strongly the Stabilizer pulls the tonal balance towards the target (dotted line on the spectrum)");
    stabKnobs[2]->slider.setTooltip ("Tilts the target: positive = brighter, negative = darker");
    referenceLabel.setFont (uiFont (10.5f));
    referenceLabel.setColour (juce::Label::textColourId, Colours::textDim);
    referenceLabel.setJustificationType (juce::Justification::centredLeft);
    stabSection.addAndMakeVisible (referenceLabel);

    // ---- DYNAMICS: Glue -----------------------------------------------------------------------------------------
    const auto cg = moduleColour (ModuleId::glue);
    glueRatio = choice (glueSection, ParamID::glueRatio, "Ratio");
    glueAttack = choice (glueSection, ParamID::glueAttack, "Attack");
    glueRelease = choice (glueSection, ParamID::glueRelease, "Release");
    glueKnobs = { knob (glueSection, ParamID::glueThreshold, "Threshold", Colours::threshold), knob (glueSection, ParamID::glueMakeup, "Makeup", cg),
                  knob (glueSection, ParamID::glueScHpf, "SC HPF", cg), knob (glueSection, ParamID::glueMix, "Mix", cg) };
    glueKnobs[2]->slider.setTooltip ("Side-chain high-pass: keeps the bass from pumping the whole mix");

    // ---- DYNAMICS: Multiband ---------------------------------------------------------------------------------------
    const auto cm = moduleColour (ModuleId::dynamics);
    mbTop = { knob (mbSection, ParamID::mbLowMid, "Low / Mid", cm), knob (mbSection, ParamID::mbMidHigh, "Mid / High", cm),
              knob (mbSection, ParamID::mbMix, "Mix", cm) };
    for (int b = 0; b < kMbBands; ++b)
    {
        auto strip = std::make_unique<BandStrip> (bandNames[b], Colours::forBand (b), &processor.meters.mb[(size_t) b]);
        mbSection.addAndMakeVisible (*strip);
        auto* thr = knob (*strip, BandIDs::mb[b][0], "Thresh", Colours::threshold);
        auto* ratio = knob (*strip, BandIDs::mb[b][1], "Ratio", Colours::forBand (b));
        auto* gain = knob (*strip, BandIDs::mb[b][4], "Gain", Colours::forBand (b), true);
        auto* att = knob (*strip, BandIDs::mb[b][2], "Attack", Colours::forBand (b));
        auto* rel = knob (*strip, BandIDs::mb[b][3], "Release", Colours::forBand (b));
        soloPills[(size_t) b] = std::make_unique<ParamPill> (state, BandIDs::mb[b][5], "SOLO", Colours::detection);
        strip->addAndMakeVisible (*soloPills[(size_t) b]);
        strip->setItems ({ thr, ratio, gain, att, rel, nullptr }, 3);
        mbStrips[(size_t) b] = std::move (strip);
    }

    // ---- DYNAMICS: Maximizer ------------------------------------------------------------------------------------------
    const auto cx = Colours::danger;
    maxMode = choice (maxSection, ParamID::maxMode, "Character");
    maxMode->box.setTooltip ("Transparent: slow, clean. Balanced: all-round. Loud: soft-clips peaks before limiting. Clip: drives a clipper hard");
    maxKnobs = { knob (maxSection, ParamID::maxGain, "Gain", Colours::accent), knob (maxSection, ParamID::maxCeiling, "Ceiling", cx),
                 knob (maxSection, ParamID::maxRelease, "Release", cx), knob (maxSection, ParamID::maxLink, "Link", cx) };
    maxKnobs[0]->slider.setTooltip ("Drive into the limiter: raises the loudness");
    truePeakPill = std::make_unique<ParamPill> (state, ParamID::maxTruePeak, "TRUE PEAK", cx);
    truePeakPill->setTooltip ("Limit inter-sample (true) peaks, as streaming services require");
    maxSection.addAndMakeVisible (*truePeakPill);
    toTargetButton.setTooltip ("Adjust the Maximizer gain so the measured integrated loudness meets the target (play the song, then press; repeat to refine)");
    toTargetButton.onClick = [this] { moveGainToTarget(); };
    maxSection.addAndMakeVisible (toTargetButton);

    // ---- STEREO & CHARACTER: Exciter ------------------------------------------------------------------------------------
    const auto cy = moduleColour (ModuleId::exciter);
    excMode = choice (excSection, ParamID::excMode, "Mode");
    excMode->box.setTooltip ("Tape: smooth odd harmonics. Tube: warm even harmonics. Warm: very soft. Triode: asymmetric, rich. Retro: gritty");
    excTop = { knob (excSection, ParamID::excLowMid, "Low / Mid", cy), knob (excSection, ParamID::excMidHigh, "Mid / High", cy) };
    for (int b = 0; b < kMbBands; ++b)
    {
        auto strip = std::make_unique<BandStrip> (bandNames[b], Colours::forBand (b));
        excSection.addAndMakeVisible (*strip);
        auto* drive = knob (*strip, BandIDs::exc[b][0], "Drive", Colours::forBand (b));
        auto* mix = knob (*strip, BandIDs::exc[b][1], "Mix", Colours::forBand (b));
        strip->setItems ({ drive, mix }, 2);
        excStrips[(size_t) b] = std::move (strip);
    }

    // ---- STEREO & CHARACTER: Imager --------------------------------------------------------------------------------------
    const auto ci = moduleColour (ModuleId::imager);
    imgTop = { knob (imgSection, ParamID::imgLowMid, "Low / Mid", ci), knob (imgSection, ParamID::imgMidHigh, "Mid / High", ci),
               knob (imgSection, ParamID::imgStereoize, "Stereoize", ci), knob (imgSection, ParamID::imgMonoBass, "Mono Bass", Colours::threshold) };
    imgTop[2]->slider.setTooltip ("Adds width to narrow material without changing the mono sum");
    imgTop[3]->slider.setTooltip ("Makes everything below this frequency mono (phase-coherent), for a tight low end and vinyl safety");
    for (int b = 0; b < kMbBands; ++b)
    {
        auto strip = std::make_unique<BandStrip> (bandNames[b], Colours::forBand (b));
        imgSection.addAndMakeVisible (*strip);
        auto* w = knob (*strip, BandIDs::img[b], "Width", Colours::forBand (b));
        w->slider.setTooltip ("0 % = mono, 100 % = unchanged, 200 % = twice the side signal");
        strip->setItems ({ w }, 1);
        imgStrips[(size_t) b] = std::move (strip);
    }

    sectionByKey = { { "input", &inputSection }, { "output", &outputSection }, { "max", &maxSection },
                     { "m0", &eqSection }, { "m1", &stabSection }, { "m2", &dynEqSection }, { "m3", &glueSection },
                     { "m4", &mbSection }, { "m5", &excSection }, { "m6", &imgSection } };

    // Dim the controls of switched-off sections
    for (auto* s : { &eqSection, &dynEqSection, &stabSection, &glueSection, &mbSection, &maxSection, &excSection, &imgSection })
    {
        s->onActiveChanged = [s]
        {
            for (auto* c : s->getChildren())
                if (dynamic_cast<Knob*> (c) != nullptr || dynamic_cast<Choice*> (c) != nullptr || dynamic_cast<BandStrip*> (c) != nullptr)
                    c->setAlpha (s->isActive() ? 1.0f : 0.5f);
        };
        s->onActiveChanged();
    }

    selectEqBand (2);
    selectDynBand (0);
    setPage (0);
    setSize (baseWidth, baseHeight);
}

Knob* MainPanel::knob (juce::Component& parent, const char* id, const juce::String& name, juce::Colour c, bool bipolar)
{
    auto k = std::make_unique<Knob> (state, id, name, c, bipolar);
    auto* raw = k.get();
    parent.addAndMakeVisible (*raw);
    owned.push_back (std::move (k));
    return raw;
}

Choice* MainPanel::choice (juce::Component& parent, const char* id, const juce::String& caption)
{
    auto c = std::make_unique<Choice> (state, id, caption);
    auto* raw = c.get();
    parent.addAndMakeVisible (*raw);
    owned.push_back (std::move (c));
    return raw;
}

void MainPanel::grid (std::vector<juce::Component*> items, int columns, juce::Rectangle<int> area)
{
    const int rows = ((int) items.size() + columns - 1) / juce::jmax (1, columns);
    const int cw = area.getWidth() / juce::jmax (1, columns);
    const int rh = area.getHeight() / juce::jmax (1, rows);
    int i = 0;
    for (auto* c : items)
    {
        if (c != nullptr)
            c->setBounds (area.getX() + (i % columns) * cw, area.getY() + (i / columns) * rh, cw, rh);
        ++i;
    }
}

void MainPanel::refreshPresetList()
{
    presetBox.clear (juce::dontSendNotification);
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        presetBox.addItem (processor.getProgramName (i), i + 1);

    numUserPresets = 0;
    auto files = MasterProcessor::getUserPresetFolder().findChildFiles (juce::File::findFiles, false, "*.lamaster");
    files.sort();
    if (! files.isEmpty())
    {
        presetBox.addSectionHeading ("User presets");
        for (auto& f : files)
        {
            if (numUserPresets >= 256) break;
            userPresetFiles[numUserPresets] = f;
            presetBox.addItem (f.getFileNameWithoutExtension(), 1000 + numUserPresets);
            ++numUserPresets;
        }
    }
    presetBox.setText (processor.getCurrentPresetName(), juce::dontSendNotification);
}

void MainPanel::refreshTargetBox()
{
    targetBox.clear (juce::dontSendNotification);
    int id = 1;
    for (const auto& t : targetPresets)
        targetBox.addItem (juce::String (t.lufs, 0) + " LUFS   " + t.label, id++);
    targetBox.addItem ("Custom...", 99);
    targetBox.setText (juce::String (state.getRawParameterValue (ParamID::targetLufs)->load(), 1) + " LUFS", juce::dontSendNotification);
}

void MainPanel::showToast (const juce::String& text)
{
    toastText = text;
    toastTicks = 30 * 9;
    repaint (0, 0, baseWidth, 46);
}

void MainPanel::showReferenceMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Load reference track...");
    m.addItem (2, "Clear reference" + (processor.hasReference() ? " (" + processor.getReferenceName() + ")" : juce::String()), processor.hasReference());
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (refButton), [this] (int r)
    {
        if (r == 2)
        {
            processor.clearReference();
            showToast ("Reference cleared");
        }
        if (r != 1)
            return;
        chooser = std::make_unique<juce::FileChooser> ("Reference track", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file.existsAsFile())
                showToast (processor.loadReference (file));
        });
    });
}

void MainPanel::moveGainToTarget()
{
    const float integrated = processor.meters.integrated.load();
    if (integrated <= kSilenceLufs + 1.0f)
    {
        showToast ("Play the song first so the loudness can be measured");
        return;
    }
    const float target = state.getRawParameterValue (ParamID::targetLufs)->load();
    if (auto* prm = state.getParameter (ParamID::maxGain))
    {
        const float now = prm->convertFrom0to1 (prm->getValue());
        const float next = juce::jlimit (0.0f, 24.0f, now + (target - integrated));
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (prm->convertTo0to1 (next));
        prm->endChangeGesture();
        showToast ("Maximizer gain " + juce::String (now, 1) + " -> " + juce::String (next, 1) + " dB (measured " + juce::String (integrated, 1)
                   + " LUFS, target " + juce::String (target, 1) + "). Loudness meter restarted");
    }
    if (auto* on = state.getParameter (ParamID::maxOn); on != nullptr && on->getValue() < 0.5f)
        on->setValueNotifyingHost (1.0f);
    processor.requestLoudnessReset();
}

void MainPanel::selectEqBand (int band)
{
    eqBand = juce::jlimit (0, kEqBands - 1, band);
    for (int b = 0; b < kEqBands; ++b)
    {
        eqBandButtons[(size_t) b]->setToggleState (b == eqBand, juce::dontSendNotification);
        for (auto* k : eqKnobs[(size_t) b])
            k->setVisible (b == eqBand);
        eqMode[(size_t) b]->setVisible (b == eqBand);
    }
    spectrum.setSelectedEqBand (eqBand);
}

void MainPanel::selectDynBand (int band)
{
    dynBand = juce::jlimit (0, kDynEqBands - 1, band);
    for (int b = 0; b < kDynEqBands; ++b)
    {
        dynBandButtons[(size_t) b]->setToggleState (b == dynBand, juce::dontSendNotification);
        for (auto* k : dynKnobs[(size_t) b])
            k->setVisible (b == dynBand);
        dynMode[(size_t) b]->setVisible (b == dynBand);
    }
    spectrum.setSelectedDynBand (dynBand);
}

void MainPanel::setPage (int page)
{
    currentPage = juce::jlimit (0, 2, page);
    tabTone.setToggleState (currentPage == 0, juce::dontSendNotification);
    tabDynamics.setToggleState (currentPage == 1, juce::dontSendNotification);
    tabStereo.setToggleState (currentPage == 2, juce::dontSendNotification);
    for (auto* s : { &eqSection, &dynEqSection, &stabSection })
        s->setVisible (currentPage == 0);
    for (auto* s : { &glueSection, &mbSection, &maxSection })
        s->setVisible (currentPage == 1);
    for (auto* s : { &excSection, &imgSection })
        s->setVisible (currentPage == 2);
}

void MainPanel::focusSection (const juce::String& key)
{
    highlightKey = key;
    highlightAmount = 1.0f;
}

//==============================================================================
void MainPanel::resized()
{
    const int margin = 12, gap = 8;

    // Header
    presetBox.setBounds (232, 11, 186, 26);
    saveButton.setBounds (422, 11, 48, 26);
    assistButton.setBounds (482, 11, 104, 26);
    styleChoice->setBounds (598, 11, 104, 26);
    targetBox.setBounds (706, 11, 112, 26);
    refButton.setBounds (822, 11, 46, 26);
    comparePill->setBounds (882, 11, 80, 26);
    matchPill->setBounds (966, 11, 58, 26);
    deltaPill->setBounds (1028, 11, 58, 26);

    chain.setBounds (margin, 54, baseWidth - 2 * margin, 44);

    const int dispY = 106, dispH = 284;
    spectrum.setBounds (margin, dispY, 580, dispH);
    history.setBounds (600, dispY, 380, dispH);
    scope.setBounds (988, dispY, 196, dispH);
    loudness.setBounds (1192, dispY, 166, dispH);
    meters.setBounds (1366, dispY + 4, 62, dispH - 8);

    const int tabsY = dispY + dispH + gap;
    const int sideW = 128, outW = 148;
    const int pageX = margin + sideW + gap;
    tabTone.setBounds (pageX, tabsY, 100, 24);
    tabDynamics.setBounds (pageX + 106, tabsY, 110, 24);
    tabStereo.setBounds (pageX + 222, tabsY, 170, 24);

    const int bottomH = baseHeight - tabsY - margin;
    inputSection.setBounds (margin, tabsY, sideW, bottomH);
    outputSection.setBounds (baseWidth - margin - outW, tabsY, outW, bottomH);
    grid ({ inputKnobs[0], inputKnobs[1], inputKnobs[2] }, 1, inputSection.getContentArea().withTrimmedTop (4));
    {
        auto c = outputSection.getContentArea().withTrimmedTop (4);
        outputKnobs[0]->setBounds (c.removeFromTop (c.getHeight() / 2));
        ditherChoice->setBounds (c.removeFromTop (46).reduced (4, 2));
        c.removeFromTop (8);
        monitorChoice->setBounds (c.removeFromTop (46).reduced (4, 2));
    }

    const int botY = tabsY + 24 + gap;
    const auto area = juce::Rectangle<int> (pageX, botY, baseWidth - 2 * margin - sideW - outW - 2 * gap, baseHeight - botY - margin);

    // ---- TONE --------------------------------------------------------------------------------------------
    {
        const int eqW = 372, dynW = 410;
        eqSection.setBounds (area.withWidth (eqW));
        dynEqSection.setBounds (area.withTrimmedLeft (eqW + gap).withWidth (dynW));
        stabSection.setBounds (area.withTrimmedLeft (eqW + dynW + 2 * gap));

        auto c = eqSection.getContentArea();
        auto row = c.removeFromTop (22);
        const int bw = row.getWidth() / kEqBands;
        for (int b = 0; b < kEqBands; ++b)
            eqBandButtons[(size_t) b]->setBounds (row.removeFromLeft (bw).reduced (1, 0));
        c.removeFromTop (6);
        auto top = c.removeFromTop (c.getHeight() / 2);
        for (int b = 0; b < kEqBands; ++b)
        {
            auto t = top;
            const int w = t.getWidth() / 4;
            for (auto* k : eqKnobs[(size_t) b]) k->setBounds (t.removeFromLeft (w));
            eqMode[(size_t) b]->setBounds (t.withSizeKeepingCentre (t.getWidth() - 4, 46));
        }
        grid ({ eqHpf, eqLpf, nullptr, nullptr }, 4, c);

        auto d = dynEqSection.getContentArea();
        auto drow = d.removeFromTop (22);
        const int dw = drow.getWidth() / 6;
        for (int b = 0; b < kDynEqBands; ++b)
            dynBandButtons[(size_t) b]->setBounds (drow.removeFromLeft (dw).reduced (1, 0));
        d.removeFromTop (6);
        auto dtop = d.removeFromTop (d.getHeight() / 2);
        for (int b = 0; b < kDynEqBands; ++b)
        {
            auto& k = dynKnobs[(size_t) b];
            grid ({ k[0], k[1], k[2], k[3] }, 4, dtop);
            const int w = d.getWidth() / 4;
            dynMode[(size_t) b]->setBounds (d.withWidth (w).withSizeKeepingCentre (w - 4, 46));
        }
        grid ({ nullptr, dynAttack, dynRelease, nullptr }, 4, d);

        auto s = stabSection.getContentArea();
        stabStyle->setBounds (s.removeFromTop (44).reduced (2, 2));
        s.removeFromTop (4);
        referenceLabel.setBounds (s.removeFromBottom (20));
        grid ({ stabKnobs[0], stabKnobs[1], stabKnobs[2] }, 3, s.withSizeKeepingCentre (s.getWidth(), juce::jmin (s.getHeight(), 120)));
    }

    // ---- DYNAMICS -----------------------------------------------------------------------------------------
    {
        const int glueW = 300, maxW = 262;
        glueSection.setBounds (area.withWidth (glueW));
        maxSection.setBounds (area.withTrimmedLeft (area.getWidth() - maxW));
        mbSection.setBounds (area.withTrimmedLeft (glueW + gap).withTrimmedRight (maxW + gap));

        auto c = glueSection.getContentArea();
        auto left = c.removeFromLeft (100);
        const int ch = left.getHeight() / 3;
        glueRatio->setBounds (left.removeFromTop (ch).withSizeKeepingCentre (left.getWidth() - 4, 46));
        glueAttack->setBounds (left.removeFromTop (ch).withSizeKeepingCentre (left.getWidth() - 4, 46));
        glueRelease->setBounds (left.withSizeKeepingCentre (left.getWidth() - 4, 46));
        grid ({ glueKnobs[0], glueKnobs[1], glueKnobs[2], glueKnobs[3] }, 2, c);

        auto m = mbSection.getContentArea();
        grid ({ mbTop[0], mbTop[1], mbTop[2] }, 3, m.removeFromTop (84));
        m.removeFromTop (2);
        const int sw = m.getWidth() / kMbBands;
        for (int b = 0; b < kMbBands; ++b)
        {
            auto& strip = *mbStrips[(size_t) b];
            strip.setBounds (m.removeFromLeft (sw).reduced (2, 0));
            auto cell = juce::Rectangle<int> ((strip.getWidth() - 4) * 2 / 3 + 2, 24 + (strip.getHeight() - 24) / 2, (strip.getWidth() - 4) / 3,
                                              (strip.getHeight() - 24) / 2);
            soloPills[(size_t) b]->setBounds (cell.withSizeKeepingCentre (cell.getWidth() - 10, 22));
        }

        auto x = maxSection.getContentArea();
        auto top = x.removeFromTop (46);
        maxMode->setBounds (top.removeFromLeft (top.getWidth() / 2).reduced (2, 0));
        auto pills = top.withTrimmedTop (15).reduced (4, 0);
        truePeakPill->setBounds (pills.removeFromTop (24));
        toTargetButton.setBounds (x.removeFromBottom (24).reduced (40, 0));
        x.removeFromBottom (4);
        grid ({ maxKnobs[0], maxKnobs[1], maxKnobs[2], maxKnobs[3] }, 2, x);
    }

    // ---- STEREO & CHARACTER ---------------------------------------------------------------------------------
    {
        const int excW = 520;
        excSection.setBounds (area.withWidth (excW));
        imgSection.setBounds (area.withTrimmedLeft (excW + gap));

        auto e = excSection.getContentArea();
        auto top = e.removeFromTop (84);
        excMode->setBounds (top.removeFromLeft (top.getWidth() / 3).withSizeKeepingCentre (top.getWidth() / 3 - 12, 46));
        grid ({ excTop[0], excTop[1] }, 2, top);
        e.removeFromTop (4);
        const int sw = e.getWidth() / kMbBands;
        for (auto& s : excStrips)
            s->setBounds (e.removeFromLeft (sw).reduced (2, 0).withTrimmedBottom (e.getHeight() / 5));

        auto im = imgSection.getContentArea();
        grid ({ imgTop[0], imgTop[1], imgTop[2], imgTop[3] }, 4, im.removeFromTop (100));
        im.removeFromTop (4);
        const int iw = im.getWidth() / kMbBands;
        for (auto& s : imgStrips)
            s->setBounds (im.removeFromLeft (iw).reduced (2, 0).withTrimmedBottom (im.getHeight() / 8));
    }
}

void MainPanel::paint (juce::Graphics& g)
{
    Hardware::drawChassis (g, { 0, 0, baseWidth, baseHeight }, 46);

    // Logo: a waveform squeezed under a ceiling line
    {
        const float x0 = 14.0f, y0 = 23.0f;
        juce::Path wave;
        for (int i = 0; i <= 26; ++i)
        {
            const float t = (float) i / 26.0f;
            const float y = y0 + 6.5f * std::sin (t * 12.0f) * (0.4f + 0.6f * t);
            if (i == 0) wave.startNewSubPath (x0 + t * 28.0f, y); else wave.lineTo (x0 + t * 28.0f, y);
        }
        g.setColour (Colours::signature);
        g.strokePath (wave, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (Colours::danger);
        g.fillRoundedRectangle (x0 - 1.0f, y0 - 9.5f, 31.0f, 2.0f, 1.0f);

        g.setFont (uiFont (21.0f, true));
        g.setColour (Colours::text);
        g.drawText ("LA", 50, 8, 30, 30, juce::Justification::centredLeft);
        g.setColour (Colours::signature);
        g.drawText ("MASTER", 78, 8, 100, 30, juce::Justification::centredLeft);
        g.setColour (Colours::textDim);
        g.setFont (uiFont (10.0f));
        g.drawText ("MASTERING", 162, 9, 66, 30, juce::Justification::centredLeft);
    }

    g.setFont (uiFont (11.5f));
    const auto infoArea = juce::Rectangle<int> (1096, 4, baseWidth - 1096 - 12, 38);
    if (toastTicks > 0)
    {
        g.setColour (Colours::learn);
        g.drawFittedText (toastText, infoArea, juce::Justification::centredRight, 3);
    }
    else
    {
        g.setColour (Colours::textDim);
        g.drawFittedText (infoText, infoArea, juce::Justification::centredRight, 2);
    }
}

void MainPanel::tick()
{
    LoudnessFrame f;
    while (processor.loudnessQueue.pop (f))
        history.addFrame (f);
    history.repaint();
    spectrum.pullSpectrum();
    scope.pull();
    loudness.tick();
    chain.tick();

    const float inPeak = processor.meters.inPeak.exchange (0.0f);
    const float outPeak = processor.meters.outPeak.exchange (0.0f);
    float gr = processor.meters.glue.load() + processor.meters.maximizer.load();
    float mbMax = 0.0f;
    for (auto& v : processor.meters.mb) mbMax = std::max (mbMax, v.load());
    meters.setLevels (inPeak, outPeak, gr + mbMax);
    for (auto& s : mbStrips) s->repaint();

    // Band titles show their frequency range
    {
        auto v = [this] (const char* id) { return state.getRawParameterValue (id)->load(); };
        auto titles = [&] (std::array<std::unique_ptr<BandStrip>, kMbBands>& strips, const char* lowId, const char* highId)
        {
            const float a = v (lowId), b = v (highId);
            strips[0]->setTitle ("LOW  < " + hzText (a));
            strips[1]->setTitle ("MID  " + hzText (a) + " - " + hzText (b));
            strips[2]->setTitle ("HIGH  > " + hzText (b));
        };
        titles (mbStrips, ParamID::mbLowMid, ParamID::mbMidHigh);
        titles (excStrips, ParamID::excLowMid, ParamID::excMidHigh);
        titles (imgStrips, ParamID::imgLowMid, ParamID::imgMidHigh);
    }

    // Assistant
    blink = (blink + 1) % 20;
    juce::String summary;
    if (processor.pollAssist (summary))
        showToast (summary);
    if (processor.isAssistAnalysing())
    {
        assistButton.setButtonText ("ANALYSING");
        assistButton.setToggleState (blink < 14, juce::dontSendNotification);
    }
    else if (processor.isAssistListening())
    {
        assistButton.setButtonText ("LISTENING " + juce::String ((int) processor.getAssistSeconds()) + "s");
        assistButton.setToggleState (blink < 14, juce::dontSendNotification);
        if (processor.getAssistSeconds() >= 29.9)
            processor.finishAssist();
    }
    else
    {
        assistButton.setButtonText ("ASSIST");
        assistButton.setToggleState (false, juce::dontSendNotification);
    }

    refButton.setToggleState (processor.hasReference(), juce::dontSendNotification);
    referenceLabel.setText (processor.hasReference() ? "Reference: " + processor.getReferenceName() + "  (" + juce::String (processor.getReferenceLufs(), 1) + " LUFS)"
                                                     : juce::String ("No reference loaded (REF in the header)"),
                            juce::dontSendNotification);

    if (highlightAmount > 0.0f)
    {
        highlightAmount = juce::jmax (0.0f, highlightAmount - 0.04f);
        if (auto it = sectionByKey.find (highlightKey); it != sectionByKey.end())
            it->second->setHighlight (highlightAmount);
    }

    if (toastTicks > 0 && --toastTicks == 0)
        repaint (0, 0, baseWidth, 46);

    const double sr = processor.getEngineSampleRate();
    juce::String info;
    const bool bypass = state.getRawParameterValue (ParamID::bypass)->load() > 0.5f;
    const bool delta = state.getRawParameterValue (ParamID::delta)->load() > 0.5f;
    if (bypass || delta)
    {
        const bool match = state.getRawParameterValue (ParamID::gainMatch)->load() > 0.5f;
        const float m = processor.meters.match.load();
        info << (delta ? "DELTA" : "COMPARING INPUT") << (match || delta ? "  (matched " + juce::String (m >= 0 ? "+" : "") + juce::String (m, 1) + " dB)" : juce::String())
             << "\n";
    }
    info << "Latency " << juce::String (1000.0 * processor.getLatencySamples() / juce::jmax (1.0, sr), 1) << " ms    "
         << juce::String (sr / 1000.0, 1) << " kHz";
    if (info != infoText)
    {
        infoText = info;
        repaint (0, 0, baseWidth, 46);
    }

    const auto name = processor.getCurrentPresetName();
    if (presetBox.getText() != name && ! presetBox.isPopupActive())
        presetBox.setText (name, juce::dontSendNotification);
    const auto targetText = juce::String (state.getRawParameterValue (ParamID::targetLufs)->load(), 1) + " LUFS";
    if (targetBox.getText() != targetText && ! targetBox.isPopupActive())
        targetBox.setText (targetText, juce::dontSendNotification);
}

//==============================================================================
MasterEditor::MasterEditor (MasterProcessor& p)
    : AudioProcessorEditor (p), panel (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (panel);

    setResizable (true, true);
    setResizeLimits (MainPanel::baseWidth * 5 / 10, MainPanel::baseHeight * 5 / 10, MainPanel::baseWidth * 2, MainPanel::baseHeight * 2);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) MainPanel::baseWidth / MainPanel::baseHeight);
    setSize (MainPanel::baseWidth * 9 / 10, MainPanel::baseHeight * 9 / 10);

    startTimerHz (30);
}

MasterEditor::~MasterEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void MasterEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);
}

void MasterEditor::resized()
{
    const float scale = (float) getWidth() / (float) MainPanel::baseWidth;
    panel.setTransform (juce::AffineTransform::scale (scale));
    panel.setBounds (0, 0, MainPanel::baseWidth, MainPanel::baseHeight);
}
} // namespace lam
