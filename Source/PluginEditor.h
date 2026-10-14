#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class MinimalLookAndFeel : public juce::LookAndFeel_V4
{
public:
    MinimalLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float startAngle, float endAngle, juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
};

class BassPitchShifterEditor : public juce::AudioProcessorEditor
{
public:
    explicit BassPitchShifterEditor (BassPitchShifterProcessor&);
    ~BassPitchShifterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    /** Two-way segmented switch bound to a two-option choice parameter. */
    struct Toggle
    {
        juce::TextButton first, second;
        std::unique_ptr<juce::ParameterAttachment> attachment;

        void setBounds (juce::Rectangle<int> r)
        {
            first.setBounds (r.removeFromLeft (r.getWidth() / 2));
            second.setBounds (r);
        }
    };

    void setupKnob (Knob&, const juce::String& paramId, const juce::String& name, double defaultValue);
    void setupToggle (Toggle&, const juce::String& paramId,
                      const juce::String& firstText, const juce::String& firstTip,
                      const juce::String& secondText, const juce::String& secondTip);

    BassPitchShifterProcessor& processor;
    MinimalLookAndFeel lnf;
    Knob pitch, fine, mix, output;

    Toggle mode, width;
    juce::TooltipWindow tooltips { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassPitchShifterEditor)
};
