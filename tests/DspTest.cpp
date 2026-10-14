// Offline checks of both pitch-shift engines: feed bass signals, measure the output
// pitch, level and distortion, plus delay for the live engine.
#include "PitchShifter.h"
#include "LiveShifter.h"
#include <cstdio>
#include <functional>

using Signal = std::vector<float>;
constexpr double pi = juce::MathConstants<double>::pi;

/** Period estimate (in samples) using the YIN difference function. */
static double estimatePeriod (const Signal& x, int start, int end, double sampleRate)
{
    const int maxLag = (int) (sampleRate / 12.0), window = (int) (sampleRate * 0.1);
    if (end - start < maxLag + window)
        return 0.0;

    std::vector<double> d ((size_t) maxLag + 1, 0.0);
    double running = 0.0;
    for (int tau = 1; tau <= maxLag; ++tau)
    {
        double s = 0.0;
        for (int j = 0; j < window; ++j)
        {
            const double diff = (double) x[(size_t) (start + j)] - (double) x[(size_t) (start + j + tau)];
            s += diff * diff;
        }
        running += s;
        d[(size_t) tau] = s * tau / running;
    }

    for (int tau = 2; tau < maxLag; ++tau)
    {
        if (d[(size_t) tau] < 0.1)
        {
            while (tau + 1 < maxLag && d[(size_t) tau + 1] < d[(size_t) tau])
                ++tau;
            const double a = d[(size_t) tau - 1], b = d[(size_t) tau], c = d[(size_t) tau + 1];
            return tau + 0.5 * (a - c) / (a - 2.0 * b + c);
        }
    }
    return 0.0;
}

static double rms (const Signal& x, int start, int end)
{
    double s = 0.0;
    for (int i = start; i < end; ++i) s += (double) x[(size_t) i] * x[(size_t) i];
    return std::sqrt (s / (double) (end - start));
}

/** Residual after a least-squares fit of a sine at `freq`, in dB below the signal (lower = cleaner). */
static double residualDb (const Signal& x, int start, int end, double freq, double sampleRate)
{
    double cc = 0, ss = 0, cs = 0, xc = 0, xs = 0;
    for (int i = start; i < end; ++i)
    {
        const double w = 2.0 * pi * freq * i / sampleRate, c = std::cos (w), s = std::sin (w);
        cc += c * c; ss += s * s; cs += c * s; xc += x[(size_t) i] * c; xs += x[(size_t) i] * s;
    }
    const double det = cc * ss - cs * cs;
    const double a = (xc * ss - xs * cs) / det, b = (xs * cc - xc * cs) / det;

    double err = 0.0, total = 0.0;
    for (int i = start; i < end; ++i)
    {
        const double w = 2.0 * pi * freq * i / sampleRate;
        const double e = x[(size_t) i] - (a * std::cos (w) + b * std::sin (w));
        err += e * e; total += (double) x[(size_t) i] * x[(size_t) i];
    }
    return 10.0 * std::log10 (err / total + 1.0e-20);
}

struct Engine
{
    const char* name;
    std::function<void (double)> prepare;
    std::function<void (float)> setRatio;
    std::function<void (float*, int)> process;
    std::function<double()> delay; // current delay in samples
};

static int failures = 0;

static void check (bool ok, const char* fmt, double a, double b, double c, double d, double e, double f)
{
    failures += ok ? 0 : 1;
    std::printf ("%s ", ok ? "PASS" : "FAIL");
    std::printf (fmt, a, b, c, d, e, f);
    std::printf ("\n");
}

static void runSineCases (Engine& eng, double maxResidualDb)
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
        { 48000.0, 30.9, -12.0f },   // low B, octave down
    };

    std::printf ("\n== %s engine: sine tests ==\n", eng.name);

    for (const auto& c : cases)
    {
        eng.prepare (c.sr);
        const float ratio = std::pow (2.0f, c.semis / 12.0f);
        eng.setRatio (ratio);

        const int n = (int) (c.sr * 3.0);
        Signal buf ((size_t) n);
        for (int i = 0; i < n; ++i)
            buf[(size_t) i] = 0.5f * (float) std::sin (2.0 * pi * c.inHz * i / c.sr);

        // Process in odd-sized blocks like a host would; track the delay as we go.
        const int settle = (int) c.sr;
        double delaySum = 0.0, delayMax = 0.0;
        int delayCount = 0;
        for (int pos = 0; pos < n; pos += 61)
        {
            eng.process (buf.data() + pos, std::min (61, n - pos));
            if (pos >= settle)
            {
                const double d = eng.delay();
                delaySum += d; delayMax = std::max (delayMax, d); ++delayCount;
            }
        }

        const double expected = c.inHz * ratio;
        const double got = c.sr / estimatePeriod (buf, settle, n, c.sr);
        const double centsErr = 1200.0 * std::log2 (got / expected);
        const double level = rms (buf, settle, n) / (0.5 / std::sqrt (2.0));
        const double resid = residualDb (buf, settle, n, expected, c.sr);
        const double avgMs = 1000.0 * delaySum / delayCount / c.sr;

        const bool ok = std::abs (centsErr) < 5.0 && level > 0.8 && level < 1.25 && resid < maxResidualDb;
        check (ok, "in=%5.1f Hz shift=%+4.0f st -> %6.2f Hz (%+.2f ct) level=%.3f residual=%6.1f dB",
               c.inHz, c.semis, got, centsErr, level, resid);
        if (delayCount > 0 && delayMax > 0.0)
            std::printf ("       delay avg %.1f ms, max %.1f ms (sr=%.0f)\n", avgMs, 1000.0 * delayMax / c.sr, c.sr);
    }
}

/** Harmonic-rich bass tone that changes note halfway, to check the pitch tracking follows. */
static void runNoteChange (Engine& eng)
{
    std::printf ("\n== %s engine: harmonic tone, note change E1 -> A1, -12 st ==\n", eng.name);

    const double sr = 48000.0;
    const double notes[] = { 41.2, 55.0 };
    const int noteLen = (int) (sr * 1.5);

    eng.prepare (sr);
    eng.setRatio (0.5f);

    Signal buf ((size_t) (2 * noteLen));
    double phase = 0.0;
    for (int i = 0; i < 2 * noteLen; ++i)
    {
        const double f = notes[i / noteLen];
        phase += 2.0 * pi * f / sr;
        double s = 0.0;
        for (int h = 1; h <= 8; ++h)
            s += std::sin (h * phase) / h;
        buf[(size_t) i] = (float) (0.3 * s);
    }

    for (int pos = 0; pos < (int) buf.size(); pos += 128)
        eng.process (buf.data() + pos, std::min (128, (int) buf.size() - pos));

    for (int k = 0; k < 2; ++k)
    {
        const int start = k * noteLen + (int) (0.5 * sr), end = (k + 1) * noteLen;
        const double expected = notes[k] * 0.5;
        const double got = sr / estimatePeriod (buf, start, end, sr);
        const double centsErr = 1200.0 * std::log2 (got / expected);
        check (std::abs (centsErr) < 10.0, "note %.0f: %5.1f Hz -> %6.2f Hz, expected %6.2f (%+.2f ct)",
               (double) k + 1, notes[k], got, expected, centsErr, 0.0);
    }
}

int main()
{
    PitchShifter studio;
    LiveShifter live;

    Engine studioEngine { "Studio",
        [&] (double sr) { studio.prepare (sr); },
        [&] (float r) { studio.setRatio (r); },
        [&] (float* d, int n) { studio.process (d, n); },
        [&] { return (double) studio.getLatencySamples(); } };

    Engine liveEngine { "Live",
        [&] (double sr) { live.prepare (sr); },
        [&] (float r) { live.setRatio (r); },
        [&] (float* d, int n) { live.process (d, n); },
        [&] { return live.getCurrentDelay(); } };

    runSineCases (studioEngine, -30.0);
    runNoteChange (studioEngine);
    runSineCases (liveEngine, -30.0);
    runNoteChange (liveEngine);

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
