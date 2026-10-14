#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <complex>
#include <vector>

/**
    Mono phase-vocoder pitch shifter with peak (identity) phase locking,
    after Laroche & Dolson, "New phase-vocoder techniques for pitch-shifting,
    harmonizing and other exotic effects" (WASPAA 1999).

    - A long FFT (4096 at 44.1/48 kHz) with 8x overlap gives enough frequency
      resolution to track low bass notes (down to ~30 Hz).
    - Spectral peaks are found each frame; every peak and the bins around it
      (its "region") are moved as one block to the new frequency and rotated
      by a single phase, so each harmonic keeps its shape and stays coherent.
      This avoids the "phasey" sound of naive bin-by-bin shifting.

    Latency is fftSize - hopSize samples. The processor reports it to the host
    and delays the dry signal by the same amount so the Mix knob stays phase
    aligned (important for bass, otherwise you get comb filtering).
*/
class PitchShifter
{
public:
    /** Pick an FFT size that keeps roughly the same time/frequency resolution
        regardless of sample rate. */
    static int fftOrderForSampleRate (double sampleRate)
    {
        if (sampleRate <= 50000.0)  return 12; // 4096
        if (sampleRate <= 100000.0) return 13; // 8192
        return 14;                             // 16384
    }

    static constexpr int overlap = 8;

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

        inFifo.assign ((size_t) fftSize, 0.0f);
        outFifo.assign ((size_t) fftSize, 0.0f);
        outAccum.assign ((size_t) (2 * fftSize), 0.0f);
        fftBuffer.assign ((size_t) (2 * fftSize), 0.0f);

        spectrum.assign ((size_t) numBins, {});
        output.assign ((size_t) numBins, {});
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
        std::fill (inFifo.begin(), inFifo.end(), 0.0f);
        std::fill (outFifo.begin(), outFifo.end(), 0.0f);
        std::fill (outAccum.begin(), outAccum.end(), 0.0f);
        std::fill (lastPhase.begin(), lastPhase.end(), 0.0);
        std::fill (rotation.begin(), rotation.end(), 0.0);
        std::fill (prevRotation.begin(), prevRotation.end(), 0.0);
        rover = latency;
    }

    /** Pitch ratio, e.g. 0.5 = one octave down. Read once per FFT frame. */
    void setRatio (float newRatio) noexcept { ratio = newRatio; }

    int getLatencySamples() const noexcept { return latency; }

    void process (float* samples, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            inFifo[(size_t) rover] = samples[i];
            samples[i] = outFifo[(size_t) (rover - latency)];

            if (++rover >= fftSize)
            {
                rover = latency;
                processFrame();
            }
        }
    }

private:
    static double wrapPhase (double p) noexcept
    {
        const double twoPi = juce::MathConstants<double>::twoPi;
        return p - twoPi * std::floor ((p + juce::MathConstants<double>::pi) / twoPi);
    }

    void processFrame() noexcept
    {
        const double r = (double) ratio;

        // ---- Analysis ------------------------------------------------------
        for (int i = 0; i < fftSize; ++i)
            fftBuffer[(size_t) i] = inFifo[(size_t) i] * window[(size_t) i];
        std::fill (fftBuffer.begin() + fftSize, fftBuffer.end(), 0.0f);

        fft->performRealOnlyForwardTransform (fftBuffer.data(), true);

        float maxMag = 0.0f;
        for (int k = 0; k < numBins; ++k)
        {
            const std::complex<float> c { fftBuffer[(size_t) (2 * k)], fftBuffer[(size_t) (2 * k + 1)] };
            spectrum[(size_t) k]  = c;
            magnitude[(size_t) k] = std::abs (c);
            phase[(size_t) k]     = std::arg (c);
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
        std::fill (output.begin(), output.end(), std::complex<float> {});
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
                    output[(size_t) target] += spectrum[(size_t) k] * rotator;
            }

            regionStart = regionEnd;
        }

        std::swap (rotation, prevRotation);
        std::copy (phase.begin(), phase.end(), lastPhase.begin());

        // ---- Synthesis -----------------------------------------------------
        for (int k = 0; k < numBins; ++k)
        {
            fftBuffer[(size_t) (2 * k)]     = output[(size_t) k].real();
            fftBuffer[(size_t) (2 * k + 1)] = output[(size_t) k].imag();
        }
        std::fill (fftBuffer.begin() + 2 * numBins, fftBuffer.end(), 0.0f);

        fft->performRealOnlyInverseTransform (fftBuffer.data());

        // ---- Overlap-add ---------------------------------------------------
        for (int i = 0; i < fftSize; ++i)
            outAccum[(size_t) i] += window[(size_t) i] * fftBuffer[(size_t) i] * outputGain;

        std::copy (outAccum.begin(), outAccum.begin() + hopSize, outFifo.begin());
        std::copy (outAccum.begin() + hopSize, outAccum.begin() + hopSize + fftSize, outAccum.begin());
        std::copy (inFifo.begin() + hopSize, inFifo.end(), inFifo.begin());
    }

    std::unique_ptr<juce::dsp::FFT> fft;

    int fftSize = 0, hopSize = 0, numBins = 0, latency = 0, rover = 0;
    double expectedPhaseAdvance = 0.0;
    float outputGain = 1.0f;
    float ratio = 1.0f;

    std::vector<float> window, inFifo, outFifo, outAccum, fftBuffer, magnitude;
    std::vector<std::complex<float>> spectrum, output;
    std::vector<double> phase, lastPhase, rotation, prevRotation;
    std::vector<int> peaks;
};
