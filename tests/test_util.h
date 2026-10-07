// Shared helpers for the Gitto FX offline DSP tests.
#pragma once

#include "../dsp/Common.h"
#include <vector>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>

using namespace gitto;

inline int failures = 0, checks = 0;
inline void check (bool ok, const std::string& name, const std::string& detail = "")
{
    ++checks;
    if (! ok)
        ++failures;
    std::printf ("  [%s] %s %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
}
inline std::string fmt (const char* f, double a = 0, double b = 0, double c = 0)
{
    char buf[256];
    std::snprintf (buf, sizeof (buf), f, a, b, c);
    return buf;
}

using Buf = std::vector<float>;
inline const double SR = 48000.0;

inline Buf sine (double hz, double amp, double seconds, double sr = SR, double phase = 0.0)
{
    Buf b ((size_t) (seconds * sr));
    for (size_t i = 0; i < b.size(); ++i)
        b[i] = (float) (amp * std::sin (kTwoPi * hz * (double) i / sr + phase));
    return b;
}
inline Buf noise (double amp, double seconds, uint32_t seed = 1234, double sr = SR)
{
    Random r (seed);
    Buf b ((size_t) (seconds * sr));
    for (auto& v : b)
        v = (float) amp * r.nextBipolar();
    return b;
}
// Decaying noise bursts every 250 ms: a rough stand-in for drums.
inline Buf drums (double amp, double seconds, double sr = SR)
{
    Random r (99);
    Buf b ((size_t) (seconds * sr));
    const int period = (int) (0.25 * sr);
    for (size_t i = 0; i < b.size(); ++i)
    {
        const double t = (double) (i % (size_t) period) / sr;
        const double env = std::exp (-t * 30.0);
        const double tone = std::sin (kTwoPi * 60.0 * t * (1.0 + 2.0 * std::exp (-t * 40.0)));
        b[i] = (float) (amp * env * (0.7 * tone + 0.5 * r.nextBipolar()));
    }
    return b;
}
inline double rms (const Buf& b, size_t from = 0, size_t to = (size_t) -1)
{
    to = std::min (to, b.size());
    double s = 0;
    for (size_t i = from; i < to; ++i)
        s += (double) b[i] * b[i];
    return std::sqrt (s / std::max<size_t> (1, to - from));
}
inline double peakOf (const Buf& b, size_t from = 0, size_t to = (size_t) -1)
{
    to = std::min (to, b.size());
    double p = 0;
    for (size_t i = from; i < to; ++i)
        p = std::max (p, (double) std::abs (b[i]));
    return p;
}
inline bool allFinite (const Buf& b)
{
    for (float v : b)
        if (! std::isfinite (v))
            return false;
    return true;
}
inline double db (double x) { return 20.0 * std::log10 (std::max (x, 1e-12)); }

// Independent true-peak estimate: 16x windowed-sinc interpolation, 48 taps per side.
inline double truePeak (const Buf& b)
{
    const int half = 48, os = 16;
    double p = 0;
    std::vector<std::vector<double>> kern ((size_t) os, std::vector<double> ((size_t) (2 * half)));
    for (int ph = 0; ph < os; ++ph)
        for (int k = 0; k < 2 * half; ++k)
        {
            const double t = (double) (k - half + 1) - (double) ph / os; // offset from the interpolation point
            const double x = (t + half) / (2.0 * half);
            const double w = (x <= 0 || x >= 1) ? 0.0 : 0.42 - 0.5 * std::cos (kTwoPi * x) + 0.08 * std::cos (2 * kTwoPi * x);
            const double s = std::abs (t) < 1e-9 ? 1.0 : std::sin (kPi * t) / (kPi * t);
            kern[(size_t) ph][(size_t) k] = s * w;
        }
    for (size_t i = (size_t) half; i + (size_t) half < b.size(); ++i)
        for (int ph = 0; ph < os; ++ph)
        {
            double acc = 0;
            for (int k = 0; k < 2 * half; ++k)
                acc += kern[(size_t) ph][(size_t) k] * b[i + (size_t) k - (size_t) half + 1];
            p = std::max (p, std::abs (acc));
        }
    return p;
}

// Amplitude of a single frequency component (Goertzel-style correlation).
inline double toneAmp (const Buf& b, double hz, size_t from, size_t to, double sr = SR)
{
    double re = 0, im = 0;
    for (size_t i = from; i < to; ++i)
    {
        const double ph = kTwoPi * hz * (double) i / sr;
        re += b[i] * std::cos (ph);
        im += b[i] * std::sin (ph);
    }
    return 2.0 * std::sqrt (re * re + im * im) / (double) (to - from);
}

template <typename Fn>
inline void processBlocks (Buf& l, Buf& r, Fn&& fn, int block = 256)
{
    for (size_t pos = 0; pos < l.size(); pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, l.size() - pos);
        fn (l.data() + pos, r.data() + pos, n);
    }
}

inline double benchmark (const std::function<void (float*, float*, int)>& fn, double seconds = 10.0)
{
    Buf l = noise (0.3, seconds, 1), r = noise (0.3, seconds, 2);
    const auto t0 = std::chrono::steady_clock::now();
    processBlocks (l, r, fn, 512);
    const double dt = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    return dt / seconds * 100.0; // % of one core at SR
}


inline double measureRt60 (const Buf& ir, double sr)
{
    // Schroeder backward integration, slope fitted between -5 and -35 dB (T30).
    std::vector<double> e (ir.size());
    double acc = 0;
    for (size_t i = ir.size(); i-- > 0;)
    {
        acc += (double) ir[i] * ir[i];
        e[i] = acc;
    }
    const double total = e[0];
    double t5 = -1, t35 = -1;
    for (size_t i = 0; i < e.size(); ++i)
    {
        const double d = 10.0 * std::log10 (e[i] / total + 1e-30);
        if (t5 < 0 && d <= -5) t5 = (double) i / sr;
        if (t35 < 0 && d <= -35) { t35 = (double) i / sr; break; }
    }
    return (t5 >= 0 && t35 >= 0) ? 2.0 * (t35 - t5) : -1.0;
}


inline std::vector<std::pair<size_t, double>> findEchoes (const Buf& b, double threshold, size_t minGap)
{
    std::vector<std::pair<size_t, double>> out;
    size_t i = 0;
    while (i < b.size())
    {
        if (std::abs (b[i]) > threshold)
        {
            size_t best = i;
            for (size_t k = i; k < std::min (b.size(), i + minGap); ++k)
                if (std::abs (b[k]) > std::abs (b[best]))
                    best = k;
            out.push_back ({ best, (double) std::abs (b[best]) });
            i += minGap;
        }
        else
            ++i;
    }
    return out;
}

