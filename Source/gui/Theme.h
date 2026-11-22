#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace lam
{
namespace Colours
{
    inline const juce::Colour background  { 0xff0e1116 };
    inline const juce::Colour panel       { 0xff161a21 };
    inline const juce::Colour panelLight  { 0xff1d222b };
    inline const juce::Colour edge        { 0xff2a313c };
    inline const juce::Colour graphBg     { 0xff0a0c10 };
    inline const juce::Colour grid        { 0xff1f252e };
    inline const juce::Colour text        { 0xffdfe5ee };
    inline const juce::Colour textDim     { 0xff7c8796 };
    inline const juce::Colour accent      { 0xff2fe0cf };   // teal: output, loudness
    inline const juce::Colour accentDim   { 0xff1a7d74 };
    inline const juce::Colour threshold   { 0xffffa53d };   // orange: EQ, thresholds
    inline const juce::Colour danger      { 0xffff4d6a };   // gain reduction, limits
    inline const juce::Colour learn       { 0xffb08cff };   // assistant, targets
    inline const juce::Colour input       { 0xff5b6b82 };   // input spectrum
    inline const juce::Colour detection   { 0xfff4e06d };   // yellow
    inline const juce::Colour blue        { 0xff5aa9ff };
    inline const juce::Colour pink        { 0xffff7ab8 };

    /** Colours for the low / mid / high bands of the multiband modules. */
    inline juce::Colour forBand (int b)
    {
        static const juce::Colour c[] = { juce::Colour (0xffff7a59), juce::Colour (0xfff4e06d), juce::Colour (0xff5aa9ff) };
        return c[((b % 3) + 3) % 3];
    }
} // namespace Colours

inline juce::Font uiFont (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

class MasterLookAndFeel : public juce::LookAndFeel_V4
{
public:
    MasterLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                           float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool isMouseOverButton, bool isButtonDown) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override { return uiFont (13.0f); }
    juce::Font getPopupMenuFont() override { return uiFont (14.0f); }
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
};
} // namespace lam
