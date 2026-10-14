#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <vector>

/**
    Low-latency, stereo-linked pitch shifter for live playing.

    A read head moves through a short delay line at `ratio` times the input
    speed, so its delay slowly grows (shifting down) or shrinks (shifting up).
    When it runs out of room it is replaced by a new head that sits exactly
    one pitch period away, with a short crossfade. Because the jump is a whole
    period of the note being played, the two heads are in phase and the
    splice is close to inaudible on a single bass note.

    The period comes from a YIN pitch detector running on a low-passed,
    decimated copy of the input, and each splice is fine-tuned with a small
    cross-correlation search at full rate.

    The delay stays between a few samples and one period of the note, so on
    average it's about half a period: ~12 ms on low E (41 Hz), ~6 ms on A2.
    It's designed for single notes; chords will warble.

    Pitch detection and every splice decision use the mid (L+R) signal, and
    all channels share the same read heads. If each channel spliced on its
    own, the splices would land at different moments on each side and the
    sound would appear to wander between left and right.
*/
class LiveShifter
{
public:
    static constexpr int maxChannels = 2;

    void prepare (double sampleRate)
    {
        sr = sampleRate;

        maxPeriod = (int) std::ceil (sr / minFreq);
        fadeMax   = (int) (0.005 * sr);

        int size = 1;
        while (size < 3 * maxPeriod + fadeMax + 64)
            size <<= 1;
        for (auto& b : buffers)
            b.assign ((size_t) size, 0.0f);
        midBuffer.assign ((size_t) size, 0.0f);
        mask = size - 1;

        // Pitch detection runs at ~11 kHz on a band-limited copy of the input.
        decimation = juce::jmax (1, (int) std::round (sr / 11025.0));
        decRate    = sr / (double) decimation;
        maxLag     = (int) std::ceil (decRate / minFreq);
        minLag     = juce::jmax (2, (int) std::floor (decRate / maxFreq));
        yinWindow  = maxLag / 2;
        detectHop  = juce::jmax (1, (int) (0.008 * decRate));

        int decSize = 1;
        while (decSize < maxLag + yinWindow + 8)
            decSize <<= 1;
        decBuffer.assign ((size_t) decSize, 0.0f);
        decMask = decSize - 1;
        yinDiff.assign ((size_t) maxLag + 2, 0.0f);

        const auto coeffs = juce::dsp::IIR::Coefficients<float>::makeLowPass (sr, 900.0);
        for (auto& f : lowpass)
            f.coefficients = coeffs;

        reset();
    }

    void reset()
    {
        for (auto& b : buffers)
            std::fill (b.begin(), b.end(), 0.0f);
        std::fill (midBuffer.begin(), midBuffer.end(), 0.0f);
        std::fill (decBuffer.begin(), decBuffer.end(), 0.0f);
        for (auto& f : lowpass)
            f.reset();

        writePos = 0;
        decWritePos = 0;
        decCounter = 0;
        detectCounter = 0;

        period = 0.010 * sr; // until a note is detected
        mainDelay = minDelay;
        fadeDelay = 0.0;
        fadePos = fadeLen = 0;
    }

    void setRatio (float newRatio) noexcept { ratio = (double) newRatio; }

    /** Current delay of the main read head, in samples (for tests / metering). */
    double getCurrentDelay() const noexcept { return mainDelay; }
    double getDetectedPeriod() const noexcept { return period; }

    /** Processes up to maxChannels channels in place, all shifted together. */
    void process (float* const* samples, int numChannelsToProcess, int numSamples) noexcept
    {
        const int numCh = juce::jlimit (1, maxChannels, numChannelsToProcess);

        for (int i = 0; i < numSamples; ++i)
        {
            float mid = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
            {
                buffers[ch][(size_t) writePos] = samples[ch][i];
                mid += samples[ch][i];
            }
            mid /= (float) numCh;
            midBuffer[(size_t) writePos] = mid;

            advanceHeads (mid);

            for (int ch = 0; ch < numCh; ++ch)
            {
                const auto& buf = buffers[ch];
                float out = read (buf, mainDelay);
                if (fadeLen > 0)
                    out = fadeGain * out + (1.0f - fadeGain) * read (buf, fadeDelay);
                samples[ch][i] = out;
            }

            moveHeads();
        }
    }

    /** Mono convenience overload. */
    void process (float* samples, int numSamples) noexcept
    {
        float* chans[] = { samples };
        process (chans, 1, numSamples);
    }

private:
    static constexpr double minFreq = 25.0;   // below low B (30.9 Hz)
    static constexpr double maxFreq = 700.0;  // well above the top of a 24-fret bass
    static constexpr double minDelay = 3.0;   // room for 4-point interpolation

    /** Runs pitch tracking on the mid signal and decides whether to splice. */
    void advanceHeads (float mid) noexcept
    {
        trackPitch (mid);

        if (fadeLen == 0)
        {
            const double guard = (double) searchRadius() + 1.0;

            if (ratio < 1.0 && mainDelay >= minDelay + period + guard)
                startSplice (-period);
            else if (ratio > 1.0 && mainDelay <= minDelay + (double) fadeLength() * (ratio - 1.0) + guard)
                startSplice (period);
            else if (mainDelay > (double) (2 * maxPeriod))
                startSplice (-period); // safety net if the period estimate jumped
        }

        // Raised-cosine crossfade from the old head (fadeDelay) to the new one (mainDelay).
        if (fadeLen > 0)
            fadeGain = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) fadePos / (float) fadeLen);
    }

    void moveHeads() noexcept
    {
        const double advance = 1.0 - ratio; // how much each head's delay changes per sample

        if (fadeLen > 0)
        {
            fadeDelay = juce::jmax (minDelay, fadeDelay + advance);
            if (++fadePos >= fadeLen)
                fadeLen = 0;
        }

        mainDelay += advance;
        mainDelay = juce::jlimit (minDelay, (double) (midBuffer.size() - 4), mainDelay);

        writePos = (writePos + 1) & mask;
    }

    int fadeLength() const noexcept
    {
        return juce::jlimit (32, fadeMax, (int) (period * 0.5));
    }

    int searchRadius() const noexcept { return decimation + 2; }

    /** Hand over to a new head about `jump` samples away, nudged to the best-matching spot. */
    void startSplice (double jump) noexcept
    {
        const int radius = searchRadius();
        const int len = juce::jlimit (32, 256, (int) period);
        const int centre = (int) std::lround (jump);
        const int a = (int) mainDelay;

        // Similarity between the waveform just before the current head and just
        // before a candidate head `offset` samples away (1 = identical).
        auto similarity = [&] (int offset)
        {
            const int b = a + offset;
            if ((double) b < minDelay || b + len + 4 >= (int) midBuffer.size())
                return -1.0;

            double xy = 0.0, energy = 0.0;
            for (int j = 0; j < len; ++j)
            {
                const double s1 = midBuffer[(size_t) ((writePos - a - j) & mask)];
                const double s2 = midBuffer[(size_t) ((writePos - b - j) & mask)];
                xy += s1 * s2;
                energy += s1 * s1 + s2 * s2;
            }
            return energy > 1.0e-12 ? 2.0 * xy / energy : 0.0;
        };

        int bestOffset = centre;
        double bestScore = -2.0;
        for (int off = centre - radius; off <= centre + radius; ++off)
        {
            const double score = similarity (off);
            if (score > bestScore)
            {
                bestScore = score;
                bestOffset = off;
            }
        }

        // Parabolic refinement for a sub-sample jump.
        double jumpExact = (double) bestOffset;
        const double l = similarity (bestOffset - 1), r = similarity (bestOffset + 1);
        const double denom = l - 2.0 * bestScore + r;
        if (l > -1.0 && r > -1.0 && denom < -1.0e-12)
            jumpExact += juce::jlimit (-0.5, 0.5, 0.5 * (l - r) / denom);

        fadeDelay = mainDelay;
        mainDelay = juce::jmax (minDelay, mainDelay + jumpExact);
        fadeLen = fadeLength();
        fadePos = 0;
    }

    /** 4-point Hermite read, `delay` samples behind the write position. */
    float read (const std::vector<float>& buffer, double delay) const noexcept
    {
        const int whole = (int) delay;
        const float frac = (float) (delay - (double) whole);
        const int idx = writePos - whole;

        const float xm1 = buffer[(size_t) ((idx + 1) & mask)];
        const float x0  = buffer[(size_t) ((idx)     & mask)];
        const float x1  = buffer[(size_t) ((idx - 1) & mask)];
        const float x2  = buffer[(size_t) ((idx - 2) & mask)];

        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * frac + c2) * frac + c1) * frac + x0;
    }

    //==========================================================================
    void trackPitch (float x) noexcept
    {
        float y = x;
        for (auto& f : lowpass)
            y = f.processSample (y);

        if (++decCounter < decimation)
            return;
        decCounter = 0;

        decBuffer[(size_t) decWritePos] = y;
        decWritePos = (decWritePos + 1) & decMask;

        if (++detectCounter >= detectHop)
        {
            detectCounter = 0;
            detectPeriod();
        }
    }

    /** YIN (de Cheveigné & Kawahara, 2002) on the decimated signal. */
    void detectPeriod() noexcept
    {
        const int start = decWritePos - (maxLag + yinWindow); // oldest sample used

        float energy = 0.0f;
        for (int j = 0; j < yinWindow; ++j)
        {
            const float s = decBuffer[(size_t) ((start + j) & decMask)];
            energy += s * s;
        }
        if (energy / (float) yinWindow < 1.0e-7f) // silence: keep the last period
            return;

        float runningSum = 0.0f;
        yinDiff[0] = 1.0f;
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            float d = 0.0f;
            for (int j = 0; j < yinWindow; ++j)
            {
                const float diff = decBuffer[(size_t) ((start + j) & decMask)]
                                 - decBuffer[(size_t) ((start + j + tau) & decMask)];
                d += diff * diff;
            }
            runningSum += d;
            yinDiff[(size_t) tau] = runningSum > 0.0f ? d * (float) tau / runningSum : 1.0f;
        }

        int tau = -1;
        for (int t = minLag; t < maxLag; ++t)
        {
            if (yinDiff[(size_t) t] < 0.15f)
            {
                while (t + 1 < maxLag && yinDiff[(size_t) t + 1] < yinDiff[(size_t) t])
                    ++t;
                tau = t;
                break;
            }
        }
        if (tau < 0)
            return; // no clear pitch: keep the last period

        // Parabolic interpolation for a sub-sample estimate.
        double refined = (double) tau;
        if (tau > 1 && tau < maxLag)
        {
            const double a = yinDiff[(size_t) tau - 1], b = yinDiff[(size_t) tau], c = yinDiff[(size_t) tau + 1];
            const double denom = a - 2.0 * b + c;
            if (std::abs (denom) > 1.0e-9)
                refined += 0.5 * (a - c) / denom;
        }

        period = juce::jlimit (sr / maxFreq, (double) maxPeriod, refined * (double) decimation);
    }

    double sr = 44100.0, ratio = 1.0;

    std::vector<float> buffers[maxChannels], midBuffer;
    int mask = 0, writePos = 0, maxPeriod = 0, fadeMax = 0;

    double period = 441.0, mainDelay = minDelay, fadeDelay = 0.0;
    int fadePos = 0, fadeLen = 0;
    float fadeGain = 1.0f;

    juce::dsp::IIR::Filter<float> lowpass[2];
    std::vector<float> decBuffer, yinDiff;
    double decRate = 11025.0;
    int decimation = 4, decMask = 0, decWritePos = 0, decCounter = 0;
    int maxLag = 0, minLag = 0, yinWindow = 0, detectHop = 0, detectCounter = 0;
};
