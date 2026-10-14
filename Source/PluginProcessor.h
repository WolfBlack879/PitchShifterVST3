#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "PitchShifter.h"
#include "LiveShifter.h"

class BassPitchShifterProcessor : public juce::AudioProcessor,
                                  private juce::AsyncUpdater
{
public:
    BassPitchShifterProcessor();
    ~BassPitchShifterProcessor() override = default;

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
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    enum Mode { studio = 0, live = 1 };
    enum Width { mono = 0, stereo = 1 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void handleAsyncUpdate() override;
    int latencyForMode (int mode) const;

    std::atomic<float>* semitonesParam = nullptr;
    std::atomic<float>* centsParam     = nullptr;
    std::atomic<float>* mixParam       = nullptr;
    std::atomic<float>* outputParam    = nullptr;
    std::atomic<float>* modeParam      = nullptr;
    std::atomic<float>* widthParam     = nullptr;

    std::atomic<int> currentMode { live };
    int currentWidth = stereo;

    static constexpr int maxChannels = 2;
    PitchShifter shifter;     // studio: high quality, ~75 ms
    LiveShifter liveShifter;  // live: pitch-synchronous, ~half a note period
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelay;

    juce::AudioBuffer<float> dryBuffer;
    juce::SmoothedValue<float> mixSmoothed, gainSmoothed;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassPitchShifterProcessor)
};
