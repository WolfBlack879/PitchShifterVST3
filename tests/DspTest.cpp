// Offline check of the pitch shifter: feed a bass sine, measure the output pitch and level.
#include "PitchShifter.h"
#include <cstdio>

static double estimateFrequency (const std::vector<float>& x, double sampleRate, int start)
{
    // Zero-crossing estimate with linear interpolation over the steady-state part.
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (size_t i = (size_t) start + 1; i < x.size(); ++i)
    {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = (double) (i - 1) + (double) (-x[i - 1]) / (double) (x[i] - x[i - 1]);
            if (first < 0.0) first = t; else ++crossings;
            last = t;
        }
    }
    return crossings > 0 ? sampleRate * crossings / (last - first) : 0.0;
}

static double rms (const std::vector<float>& x, int start)
{
    double s = 0.0;
    for (size_t i = (size_t) start; i < x.size(); ++i) s += (double) x[i] * x[i];
    return std::sqrt (s / (double) (x.size() - (size_t) start));
}

int main()
{
    struct Case { double sr, inHz; float semis; };
    const Case cases[] = {
        { 48000.0, 41.2,   0.0f },   // low E, unchanged
        { 48000.0, 82.4, -12.0f },   // E2 -> E1
        { 48000.0, 55.0,  12.0f },   // A1 -> A2
        { 44100.0, 98.0,  -5.0f },
        { 96000.0, 41.2,  -2.0f },   // drop-D style on low E
        { 48000.0, 61.7, -24.0f },   // two octaves down
        { 48000.0, 41.2,   7.0f },   // fifth up
        { 44100.0, 73.4,   3.0f },
    };

    int failures = 0;
    for (const auto& c : cases)
    {
        PitchShifter ps;
        ps.prepare (c.sr);
        const float ratio = std::pow (2.0f, c.semis / 12.0f);
        ps.setRatio (ratio);

        const int n = (int) (c.sr * 3.0);
        std::vector<float> buf ((size_t) n);
        for (int i = 0; i < n; ++i)
            buf[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * c.inHz * i / c.sr);

        // Process in odd-sized blocks like a host would.
        for (int pos = 0; pos < n; pos += 333)
            ps.process (buf.data() + pos, std::min (333, n - pos));

        const int settle = ps.getLatencySamples() + (int) c.sr / 2;
        const double expected = c.inHz * ratio;
        const double got = estimateFrequency (buf, c.sr, settle);
        const double centsErr = 1200.0 * std::log2 (got / expected);
        const double level = rms (buf, settle) / (0.5 / std::sqrt (2.0));
        const bool ok = std::abs (centsErr) < 5.0 && level > 0.8 && level < 1.25;
        failures += ok ? 0 : 1;

        std::printf ("%s sr=%6.0f in=%6.1f Hz shift=%+5.1f st  expected=%7.2f Hz got=%7.2f Hz (%+.2f ct) level=%.3f latency=%d\n",
                     ok ? "PASS" : "FAIL", c.sr, c.inHz, c.semis, expected, got, centsErr, level, ps.getLatencySamples());
    }

    return failures == 0 ? 0 : 1;
}
