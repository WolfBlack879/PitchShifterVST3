#include "PluginEditor.h"

namespace Colours
{
    const juce::Colour background { 0xff16181d };
    const juce::Colour panel      { 0xff1f2229 };
    const juce::Colour track      { 0xff2e323b };
    const juce::Colour accent     { 0xffff8a3d };
    const juce::Colour text       { 0xffe6e6e6 };
    const juce::Colour dimText    { 0xff8a8f99 };
}

//==============================================================================
MinimalLookAndFeel::MinimalLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, Colours::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, Colours::accent.withAlpha (0.4f));
    setColour (juce::Label::textColourId, Colours::dimText);
    setColour (juce::TextEditor::textColourId, Colours::text);
    setColour (juce::TextEditor::highlightColourId, Colours::accent.withAlpha (0.4f));
    setColour (juce::CaretComponent::caretColourId, Colours::accent);
    setColour (juce::TextButton::buttonColourId, Colours::panel);
    setColour (juce::TextButton::buttonOnColourId, Colours::accent);
    setColour (juce::TextButton::textColourOffId, Colours::dimText);
    setColour (juce::TextButton::textColourOnId, Colours::background);
    setColour (juce::ComboBox::outlineColourId, Colours::track);
}

void MinimalLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (6.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float lineW = juce::jmax (3.0f, radius * 0.12f);
    const float arcRadius = radius - lineW * 0.5f;
    const float angle = startAngle + sliderPos * (endAngle - startAngle);

    // Bipolar knobs (range crosses zero) draw their arc from the centre.
    const auto range = slider.getRange();
    const bool bipolar = range.getStart() < 0.0 && range.getEnd() > 0.0;
    const float zeroAngle = bipolar
        ? startAngle + (float) slider.valueToProportionOfLength (0.0) * (endAngle - startAngle)
        : startAngle;

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (Colours::track);
    g.strokePath (track, { lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });

    if (std::abs (angle - zeroAngle) > 0.001f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                             juce::jmin (zeroAngle, angle), juce::jmax (zeroAngle, angle), true);
        g.setColour (Colours::accent);
        g.strokePath (value, { lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });
    }

    const float innerR = arcRadius - lineW * 1.6f;
    g.setColour (Colours::panel.brighter (0.15f));
    g.fillEllipse (juce::Rectangle<float> (innerR * 2.0f, innerR * 2.0f).withCentre (centre));

    const auto tip = centre.getPointOnCircumference (innerR * 0.8f, angle);
    const auto base = centre.getPointOnCircumference (innerR * 0.25f, angle);
    g.setColour (Colours::text);
    g.drawLine ({ base, tip }, juce::jmax (2.0f, lineW * 0.6f));
}

juce::Label* MinimalLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (slider);
    l->setFont (juce::FontOptions (14.0f));
    l->setJustificationType (juce::Justification::centred);
    return l;
}

//==============================================================================
BassPitchShifterEditor::BassPitchShifterEditor (BassPitchShifterProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setLookAndFeel (&lnf);

    setupKnob (pitch,  "semitones", "PITCH",  0.0);
    setupKnob (fine,   "cents",     "FINE",   0.0);
    setupKnob (mix,    "mix",       "MIX",    100.0);
    setupKnob (output, "output",    "OUTPUT", 0.0);

    setupToggle (mode, "mode",
                 "STUDIO", "Best quality, ~75 ms latency. For recording and mixing.",
                 "LIVE",   "Low latency (about half a note period). For playing live, single notes.");
    setupToggle (width, "width",
                 "MONO",   "Sums left and right, shifts once and outputs the same signal on both sides.",
                 "STEREO", "Keeps the stereo image; both sides are shifted together.");

    setSize (540, 230);
}

BassPitchShifterEditor::~BassPitchShifterEditor()
{
    setLookAndFeel (nullptr);
}

void BassPitchShifterEditor::setupKnob (Knob& k, const juce::String& paramId, const juce::String& name, double defaultValue)
{
    k.slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    k.slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                                  juce::MathConstants<float>::pi * 2.75f, true);
    k.slider.setDoubleClickReturnValue (true, defaultValue);
    addAndMakeVisible (k.slider);

    k.label.setText (name, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    k.label.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    addAndMakeVisible (k.label);

    k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, paramId, k.slider);
}

void BassPitchShifterEditor::setupToggle (Toggle& t, const juce::String& paramId,
                                          const juce::String& firstText, const juce::String& firstTip,
                                          const juce::String& secondText, const juce::String& secondTip)
{
    t.first.setButtonText (firstText);
    t.second.setButtonText (secondText);
    t.first.setTooltip (firstTip);
    t.second.setTooltip (secondTip);
    t.first.setConnectedEdges (juce::Button::ConnectedOnRight);
    t.second.setConnectedEdges (juce::Button::ConnectedOnLeft);
    addAndMakeVisible (t.first);
    addAndMakeVisible (t.second);

    t.attachment = std::make_unique<juce::ParameterAttachment> (*processor.apvts.getParameter (paramId),
        [&t] (float value)
        {
            t.first.setToggleState ((int) value == 0, juce::dontSendNotification);
            t.second.setToggleState ((int) value == 1, juce::dontSendNotification);
        });

    t.first.onClick  = [&t] { t.attachment->setValueAsCompleteGesture (0.0f); };
    t.second.onClick = [&t] { t.attachment->setValueAsCompleteGesture (1.0f); };
    t.attachment->sendInitialUpdate();
}

void BassPitchShifterEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);

    auto header = getLocalBounds().removeFromTop (40).reduced (16, 0);
    g.setColour (Colours::text);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("BASS PITCH SHIFTER", header, juce::Justification::centredLeft);

    g.setColour (Colours::panel);
    g.fillRoundedRectangle (getLocalBounds().withTrimmedTop (40).reduced (12).toFloat(), 8.0f);
}

void BassPitchShifterEditor::resized()
{
    auto header = getLocalBounds().removeFromTop (40).reduced (16, 9);
    mode.setBounds (header.removeFromRight (136));
    header.removeFromRight (10);
    width.setBounds (header.removeFromRight (136));

    auto area = getLocalBounds().withTrimmedTop (40).reduced (20, 18);

    // Pitch gets the biggest knob; the rest share the remaining space.
    auto pitchArea = area.removeFromLeft (area.getWidth() * 34 / 100);
    const int smallW = area.getWidth() / 3;

    auto place = [] (Knob& k, juce::Rectangle<int> r)
    {
        k.label.setBounds (r.removeFromTop (18));
        k.slider.setBounds (r);
    };

    place (pitch, pitchArea);
    place (fine,   area.removeFromLeft (smallW).reduced (4, 14));
    place (mix,    area.removeFromLeft (smallW).reduced (4, 14));
    place (output, area.reduced (4, 14));
}
