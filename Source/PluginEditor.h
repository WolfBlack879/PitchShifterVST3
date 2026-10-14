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

    void setupKnob (Knob&, const juce::String& paramId, const juce::String& name, double defaultValue);

    BassPitchShifterProcessor& processor;
    MinimalLookAndFeel lnf;
    Knob pitch, fine, mix, output;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassPitchShifterEditor)
};
