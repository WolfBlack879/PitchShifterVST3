#include "PluginProcessor.h"
#include "PluginEditor.h"

BassPitchShifterProcessor::BassPitchShifterProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    semitonesParam = apvts.getRawParameterValue ("semitones");
    centsParam     = apvts.getRawParameterValue ("cents");
    mixParam       = apvts.getRawParameterValue ("mix");
    outputParam    = apvts.getRawParameterValue ("output");
}

juce::AudioProcessorValueTreeState::ParameterLayout BassPitchShifterProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "semitones", 1 }, "Pitch", -24, 12, 0,
        AudioParameterIntAttributes().withLabel ("st")
            .withStringFromValueFunction ([] (int v, int) { return (v > 0 ? "+" : "") + String (v) + " st"; })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "cents", 1 }, "Fine",
        NormalisableRange<float> (-100.0f, 100.0f, 1.0f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("ct")
            .withStringFromValueFunction ([] (float v, int) { return (v > 0 ? "+" : "") + String ((int) std::round (v)) + " ct"; })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix",
        NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f,
        AudioParameterFloatAttributes().withLabel ("%")
            .withStringFromValueFunction ([] (float v, int) { return String ((int) std::round (v)) + " %"; })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output",
        NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")
            .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })));

    return layout;
}

bool BassPitchShifterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void BassPitchShifterProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    for (auto& s : shifters)
        s.prepare (sampleRate);

    const int latency = shifters[0].getLatencySamples();
    setLatencySamples (latency);

    dryDelay.setMaximumDelayInSamples (latency + 1);
    dryDelay.prepare ({ sampleRate, (juce::uint32) samplesPerBlock, (juce::uint32) maxChannels });
    dryDelay.setDelay ((float) latency);
    dryDelay.reset();

    dryBuffer.setSize (maxChannels, samplesPerBlock);

    mixSmoothed.reset (sampleRate, 0.05);
    gainSmoothed.reset (sampleRate, 0.05);
    mixSmoothed.setCurrentAndTargetValue (mixParam->load() * 0.01f);
    gainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));
}

double BassPitchShifterProcessor::getTailLengthSeconds() const
{
    return getSampleRate() > 0.0 ? (double) shifters[0].getLatencySamples() / getSampleRate() : 0.0;
}

void BassPitchShifterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), maxChannels);

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    const float semis = semitonesParam->load() + centsParam->load() * 0.01f;
    const float ratio = std::pow (2.0f, semis / 12.0f);

    mixSmoothed.setTargetValue (mixParam->load() * 0.01f);
    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    if (dryBuffer.getNumSamples() < numSamples)
        dryBuffer.setSize (maxChannels, numSamples, false, false, true);

    // Delay the dry path by the shifter's latency so wet/dry stay aligned.
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* in = buffer.getReadPointer (ch);
        float* dry = dryBuffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
        {
            dryDelay.pushSample (ch, in[i]);
            dry[i] = dryDelay.popSample (ch);
        }

        shifters[ch].setRatio (ratio);
        shifters[ch].process (buffer.getWritePointer (ch), numSamples);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const float mix  = mixSmoothed.getNextValue();
        const float gain = gainSmoothed.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* wet = buffer.getWritePointer (ch);
            const float dry = dryBuffer.getReadPointer (ch)[i];
            wet[i] = (dry + mix * (wet[i] - dry)) * gain;
        }
    }
}

juce::AudioProcessorEditor* BassPitchShifterProcessor::createEditor()
{
    return new BassPitchShifterEditor (*this);
}

void BassPitchShifterProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void BassPitchShifterProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BassPitchShifterProcessor();
}
