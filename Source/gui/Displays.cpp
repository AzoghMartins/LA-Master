#include "Displays.h"
#include "../PluginProcessor.h"

namespace lam
{
namespace
{
void drawLegend (juce::Graphics& g, juce::Rectangle<int> area, std::initializer_list<std::pair<juce::String, juce::Colour>> items)
{
    g.setFont (uiFont (10.5f, true));
    int x = area.getRight();
    std::vector<std::pair<juce::String, juce::Colour>> list (items);
    for (auto it = list.rbegin(); it != list.rend(); ++it)
    {
        const int w = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), it->first) + 26;
        x -= w;
        g.setColour (it->second);
        g.fillRoundedRectangle ((float) x, (float) area.getCentreY() - 2.0f, 12.0f, 4.0f, 2.0f);
        g.setColour (Colours::textDim);
        g.drawText (it->first, x + 16, area.getY(), w - 16, area.getHeight(), juce::Justification::centredLeft);
    }
}

void drawFrame (juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour (Colours::graphBg);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (Colours::edge);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);
}

juce::String lufsText (float v, int decimals = 1)
{
    return v <= kSilenceLufs + 1.0f ? juce::String ("--") : juce::String (v, decimals);
}

void strokeDashed (juce::Graphics& g, const juce::Path& p, float thickness, float on, float off)
{
    juce::Path dashed;
    const float dashes[] = { on, off };
    juce::PathStrokeType (thickness).createDashedStroke (dashed, p, dashes, 2);
    g.fillPath (dashed);
}
} // namespace

juce::Colour moduleColour (ModuleId m)
{
    switch (m)
    {
        case ModuleId::eq:         return Colours::threshold;
        case ModuleId::stabilizer: return Colours::learn;
        case ModuleId::dyneq:      return Colours::pink;
        case ModuleId::glue:       return Colours::blue;
        case ModuleId::dynamics:   return Colours::danger;
        case ModuleId::exciter:    return Colours::detection;
        case ModuleId::imager:     return Colours::accent;
        case ModuleId::numModules: break;
    }
    return Colours::accent;
}

//==============================================================================
SpectrumDisplay::SpectrumDisplay (MasterProcessor& p) : processor (p), state (p.getState())
{
    fifoIn.assign ((size_t) fftSize, 0.0f);
    fifoOut.assign ((size_t) fftSize, 0.0f);
    fftData.assign ((size_t) fftSize * 2, 0.0f);
    window.resize ((size_t) fftSize);
    specIn.assign ((size_t) fftSize / 2, -120.0f);
    specOut.assign ((size_t) fftSize / 2, -120.0f);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (fftSize - 1));
    setOpaque (true);
    setTooltip ("Drag the EQ nodes (and the pink Dynamic EQ nodes) to set frequency and gain, mouse wheel for Q, "
                "double-click to reset. The dotted line is the target tonal balance");
}

float SpectrumDisplay::value (const char* id) const { return state.getRawParameterValue (id)->load(); }

juce::RangedAudioParameter* SpectrumDisplay::param (Hit h, int which) const
{
    if (h.kind == 0)
        return state.getParameter (BandIDs::eq[h.band][which]);
    static const int map[] = { 0, 3, 1 }; // freq, range, q
    return state.getParameter (BandIDs::dynEq[h.band][map[which]]);
}

void SpectrumDisplay::analyse (std::vector<float>& fifo, int& fifoPos, std::vector<float>& smoothed)
{
    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = fifo[(size_t) ((fifoPos + i) % fftSize)] * window[(size_t) i];
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);
    for (int i = 0; i < fftSize / 2; ++i)
    {
        const float db = juce::Decibels::gainToDecibels (fftData[(size_t) i] / (float) (fftSize / 4), -140.0f);
        auto& s = smoothed[(size_t) i];
        s = db > s ? s + (db - s) * 0.4f : s + (db - s) * 0.1f;
    }
}

void SpectrumDisplay::pullSpectrum()
{
    float tmp[2048];
    int got;
    while ((got = processor.spectrumIn.pop (tmp, 2048)) > 0)
        for (int i = 0; i < got; ++i) { fifoIn[(size_t) posIn] = tmp[i]; posIn = (posIn + 1) % fftSize; }
    while ((got = processor.spectrumOut.pop (tmp, 2048)) > 0)
        for (int i = 0; i < got; ++i) { fifoOut[(size_t) posOut] = tmp[i]; posOut = (posOut + 1) % fftSize; }
    analyse (fifoIn, posIn, specIn);
    analyse (fifoOut, posOut, specOut);
    repaint();
}

juce::Rectangle<float> SpectrumDisplay::plotArea() const
{
    auto r = getLocalBounds().toFloat();
    r.removeFromTop (22.0f);
    r.removeFromBottom (18.0f);
    r.removeFromLeft (30.0f);
    r.removeFromRight (6.0f);
    return r;
}

float SpectrumDisplay::freqToX (float f, juce::Rectangle<float> a) const
{
    return a.getX() + a.getWidth() * std::log (f / 20.0f) / std::log (1000.0f);
}

float SpectrumDisplay::xToFreq (float x, juce::Rectangle<float> a) const
{
    return 20.0f * std::pow (1000.0f, juce::jlimit (0.0f, 1.0f, (x - a.getX()) / a.getWidth()));
}

float SpectrumDisplay::dbToY (float db, juce::Rectangle<float> a) const { return juce::jmap (db, maxDb, minDb, a.getY(), a.getBottom()); }

float SpectrumDisplay::yToDb (float y, juce::Rectangle<float> a) const
{
    return juce::jmap (juce::jlimit (a.getY(), a.getBottom(), y), a.getY(), a.getBottom(), maxDb, minDb);
}

juce::Point<float> SpectrumDisplay::nodePosition (Hit h, juce::Rectangle<float> a) const
{
    const auto* f = param (h, 0);
    const auto* gn = param (h, 1);
    return { freqToX (f->convertFrom0to1 (f->getValue()), a), dbToY (gn->convertFrom0to1 (gn->getValue()), a) };
}

SpectrumDisplay::Hit SpectrumDisplay::hitNode (juce::Point<float> p) const
{
    const auto a = plotArea();
    if (value (ParamID::deqOn) > 0.5f)
        for (int b = 0; b < kDynEqBands; ++b)
            if (nodePosition ({ 1, b }, a).getDistanceFrom (p) < 10.0f)
                return { 1, b };
    for (int b = 0; b < kEqBands; ++b)
        if (nodePosition ({ 0, b }, a).getDistanceFrom (p) < 10.0f)
            return { 0, b };
    return {};
}

void SpectrumDisplay::mouseMove (const juce::MouseEvent& e)
{
    const auto h = hitNode (e.position);
    if (h.kind != hover.kind || h.band != hover.band)
    {
        hover = h;
        setMouseCursor (h.kind >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void SpectrumDisplay::mouseDown (const juce::MouseEvent& e)
{
    dragging = hitNode (e.position);
    if (dragging.kind < 0)
        return;
    if (dragging.kind == 0)
    {
        selectedEq = dragging.band;
        if (onEqBandSelected) onEqBandSelected (selectedEq);
        if (auto* on = state.getParameter (ParamID::eqOn); on != nullptr && on->getValue() < 0.5f)
            on->setValueNotifyingHost (1.0f);
    }
    else
    {
        selectedDyn = dragging.band;
        if (onDynBandSelected) onDynBandSelected (selectedDyn);
    }
    param (dragging, 0)->beginChangeGesture();
    param (dragging, 1)->beginChangeGesture();
}

void SpectrumDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging.kind < 0)
        return;
    const auto a = plotArea();
    auto* f = param (dragging, 0);
    auto* gn = param (dragging, 1);
    f->setValueNotifyingHost (f->convertTo0to1 (f->getNormalisableRange().snapToLegalValue (xToFreq (e.position.x, a))));
    gn->setValueNotifyingHost (gn->convertTo0to1 (juce::jlimit (-12.0f, 12.0f, yToDb (e.position.y, a))));
    repaint();
}

void SpectrumDisplay::mouseUp (const juce::MouseEvent&)
{
    if (dragging.kind >= 0)
    {
        param (dragging, 0)->endChangeGesture();
        param (dragging, 1)->endChangeGesture();
    }
    dragging = {};
}

void SpectrumDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto h = hitNode (e.position);
    if (h.kind < 0)
        return;
    auto* gn = param (h, 1);
    gn->beginChangeGesture();
    gn->setValueNotifyingHost (gn->getDefaultValue());
    gn->endChangeGesture();
}

void SpectrumDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    auto h = hitNode (e.position);
    if (h.kind < 0) h = { 0, selectedEq };
    auto* q = param (h, 2);
    q->beginChangeGesture();
    q->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, q->getValue() + (w.deltaY > 0 ? 0.04f : -0.04f)));
    q->endChangeGesture();
}

void SpectrumDisplay::paint (juce::Graphics& g)
{
    drawFrame (g, getLocalBounds().toFloat());
    const auto a = plotArea();
    const double sr = std::max (8000.0, processor.getEngineSampleRate());

    // Grid
    g.setFont (uiFont (9.5f));
    for (float f : { 30.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = freqToX (f, a);
        g.setColour (Colours::grid);
        g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
        g.setColour (Colours::textDim);
        g.drawText (f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + "k" : juce::String ((int) f), (int) x - 20,
                    (int) a.getBottom() + 3, 40, 12, juce::Justification::centred);
    }
    for (float db : { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f })
    {
        const float y = dbToY (db, a);
        g.setColour (std::abs (db) < 0.1f ? Colours::edge : Colours::grid);
        g.drawHorizontalLine ((int) y, a.getX(), a.getRight());
        g.setColour (Colours::textDim);
        g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), 0, (int) y - 6, 26, 12, juce::Justification::centredRight);
    }

    g.saveState();
    g.reduceClipRegion (a.toNearestInt());

    // ---- Spectra (slope compensated +3 dB/oct so pink noise is flat) --------------------------------
    const int bins = fftSize / 2;
    auto spectrumDbAt = [&] (const std::vector<float>& s, float f)
    {
        const float binWidth = (float) sr / (float) fftSize;
        const int b0 = juce::jlimit (1, bins - 1, (int) (f * 0.944f / binWidth));
        const int b1 = juce::jlimit (b0, bins - 1, (int) (f * 1.059f / binWidth));
        double p = 0.0;
        for (int b = b0; b <= b1; ++b) p += std::pow (10.0, s[(size_t) b] / 10.0);
        const float db = (float) (10.0 * std::log10 (p / (double) (b1 - b0 + 1) + 1.0e-20));
        return db + 3.0f * std::log2 (f / 1000.0f);
    };
    constexpr float specLo = -96.0f, specHi = -12.0f;
    auto specY = [&] (float db) { return juce::jmap (juce::jlimit (specLo, specHi, db), specLo, specHi, a.getBottom(), a.getY()); };

    juce::Path inPath, outPath;
    outPath.startNewSubPath (a.getX(), a.getBottom());
    for (int px = 0; px <= (int) a.getWidth(); px += 2)
    {
        const float f = xToFreq (a.getX() + (float) px, a);
        const float yi = specY (spectrumDbAt (specIn, f)), yo = specY (spectrumDbAt (specOut, f));
        if (px == 0) inPath.startNewSubPath (a.getX(), yi); else inPath.lineTo (a.getX() + (float) px, yi);
        outPath.lineTo (a.getX() + (float) px, yo);
    }
    outPath.lineTo (a.getRight(), a.getBottom());
    outPath.closeSubPath();
    g.setGradientFill (juce::ColourGradient (Colours::accent.withAlpha (0.28f), 0.0f, a.getY(), Colours::accent.withAlpha (0.03f), 0.0f, a.getBottom(), false));
    g.fillPath (outPath);
    g.setColour (Colours::input.brighter (0.5f).withAlpha (0.85f));
    g.strokePath (inPath, juce::PathStrokeType (1.3f));

    // ---- Target balance, aligned with the output spectrum ----------------------------------------------
    {
        const auto target = processor.getTargetCurve();
        float meanOut = 0.0f;
        for (int b = 2; b <= 7; ++b) meanOut += spectrumDbAt (specOut, kBandFreqs[(size_t) b]);
        meanOut /= 6.0f;
        if (meanOut > specLo + 6.0f)
        {
            juce::Path tp;
            for (int px = 0; px <= (int) a.getWidth(); px += 3)
            {
                const float f = xToFreq (a.getX() + (float) px, a);
                const float pos = juce::jlimit (0.0f, (float) kNumBands - 1.001f, std::log2 (f / kBandFreqs[0]));
                const int i0 = (int) pos;
                const float t = pos - (float) i0;
                const float db = target[(size_t) i0] + t * (target[(size_t) i0 + 1] - target[(size_t) i0]);
                const float y = specY (meanOut + db);
                if (px == 0) tp.startNewSubPath (a.getX(), y); else tp.lineTo (a.getX() + (float) px, y);
            }
            g.setColour (Colours::learn.withAlpha (0.85f));
            strokeDashed (g, tp, 1.6f, 2.0f, 4.0f);
        }
    }

    // ---- Response curves ---------------------------------------------------------------------------
    const bool eqOn = value (ParamID::eqOn) > 0.5f;
    std::array<Biquad, kEqBands> bands;
    for (int b = 0; b < kEqBands; ++b)
        EqModule::design (bands[(size_t) b], sr, b, value (BandIDs::eq[b][0]), value (BandIDs::eq[b][1]), value (BandIDs::eq[b][2]));
    const float hpf = value (ParamID::eqHpf), lpf = value (ParamID::eqLpf);
    Filter24 hp, lp;
    hp.setHighPass (sr, std::max (10.0f, hpf));
    lp.setLowPass (sr, std::min ((double) lpf, sr * 0.45));

    std::array<Biquad, kNumBands> stab;
    const bool stabOn = value (ParamID::stabOn) > 0.5f;
    for (int b = 0; b < kNumBands; ++b)
        stab[(size_t) b].setPeak (sr, std::min ((double) kBandFreqs[(size_t) b], sr * 0.45), 1.0, processor.meters.stab[(size_t) b].load());

    juce::Path eqPath, stabPath;
    for (int px = 0; px <= (int) a.getWidth(); px += 2)
    {
        const float f = xToFreq (a.getX() + (float) px, a);
        double db = 0.0;
        for (auto& b : bands) db += b.magnitudeDb (sr, f);
        if (hpf > 10.5f) db += hp.magnitudeDb (sr, f);
        if (lpf < 21500.0f) db += lp.magnitudeDb (sr, f);
        const float y = dbToY ((float) juce::jlimit (-40.0, 40.0, db), a);
        if (px == 0) eqPath.startNewSubPath (a.getX(), y); else eqPath.lineTo (a.getX() + (float) px, y);

        double sdb = 0.0;
        for (auto& b : stab) sdb += b.magnitudeDb (sr, f);
        const float sy = dbToY ((float) sdb, a);
        if (px == 0) stabPath.startNewSubPath (a.getX(), sy); else stabPath.lineTo (a.getX() + (float) px, sy);
    }
    if (stabOn)
    {
        g.setColour (Colours::learn.withAlpha (0.9f));
        strokeDashed (g, stabPath, 1.5f, 6.0f, 4.0f);
    }
    {
        juce::Path fill (eqPath);
        const float zero = dbToY (0.0f, a);
        fill.lineTo (a.getRight(), zero);
        fill.lineTo (a.getX(), zero);
        fill.closeSubPath();
        g.setColour (Colours::threshold.withAlpha (eqOn ? 0.12f : 0.04f));
        g.fillPath (fill);
        g.setColour (eqOn ? Colours::threshold : Colours::textDim);
        g.strokePath (eqPath, juce::PathStrokeType (2.0f));
    }

    // ---- Dynamic EQ bands: live gain + nodes --------------------------------------------------------------
    const bool dynOn = value (ParamID::deqOn) > 0.5f;
    if (dynOn)
    {
        for (int b = 0; b < kDynEqBands; ++b)
        {
            const float range = value (BandIDs::dynEq[b][3]);
            if (std::abs (range) < 0.05f) continue;
            Biquad live;
            live.setPeak (sr, value (BandIDs::dynEq[b][0]), std::max (0.3f, value (BandIDs::dynEq[b][1])), processor.meters.dynEq[(size_t) b].load());
            juce::Path lp2;
            const float zero = dbToY (0.0f, a);
            lp2.startNewSubPath (a.getX(), zero);
            for (int px = 0; px <= (int) a.getWidth(); px += 3)
            {
                const float f = xToFreq (a.getX() + (float) px, a);
                lp2.lineTo (a.getX() + (float) px, dbToY ((float) live.magnitudeDb (sr, f), a));
            }
            lp2.lineTo (a.getRight(), zero);
            lp2.closeSubPath();
            g.setColour (Colours::pink.withAlpha (0.35f));
            g.fillPath (lp2);
        }
    }

    auto drawNode = [&] (Hit h, juce::Colour c, const juce::String& label, bool selected, bool active)
    {
        const auto pos = nodePosition (h, a);
        const bool hov = (h.kind == hover.kind && h.band == hover.band) || (h.kind == dragging.kind && h.band == dragging.band);
        const float r = selected || hov ? 8.0f : 6.5f;
        g.setColour (selected ? c : Colours::panelLight);
        g.fillEllipse (juce::Rectangle<float> (r * 2, r * 2).withCentre (pos));
        g.setColour (c.withAlpha (active ? 1.0f : 0.4f));
        g.drawEllipse (juce::Rectangle<float> (r * 2, r * 2).withCentre (pos), 1.3f);
        g.setColour (selected ? Colours::background : Colours::text);
        g.setFont (uiFont (8.5f, true));
        g.drawText (label, juce::Rectangle<float> (20, 12).withCentre (pos), juce::Justification::centred);
    };
    if (dynOn)
        for (int b = 0; b < kDynEqBands; ++b)
            drawNode ({ 1, b }, Colours::pink, "D" + juce::String (b + 1), b == selectedDyn, std::abs (value (BandIDs::dynEq[b][3])) >= 0.05f);
    for (int b = 0; b < kEqBands; ++b)
        drawNode ({ 0, b }, Colours::threshold, juce::String (b + 1), b == selectedEq, eqOn);

    g.restoreState();
    g.setFont (uiFont (11.0f, true));
    g.setColour (Colours::text);
    g.drawText ("EQ / SPECTRUM", 10, 3, 140, 18, juce::Justification::centredLeft);
    drawLegend (g, { 0, 3, getWidth() - 10, 18 },
                { { "IN", Colours::input }, { "OUT", Colours::accent }, { "EQ", Colours::threshold }, { "DYN", Colours::pink },
                  { "TARGET", Colours::learn } });
}

//==============================================================================
LoudnessHistory::LoudnessHistory (MasterProcessor& p) : processor (p), frames ((size_t) capacity)
{
    setOpaque (true);
    setTooltip ("Loudness over time: short-term (thick) and momentary (thin) output, input (grey), maximizer gain reduction (red) "
                "and the target loudness. Mouse wheel zooms time");
}

void LoudnessHistory::addFrame (const LoudnessFrame& f)
{
    frames[(size_t) (count % capacity)] = f;
    ++count;
}

void LoudnessHistory::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    spanSeconds = juce::jlimit (5.0, 120.0, spanSeconds * (w.deltaY > 0 ? 0.85 : 1.0 / 0.85));
    repaint();
}

void LoudnessHistory::paint (juce::Graphics& g)
{
    drawFrame (g, getLocalBounds().toFloat());
    auto a = getLocalBounds().toFloat();
    a.removeFromTop (22.0f);
    a.removeFromBottom (18.0f);
    a.removeFromLeft (30.0f);
    a.removeFromRight (8.0f);
    auto yOf = [&] (float lufs) { return juce::jmap (juce::jlimit (lo, hi, lufs), lo, hi, a.getBottom(), a.getY()); };

    g.setFont (uiFont (9.5f));
    for (float v = lo; v <= hi + 0.1f; v += 6.0f)
    {
        g.setColour (Colours::grid);
        g.drawHorizontalLine ((int) yOf (v), a.getX(), a.getRight());
        g.setColour (Colours::textDim);
        g.drawText (juce::String ((int) v), 0, (int) yOf (v) - 6, 26, 12, juce::Justification::centredRight);
    }
    const int shown = (int) (spanSeconds * 10.0);
    const float dx = a.getWidth() / (float) shown;
    for (int s = 0; s <= (int) spanSeconds; s += spanSeconds > 40.0 ? 10 : 5)
    {
        const float x = a.getRight() - (float) s * 10.0f * dx;
        g.setColour (Colours::grid);
        g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
        g.setColour (Colours::textDim);
        g.drawText (s == 0 ? juce::String ("now") : "-" + juce::String (s) + "s", (int) x - 20, (int) a.getBottom() + 3, 40, 12,
                    juce::Justification::centred);
    }

    g.saveState();
    g.reduceClipRegion (a.toNearestInt());

    const int n = (int) std::min<int64_t> (count, std::min (shown, capacity));
    auto frameAt = [&] (int back) -> const LoudnessFrame& { return frames[(size_t) ((count - 1 - back) % capacity)]; };

    // Gain reduction bars from the top (0..12 dB = top third)
    g.setColour (Colours::danger.withAlpha (0.55f));
    for (int k = 0; k < n; ++k)
    {
        const float gr = frameAt (k).maxGrDb;
        if (gr < 0.05f) continue;
        const float x = a.getRight() - (float) (k + 1) * dx;
        g.fillRect (x, a.getY(), std::max (1.0f, dx), a.getHeight() * 0.33f * juce::jlimit (0.0f, 1.0f, gr / 12.0f));
    }

    auto line = [&] (auto member, juce::Colour c, float thickness)
    {
        juce::Path p;
        bool started = false;
        for (int k = 0; k < n; ++k)
        {
            const float v = frameAt (k).*member;
            const float x = a.getRight() - (float) k * dx;
            if (v <= kSilenceLufs + 1.0f) { started = false; continue; }
            if (! started) { p.startNewSubPath (x, yOf (v)); started = true; }
            else p.lineTo (x, yOf (v));
        }
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };
    line (&LoudnessFrame::inShortTerm, Colours::input, 1.4f);
    line (&LoudnessFrame::momentary, Colours::accent.withAlpha (0.35f), 1.0f);
    line (&LoudnessFrame::shortTerm, Colours::accent, 2.2f);

    // Target
    const float target = processor.getState().getRawParameterValue (ParamID::targetLufs)->load();
    {
        juce::Path tl;
        tl.startNewSubPath (a.getX(), yOf (target));
        tl.lineTo (a.getRight(), yOf (target));
        g.setColour (Colours::learn);
        strokeDashed (g, tl, 1.4f, 6.0f, 4.0f);
        g.setFont (uiFont (10.0f, true));
        g.drawText ("TARGET " + juce::String (target, 1), (int) a.getX() + 6, (int) yOf (target) - 15, 120, 13, juce::Justification::centredLeft);
    }
    g.restoreState();

    g.setFont (uiFont (11.0f, true));
    g.setColour (Colours::text);
    g.drawText ("LOUDNESS (LUFS)", 10, 3, 160, 18, juce::Justification::centredLeft);
    drawLegend (g, { 0, 3, getWidth() - 10, 18 },
                { { "SHORT", Colours::accent }, { "MOM", Colours::accent.withAlpha (0.4f) }, { "IN", Colours::input }, { "GR", Colours::danger } });
}

//==============================================================================
StereoScope::StereoScope (MasterProcessor& p) : processor (p), l ((size_t) points, 0.0f), r ((size_t) points, 0.0f)
{
    setOpaque (true);
    setTooltip ("Vectorscope: vertical = mid (mono), horizontal = side (stereo). The bar shows the phase correlation "
                "(+1 mono, 0 wide, below 0 = phase problems)");
}

void StereoScope::pull()
{
    float a[1024], b[1024];
    for (;;)
    {
        const int na = processor.scopeL.pop (a, 1024);
        const int nb = processor.scopeR.pop (b, na);
        const int n = std::min (na, nb);
        for (int i = 0; i < n; ++i)
        {
            l[(size_t) pos] = a[i];
            r[(size_t) pos] = b[i];
            pos = (pos + 1) % points;
        }
        if (na < 1024) break;
    }
    correlation += 0.3f * (processor.meters.correlation.load() - correlation);
    double em = 0.0, es = 0.0;
    for (int i = 0; i < points; ++i)
    {
        const double m = 0.5 * (l[(size_t) i] + r[(size_t) i]), s = 0.5 * (l[(size_t) i] - r[(size_t) i]);
        em += m * m;
        es += s * s;
    }
    width = (float) (es / (em + es + 1.0e-12));
    repaint();
}

void StereoScope::paint (juce::Graphics& g)
{
    drawFrame (g, getLocalBounds().toFloat());
    g.setFont (uiFont (11.0f, true));
    g.setColour (Colours::text);
    g.drawText ("STEREO", 10, 3, 100, 18, juce::Justification::centredLeft);

    auto area = getLocalBounds().toFloat().reduced (10.0f, 0.0f);
    area.removeFromTop (24.0f);
    auto bottom = area.removeFromBottom (40.0f);
    const float size = std::min (area.getWidth(), area.getHeight());
    const auto sq = area.withSizeKeepingCentre (size, size);
    const auto c = sq.getCentre();
    const float rad = size * 0.5f;

    g.setColour (Colours::grid);
    g.drawEllipse (sq.reduced (1.0f), 1.0f);
    g.drawLine (c.x, sq.getY(), c.x, sq.getBottom());
    g.drawLine (sq.getX(), c.y, sq.getRight(), c.y);
    const float d = rad * 0.7071f;
    g.drawLine (c.x - d, c.y - d, c.x + d, c.y + d, 0.7f);
    g.drawLine (c.x - d, c.y + d, c.x + d, c.y - d, 0.7f);
    g.setColour (Colours::textDim);
    g.setFont (uiFont (9.0f, true));
    g.drawText ("M", (int) c.x - 8, (int) sq.getY() + 2, 16, 10, juce::Justification::centred);
    g.drawText ("L", (int) (c.x - d) - 12, (int) (c.y - d) - 10, 12, 10, juce::Justification::centred);
    g.drawText ("R", (int) (c.x + d), (int) (c.y - d) - 10, 12, 10, juce::Justification::centred);

    float peak = 1.0e-4f;
    for (int i = 0; i < points; ++i) peak = std::max ({ peak, std::abs (l[(size_t) i]), std::abs (r[(size_t) i]) });
    const float scale = rad * 0.95f / std::max (0.05f, peak);
    g.setColour (Colours::accent.withAlpha (0.45f));
    for (int i = 0; i < points; i += 2)
    {
        const float s = 0.7071f * (r[(size_t) i] - l[(size_t) i]) * scale;
        const float m = 0.7071f * (l[(size_t) i] + r[(size_t) i]) * scale;
        const float x = juce::jlimit (sq.getX(), sq.getRight(), c.x - s);
        const float y = juce::jlimit (sq.getY(), sq.getBottom(), c.y - m);
        g.fillRect (x, y, 1.5f, 1.5f);
    }

    // Correlation bar
    auto bar = bottom.removeFromTop (12.0f).reduced (0.0f, 2.0f).translated (0.0f, 6.0f);
    g.setColour (Colours::panelLight);
    g.fillRoundedRectangle (bar, 3.0f);
    const float x0 = bar.getCentreX();
    const float xv = bar.getX() + bar.getWidth() * (juce::jlimit (-1.0f, 1.0f, correlation) + 1.0f) * 0.5f;
    g.setColour (correlation < 0.0f ? Colours::danger : Colours::accent);
    g.fillRect (juce::Rectangle<float> (std::min (x0, xv), bar.getY(), std::abs (xv - x0), bar.getHeight()));
    g.setColour (Colours::text);
    g.fillRect (xv - 1.0f, bar.getY() - 2.0f, 2.0f, bar.getHeight() + 4.0f);
    g.setFont (uiFont (9.5f));
    g.setColour (Colours::textDim);
    auto labels = bottom.translated (0.0f, 6.0f);
    g.drawText ("-1", labels.removeFromLeft (20.0f), juce::Justification::centredLeft);
    g.drawText ("+1", labels.removeFromRight (20.0f), juce::Justification::centredRight);
    g.setColour (Colours::text);
    g.drawText ("CORR " + juce::String (correlation, 2) + "   WIDTH " + juce::String (juce::roundToInt (width * 100.0f)) + "%", labels,
                juce::Justification::centred);
}

//==============================================================================
LoudnessPanel::LoudnessPanel (MasterProcessor& p) : processor (p)
{
    setOpaque (true);
    setTooltip ("Output loudness (EBU R128). Click to restart the integrated measurement");
}

void LoudnessPanel::tick()
{
    auto& m = processor.meters;
    integrated = m.integrated.load();
    shortTerm = m.shortTerm.load();
    momentary = m.momentary.load();
    range = m.range.load();
    truePeak = m.truePeakMax.load();
    inIntegrated = m.inIntegrated.load();
    repaint();
}

void LoudnessPanel::mouseDown (const juce::MouseEvent&)
{
    processor.requestLoudnessReset();
}

void LoudnessPanel::paint (juce::Graphics& g)
{
    drawFrame (g, getLocalBounds().toFloat());
    auto& s = processor.getState();
    const float target = s.getRawParameterValue (ParamID::targetLufs)->load();
    const float ceiling = s.getRawParameterValue (ParamID::maxCeiling)->load();
    auto r = getLocalBounds().reduced (12, 6);

    g.setColour (Colours::textDim);
    g.setFont (uiFont (10.5f, true));
    g.drawText ("INTEGRATED", r.removeFromTop (18), juce::Justification::centredLeft);

    const bool valid = integrated > kSilenceLufs + 1.0f;
    const float diff = integrated - target;
    const juce::Colour big = ! valid ? Colours::textDim : (std::abs (diff) <= 1.0f ? Colours::accent : (diff > 0.0f ? Colours::threshold : Colours::text));
    g.setColour (big);
    g.setFont (uiFont (36.0f, true));
    auto bigRow = r.removeFromTop (40);
    g.drawText (lufsText (integrated), bigRow, juce::Justification::centredLeft);
    g.setColour (Colours::textDim);
    g.setFont (uiFont (11.0f, true));
    g.drawText ("LUFS", bigRow.withTrimmedLeft (98).withTrimmedTop (14), juce::Justification::centredLeft);

    g.setFont (uiFont (10.5f));
    g.setColour (valid ? big : Colours::textDim);
    g.drawText (valid ? (diff >= 0 ? "+" : "") + juce::String (diff, 1) + " LU vs target " + juce::String (target, 1) : juce::String ("target " + juce::String (target, 1)),
                r.removeFromTop (16), juce::Justification::centredLeft);
    r.removeFromTop (8);

    auto row = [&] (const juce::String& label, const juce::String& v, juce::Colour c)
    {
        auto line = r.removeFromTop (21);
        g.setFont (uiFont (10.5f, true));
        g.setColour (Colours::textDim);
        g.drawText (label, line, juce::Justification::centredLeft);
        g.setFont (uiFont (13.0f, true));
        g.setColour (c);
        g.drawText (v, line, juce::Justification::centredRight);
    };
    row ("SHORT-TERM", lufsText (shortTerm), Colours::text);
    row ("MOMENTARY", lufsText (momentary), Colours::text);
    row ("RANGE", juce::String (range, 1) + " LU", Colours::text);
    row ("TRUE PEAK", truePeak <= -99.0f ? juce::String ("--") : juce::String (truePeak, 1) + " dB",
         truePeak > ceiling + 0.05f ? Colours::danger : Colours::text);
    row ("PLR", valid && truePeak > -99.0f ? juce::String (truePeak - integrated, 1) + " LU" : juce::String ("--"), Colours::text);
    row ("INPUT", lufsText (inIntegrated), Colours::input.brighter (0.4f));

    g.setFont (uiFont (9.5f));
    g.setColour (Colours::textDim.withAlpha (0.7f));
    g.drawText ("click to reset", getLocalBounds().reduced (12, 6).removeFromBottom (14), juce::Justification::centredLeft);
}

//==============================================================================
int ChainStrip::pageOf (ModuleId m)
{
    switch (m)
    {
        case ModuleId::eq: case ModuleId::stabilizer: case ModuleId::dyneq: return pageTone;
        case ModuleId::glue: case ModuleId::dynamics: return pageDynamics;
        case ModuleId::exciter: case ModuleId::imager: case ModuleId::numModules: break;
    }
    return pageStereo;
}

ChainStrip::ChainStrip (MasterProcessor& p) : processor (p), state (p.getState())
{
    order = processor.getModuleOrder();
    rebuildTiles();
    setTooltip ("Click a stage to edit it, click its dot to switch it on/off, drag the modules to change their order");
}

void ChainStrip::rebuildTiles()
{
    tiles.clear();
    tiles.push_back ({ "input", "INPUT", {}, Colours::textDim, -1, -1, {} });
    for (int m : order)
        tiles.push_back ({ juce::String ("m") + juce::String (m), moduleShortName ((ModuleId) m), moduleEnableID ((ModuleId) m),
                           moduleColour ((ModuleId) m), pageOf ((ModuleId) m), m, {} });
    tiles.push_back ({ "max", "MAXIMIZER", ParamID::maxOn, Colours::danger, pageDynamics, -1, {} });
    tiles.push_back ({ "output", "OUTPUT", {}, Colours::textDim, -1, -1, {} });
    layoutTiles();
}

void ChainStrip::layoutTiles()
{
    const float gap = 12.0f;
    const float w = ((float) getWidth() - gap * (float) (tiles.size() - 1)) / (float) tiles.size();
    float x = 0.0f;
    for (auto& t : tiles)
    {
        t.bounds = { x, 0.0f, w, (float) getHeight() };
        x += w + gap;
    }
}

juce::Rectangle<float> ChainStrip::powerRect (const Tile& t) const
{
    return { t.bounds.getX() + 7.0f, t.bounds.getY() + 8.0f, 11.0f, 11.0f };
}

bool ChainStrip::tileEnabled (const Tile& t) const
{
    return t.enableId.isEmpty() || state.getRawParameterValue (t.enableId)->load() > 0.5f;
}

juce::String ChainStrip::tileValue (const Tile& t) const
{
    auto gr = [] (float v) { return (v > 0.05f ? "-" : "") + juce::String (std::abs (v), 1) + " dB"; };
    auto value = [this] (const char* id) { return state.getRawParameterValue (id)->load(); };
    auto& m = processor.meters;
    if (t.key == "input") return (value (ParamID::inGain) > 0.05f ? "+" : "") + juce::String (value (ParamID::inGain), 1) + " dB";
    if (t.key == "output") return lufsText (m.shortTerm.load()) + " LUFS";
    if (t.key == "max") return "+" + juce::String (value (ParamID::maxGain), 1) + "  GR " + juce::String (m.maximizer.load(), 1);
    if (t.module < 0)
        return {};
    switch ((ModuleId) t.module)
    {
        case ModuleId::eq:
        {
            int active = 0;
            for (int b = 0; b < kEqBands; ++b) active += std::abs (value (BandIDs::eq[b][1])) > 0.05f ? 1 : 0;
            return juce::String (active) + (active == 1 ? " band" : " bands");
        }
        case ModuleId::stabilizer: return targetStyleName ((TargetStyle) juce::roundToInt (value (ParamID::targetStyle)));
        case ModuleId::dyneq:
        {
            float g = 0.0f;
            for (auto& v : m.dynEq) if (std::abs (v.load()) > std::abs (g)) g = v.load();
            return (g > 0.05f ? "+" : "") + juce::String (g, 1) + " dB";
        }
        case ModuleId::glue: return gr (m.glue.load());
        case ModuleId::dynamics:
        {
            float g = 0.0f;
            for (auto& v : m.mb) g = std::max (g, v.load());
            return gr (g);
        }
        case ModuleId::exciter:
        {
            static const char* modes[] = { "Tape", "Tube", "Warm", "Triode", "Retro" };
            return modes[juce::jlimit (0, 4, juce::roundToInt (value (ParamID::excMode)))];
        }
        case ModuleId::imager:
        {
            const float mono = value (ParamID::imgMonoBass);
            return mono > 20.5f ? "Mono < " + juce::String (juce::roundToInt (mono)) : "Corr " + juce::String (m.correlation.load(), 2);
        }
        case ModuleId::numModules: break;
    }
    return {};
}

void ChainStrip::tick()
{
    const auto o = processor.getModuleOrder();
    if (o != order && dragTile < 0)
    {
        order = o;
        rebuildTiles();
    }
    repaint();
}

void ChainStrip::paint (juce::Graphics& g)
{
    if (tiles.empty())
        return;
    if (tiles.front().bounds.getWidth() <= 0.0f)
        layoutTiles();

    for (size_t i = 0; i + 1 < tiles.size(); ++i)
    {
        const auto& a = tiles[i].bounds;
        const float x = a.getRight() + 6.0f, y = a.getCentreY();
        juce::Path arrow;
        arrow.startNewSubPath (x - 3.0f, y - 4.0f);
        arrow.lineTo (x + 1.5f, y);
        arrow.lineTo (x - 3.0f, y + 4.0f);
        g.setColour (Colours::textDim.withAlpha (0.6f));
        g.strokePath (arrow, juce::PathStrokeType (1.4f));
    }

    {
        float x0 = 1.0e9f, x1 = -1.0e9f;
        for (auto& t : tiles)
            if (t.module >= 0) { x0 = std::min (x0, t.bounds.getX()); x1 = std::max (x1, t.bounds.getRight()); }
        g.setColour (Colours::edge);
        g.drawHorizontalLine (getHeight() - 1, x0, x1);
    }

    for (size_t i = 0; i < tiles.size(); ++i)
    {
        const auto& t = tiles[i];
        auto b = t.bounds;
        if ((int) i == dragTile)
            b = b.withX (dragX - dragOffset);
        const bool on = tileEnabled (t);
        const bool hov = (int) i == hoverTile;

        g.setColour ((int) i == dragTile ? Colours::panelLight.brighter (0.15f) : (hov ? Colours::panelLight.brighter (0.06f) : Colours::panelLight));
        g.fillRoundedRectangle (b.reduced (0.5f, 1.5f), 5.0f);
        g.setColour (on ? t.colour.withAlpha (0.8f) : Colours::edge);
        g.drawRoundedRectangle (b.reduced (0.5f, 1.5f), 5.0f, on ? 1.2f : 1.0f);
        if (on)
        {
            g.setColour (t.colour);
            g.fillRoundedRectangle (b.getX() + 6.0f, b.getBottom() - 5.0f, b.getWidth() - 12.0f, 2.0f, 1.0f);
        }
        if (t.enableId.isNotEmpty())
        {
            auto pr = powerRect (t).withX (b.getX() + 7.0f);
            g.setColour (on ? t.colour : Colours::graphBg);
            g.fillEllipse (pr);
            g.setColour (on ? t.colour.brighter (0.3f) : Colours::textDim);
            g.drawEllipse (pr, 1.0f);
        }
        g.setFont (uiFont (11.0f, true));
        g.setColour (on ? Colours::text : Colours::textDim);
        g.drawText (t.name, b.withTrimmedLeft (t.enableId.isNotEmpty() ? 20.0f : 6.0f).withHeight (26.0f).translated (0.0f, 2.0f),
                    t.enableId.isNotEmpty() ? juce::Justification::centredLeft : juce::Justification::centred);
        g.setFont (uiFont (10.0f));
        g.setColour (on ? t.colour.withAlpha (0.9f) : Colours::textDim.withAlpha (0.6f));
        g.drawText (on ? tileValue (t) : juce::String ("off"), b.withTrimmedTop (22.0f).withTrimmedBottom (6.0f).reduced (6.0f, 0.0f),
                    juce::Justification::centred);
    }

    if (dragTile >= 0 && dragTarget >= 0)
    {
        const auto& t = tiles[(size_t) dragTarget];
        const float x = dragTarget > dragTile ? t.bounds.getRight() + 6.0f : t.bounds.getX() - 6.0f;
        g.setColour (Colours::accent);
        g.fillRoundedRectangle (x - 1.5f, 2.0f, 3.0f, (float) getHeight() - 4.0f, 1.5f);
    }
}

void ChainStrip::mouseMove (const juce::MouseEvent& e)
{
    int h = -1;
    for (size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i].bounds.contains (e.position)) h = (int) i;
    if (h != hoverTile)
    {
        hoverTile = h;
        setMouseCursor (h >= 0 && tiles[(size_t) h].module >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::PointingHandCursor);
        repaint();
    }
}

void ChainStrip::mouseDown (const juce::MouseEvent& e)
{
    dragTile = -1;
    dragMoved = false;
    for (size_t i = 0; i < tiles.size(); ++i)
    {
        const auto& t = tiles[i];
        if (! t.bounds.contains (e.position))
            continue;
        if (t.enableId.isNotEmpty() && powerRect (t).expanded (4.0f).contains (e.position))
        {
            if (auto* p = state.getParameter (t.enableId))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.0f : 1.0f);
                p->endChangeGesture();
            }
            return;
        }
        dragTile = (int) i;
        dragOffset = e.position.x - t.bounds.getX();
        dragX = e.position.x;
        return;
    }
}

void ChainStrip::mouseDrag (const juce::MouseEvent& e)
{
    if (dragTile < 0 || tiles[(size_t) dragTile].module < 0)
        return;
    if (! dragMoved && std::abs (e.getDistanceFromDragStartX()) < 5)
        return;
    dragMoved = true;
    dragX = e.position.x;
    dragTarget = -1;
    for (size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i].module >= 0 && e.position.x >= tiles[i].bounds.getX() - 6.0f && e.position.x <= tiles[i].bounds.getRight() + 6.0f)
            dragTarget = (int) i;
    if (dragTarget < 0)
    {
        int firstM = -1, lastM = -1;
        for (size_t i = 0; i < tiles.size(); ++i)
            if (tiles[i].module >= 0) { if (firstM < 0) firstM = (int) i; lastM = (int) i; }
        dragTarget = e.position.x < tiles[(size_t) firstM].bounds.getX() ? firstM : lastM;
    }
    repaint();
}

void ChainStrip::mouseUp (const juce::MouseEvent&)
{
    if (dragTile >= 0 && ! dragMoved)
    {
        if (onTileClicked)
            onTileClicked (tiles[(size_t) dragTile].page, tiles[(size_t) dragTile].key);
    }
    else if (dragTile >= 0 && dragTarget >= 0 && dragTarget != dragTile)
    {
        int firstM = -1;
        for (size_t i = 0; i < tiles.size(); ++i)
            if (tiles[i].module >= 0) { firstM = (int) i; break; }
        const int from = dragTile - firstM, to = dragTarget - firstM;
        std::vector<int> o (order.begin(), order.end());
        const int moved = o[(size_t) from];
        o.erase (o.begin() + from);
        o.insert (o.begin() + to, moved);
        for (int i = 0; i < kNumOrderable; ++i) order[(size_t) i] = o[(size_t) i];
        processor.setModuleOrder (order);
        rebuildTiles();
        if (onOrderChanged)
            onOrderChanged();
    }
    dragTile = dragTarget = -1;
    dragMoved = false;
    repaint();
}
} // namespace lam
