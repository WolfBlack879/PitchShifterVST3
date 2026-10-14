#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <complex>
#include <vector>

/**
    Stereo-linked phase-vocoder pitch shifter with peak (identity) phase
    locking, after Laroche & Dolson, "New phase-vocoder techniques for
    pitch-shifting, harmonizing and other exotic effects" (WASPAA 1999).

    - A long FFT (4096 at 44.1/48 kHz) with 8x overlap gives enough frequency
      resolution to track low bass notes (down to ~30 Hz).
    - Spectral peaks are found each frame; every peak and the bins around it
      (its "region") are moved as one block to the new frequency and rotated
      by a single phase, so each harmonic keeps its shape and stays coherent.
      This avoids the "phasey" sound of naive bin-by-bin shifting.
    - Peaks, regions and phase rotations are computed once from the mid
      (L+R) spectrum and applied identically to every channel. Processing the
      channels independently lets them drift apart, which you hear as the
      sound wandering between left and right.

    Latency is fftSize - hopSize samples. The processor reports it to the host
    and delays the dry signal by the same amount so the Mix knob stays phase
    aligned (important for bass, otherwise you get comb filtering).
*/
class PitchShifter
{
public:
    static constexpr int maxChannels = 2;
    static constexpr int overlap = 8;

    /** Pick an FFT size that keeps roughly the same time/frequency resolution
        regardless of sample rate. */
    static int fftOrderForSampleRate (double sampleRate)
    {
        if (sampleRate <= 50000.0)  return 12; // 4096
        if (sampleRate <= 100000.0) return 13; // 8192
        return 14;                             // 16384
    }

    void prepare (double sampleRate)
    {
        const int order = fftOrderForSampleRate (sampleRate);
        fft = std::make_unique<juce::dsp::FFT> (order);

        fftSize  = 1 << order;
        hopSize  = fftSize / overlap;
        numBins  = fftSize / 2 + 1;
        latency  = fftSize - hopSize;
        expectedPhaseAdvance = juce::MathConstants<double>::twoPi * (double) hopSize / (double) fftSize;

        // Periodic Hann window (COLA-friendly), applied on analysis and synthesis.
        window.resize ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);

        // Normalise so that the overlapped window^2 sums to 1.
        double sumSq = 0.0;
        for (auto w : window)
            sumSq += (double) w * (double) w;
        outputGain = (float) ((double) fftSize / (sumSq * (double) overlap));

        for (auto& c : channels)
        {
            c.inFifo.assign ((size_t) fftSize, 0.0f);
            c.outFifo.assign ((size_t) fftSize, 0.0f);
            c.outAccum.assign ((size_t) (2 * fftSize), 0.0f);
            c.spectrum.assign ((size_t) numBins, {});
            c.output.assign ((size_t) numBins, {});
        }

        fftBuffer.assign ((size_t) (2 * fftSize), 0.0f);
        magnitude.assign ((size_t) numBins, 0.0f);
        phase.assign ((size_t) numBins, 0.0);
        lastPhase.assign ((size_t) numBins, 0.0);
        rotation.assign ((size_t) numBins, 0.0);
        prevRotation.assign ((size_t) numBins, 0.0);
        peaks.reserve ((size_t) numBins);

        reset();
    }

    void reset()
    {
        for (auto& c : channels)
        {
            std::fill (c.inFifo.begin(), c.inFifo.end(), 0.0f);
            std::fill (c.outFifo.begin(), c.outFifo.end(), 0.0f);
            std::fill (c.outAccum.begin(), c.outAccum.end(), 0.0f);
        }
        std::fill (lastPhase.begin(), lastPhase.end(), 0.0);
        std::fill (rotation.begin(), rotation.end(), 0.0);
        std::fill (prevRotation.begin(), prevRotation.end(), 0.0);
        rover = latency;
    }

    /** Pitch ratio, e.g. 0.5 = one octave down. Read once per FFT frame. */
    void setRatio (float newRatio) noexcept { ratio = newRatio; }

    int getLatencySamples() const noexcept { return latency; }

    /** Processes up to maxChannels channels in place, all shifted together. */
    void process (float* const* samples, int numChannelsToProcess, int numSamples) noexcept
    {
        numActive = juce::jlimit (1, maxChannels, numChannelsToProcess);

        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < numActive; ++ch)
            {
                auto& c = channels[ch];
                c.inFifo[(size_t) rover] = samples[ch][i];
                samples[ch][i] = c.outFifo[(size_t) (rover - latency)];
            }

            if (++rover >= fftSize)
            {
                rover = latency;
                processFrame();
            }
        }
    }

    /** Mono convenience overload. */
    void process (float* samples, int numSamples) noexcept
    {
        float* chans[] = { samples };
        process (chans, 1, numSamples);
    }

private:
    struct Channel
    {
        std::vector<float> inFifo, outFifo, outAccum;
        std::vector<std::complex<float>> spectrum, output;
    };

    static double wrapPhase (double p) noexcept
    {
        const double twoPi = juce::MathConstants<double>::twoPi;
        return p - twoPi * std::floor ((p + juce::MathConstants<double>::pi) / twoPi);
    }

    void processFrame() noexcept
    {
        const double r = (double) ratio;

        // ---- Analysis (per channel) ----------------------------------------
        for (int ch = 0; ch < numActive; ++ch)
        {
            auto& c = channels[ch];
            for (int i = 0; i < fftSize; ++i)
                fftBuffer[(size_t) i] = c.inFifo[(size_t) i] * window[(size_t) i];
            std::fill (fftBuffer.begin() + fftSize, fftBuffer.end(), 0.0f);

            fft->performRealOnlyForwardTransform (fftBuffer.data(), true);

            for (int k = 0; k < numBins; ++k)
                c.spectrum[(size_t) k] = { fftBuffer[(size_t) (2 * k)], fftBuffer[(size_t) (2 * k + 1)] };
        }

        // ---- Mid spectrum drives every decision ------------------------------
        float maxMag = 0.0f;
        for (int k = 0; k < numBins; ++k)
        {
            std::complex<float> mid = channels[0].spectrum[(size_t) k];
            for (int ch = 1; ch < numActive; ++ch)
                mid += channels[ch].spectrum[(size_t) k];

            magnitude[(size_t) k] = std::abs (mid);
            phase[(size_t) k]     = std::arg (mid);
            maxMag = juce::jmax (maxMag, magnitude[(size_t) k]);
        }

        // ---- Peak picking --------------------------------------------------
        peaks.clear();
        const float floor = maxMag * 1.0e-6f;
        for (int k = 1; k < numBins - 1; ++k)
        {
            const float m = magnitude[(size_t) k];
            if (m > floor && m > magnitude[(size_t) k - 1] && m >= magnitude[(size_t) k + 1]
                && (k < 2 || m > magnitude[(size_t) k - 2])
                && (k + 2 >= numBins || m >= magnitude[(size_t) k + 2]))
                peaks.push_back (k);
        }

        // ---- Shift each peak's region as one block ---------------------------
        for (int ch = 0; ch < numActive; ++ch)
            std::fill (channels[ch].output.begin(), channels[ch].output.end(), std::complex<float> {});
        std::fill (rotation.begin(), rotation.end(), 0.0);

        const int numPeaks = (int) peaks.size();
        int regionStart = 0;

        for (int p = 0; p < numPeaks; ++p)
        {
            const int kp = peaks[(size_t) p];

            // Region ends at the magnitude minimum between this peak and the next.
            int regionEnd = numBins; // exclusive
            if (p + 1 < numPeaks)
            {
                const int next = peaks[(size_t) p + 1];
                regionEnd = kp + 1;
                for (int k = kp + 1; k < next; ++k)
                    if (magnitude[(size_t) k] < magnitude[(size_t) regionEnd])
                        regionEnd = k;
                regionEnd = juce::jmax (regionEnd, kp + 1);
            }

            // True frequency of the peak (in bins) from the inter-frame phase advance.
            const double dev = wrapPhase (phase[(size_t) kp] - lastPhase[(size_t) kp] - (double) kp * expectedPhaseAdvance);
            const double trueBin = (double) kp + dev / expectedPhaseAdvance;
            const double deltaBins = trueBin * (r - 1.0);

            // Phase rotation continues from whatever region held this bin last frame.
            const double rot = wrapPhase (prevRotation[(size_t) kp] + deltaBins * expectedPhaseAdvance);
            const std::complex<float> rotator = std::polar (1.0f, (float) rot);
            const int shift = (int) std::lround ((double) kp * r) - kp;

            for (int k = regionStart; k < regionEnd; ++k)
            {
                rotation[(size_t) k] = rot;
                const int target = k + shift;
                if (target > 0 && target < numBins - 1)
                    for (int ch = 0; ch < numActive; ++ch)
                        channels[ch].output[(size_t) target] += channels[ch].spectrum[(size_t) k] * rotator;
            }

            regionStart = regionEnd;
        }

        std::swap (rotation, prevRotation);
        std::copy (phase.begin(), phase.end(), lastPhase.begin());

        // ---- Synthesis + overlap-add (per channel) ---------------------------
        for (int ch = 0; ch < numActive; ++ch)
        {
            auto& c = channels[ch];
            for (int k = 0; k < numBins; ++k)
            {
                fftBuffer[(size_t) (2 * k)]     = c.output[(size_t) k].real();
                fftBuffer[(size_t) (2 * k + 1)] = c.output[(size_t) k].imag();
            }
            std::fill (fftBuffer.begin() + 2 * numBins, fftBuffer.end(), 0.0f);

            fft->performRealOnlyInverseTransform (fftBuffer.data());

            for (int i = 0; i < fftSize; ++i)
                c.outAccum[(size_t) i] += window[(size_t) i] * fftBuffer[(size_t) i] * outputGain;

            std::copy (c.outAccum.begin(), c.outAccum.begin() + hopSize, c.outFifo.begin());
            std::copy (c.outAccum.begin() + hopSize, c.outAccum.begin() + hopSize + fftSize, c.outAccum.begin());
            std::copy (c.inFifo.begin() + hopSize, c.inFifo.end(), c.inFifo.begin());
        }
    }

    std::unique_ptr<juce::dsp::FFT> fft;

    int fftSize = 0, hopSize = 0, numBins = 0, latency = 0, rover = 0, numActive = 1;
    double expectedPhaseAdvance = 0.0;
    float outputGain = 1.0f;
    float ratio = 1.0f;

    Channel channels[maxChannels];
    std::vector<float> window, fftBuffer, magnitude;
    std::vector<double> phase, lastPhase, rotation, prevRotation;
    std::vector<int> peaks;
};
