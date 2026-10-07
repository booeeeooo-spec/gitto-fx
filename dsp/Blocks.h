// Gitto FX - larger shared DSP blocks: 4x oversampling, FFT, crossovers and waveshapers.
#pragma once

#include "Common.h"

namespace gitto
{
//==============================================================================
// 4x oversampler built on one 129-tap Kaiser-windowed low-pass, used polyphase for
// interpolation and directly for decimation. Round-trip latency is exactly kLatency
// samples at the base rate, so a dry path can be lined up with it.
class Oversampler4x
{
public:
    static constexpr int kFactor = 4;
    static constexpr int kTaps = 129;
    static constexpr int kPhaseTaps = 33; // ceil(129 / 4)
    static constexpr int kLatency = 32;

    Oversampler4x()
    {
        auto bessel0 = [] (double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 40; ++k)
            {
                term *= (x * 0.5 / k) * (x * 0.5 / k);
                sum += term;
            }
            return sum;
        };
        const double beta = 9.0, centre = (kTaps - 1) * 0.5;
        const double cutoff = 0.122; // cycles per sample at the 4x rate; just under the base Nyquist
        double sum = 0.0;
        std::array<double, kTaps> h {};
        for (int i = 0; i < kTaps; ++i)
        {
            const double t = (double) i - centre;
            const double sinc = std::abs (t) < 1.0e-9 ? 2.0 * cutoff : std::sin (kTwoPi * cutoff * t) / (kPi * t);
            const double r = t / centre;
            h[(size_t) i] = sinc * bessel0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / bessel0 (beta);
            sum += h[(size_t) i];
        }
        for (int i = 0; i < kTaps; ++i)
        {
            const float c = (float) (h[(size_t) i] / sum);
            down[(size_t) i] = c;
            up[(size_t) (i % kFactor)][(size_t) (i / kFactor)] = c * (float) kFactor;
        }
        reset();
    }

    void reset() noexcept
    {
        upHistory.fill (0.0f);
        downHistory.fill (0.0f);
        upPos = downPos = 0;
    }

    // One input sample in, four samples out.
    void upsample (float x, float* out) noexcept
    {
        upHistory[(size_t) upPos] = x;
        upHistory[(size_t) (upPos + kPhaseTaps)] = x;
        const float* newestFirstBase = upHistory.data() + upPos + kPhaseTaps; // newest sample
        for (int p = 0; p < kFactor; ++p)
        {
            const auto& c = up[(size_t) p];
            float acc = 0.0f;
            for (int t = 0; t < kPhaseTaps; ++t)
                acc += c[(size_t) t] * newestFirstBase[-t];
            out[p] = acc;
        }
        upPos = (upPos + 1) % kPhaseTaps;
    }

    // Four samples in, one sample out.
    float downsample (const float* in) noexcept
    {
        // The output is taken at the first of the four samples, which is what makes the
        // round trip a whole number of base-rate samples.
        pushDown (in[0]);
        const float* oldest = downHistory.data() + downPos; // kTaps contiguous samples, oldest first
        float acc = 0.0f;
        for (int i = 0; i < kTaps; ++i)
            acc += down[(size_t) i] * oldest[i];
        pushDown (in[1]);
        pushDown (in[2]);
        pushDown (in[3]);
        return acc;
    }

private:
    void pushDown (float v) noexcept
    {
        downHistory[(size_t) downPos] = v;
        downHistory[(size_t) (downPos + kTaps)] = v;
        downPos = (downPos + 1) % kTaps;
    }

    std::array<float, kTaps> down {};
    std::array<std::array<float, kPhaseTaps>, kFactor> up {};
    std::array<float, 2 * kPhaseTaps + 1> upHistory {};
    std::array<float, 2 * kTaps> downHistory {};
    int upPos = 0, downPos = 0;
};

//==============================================================================
// Radix-2 complex FFT with precomputed tables, safe to call on the audio thread.
class Fft
{
public:
    void prepare (int size)
    {
        n = size;
        rev.assign ((size_t) n, 0);
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            rev[(size_t) i] = j;
        }
        twiddle.assign ((size_t) (n / 2), {});
        for (int k = 0; k < n / 2; ++k)
            twiddle[(size_t) k] = std::polar (1.0f, (float) (-kTwoPi * k / n));
    }

    int size() const noexcept { return n; }

    void forward (std::complex<float>* a) const noexcept { transform (a, false); }

    // Inverse, scaled so forward followed by inverse returns the input.
    void inverse (std::complex<float>* a) const noexcept
    {
        transform (a, true);
        const float s = 1.0f / (float) n;
        for (int i = 0; i < n; ++i)
            a[i] *= s;
    }

private:
    void transform (std::complex<float>* a, bool inv) const noexcept
    {
        for (int i = 0; i < n; ++i)
            if (i < rev[(size_t) i])
                std::swap (a[i], a[rev[(size_t) i]]);
        for (int len = 2; len <= n; len <<= 1)
        {
            const int step = n / len;
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < len / 2; ++k)
                {
                    auto w = twiddle[(size_t) (k * step)];
                    if (inv)
                        w = std::conj (w);
                    const auto u = a[i + k];
                    const auto v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                }
        }
    }

    int n = 0;
    std::vector<int> rev;
    std::vector<std::complex<float>> twiddle;
};

//==============================================================================
// Fourth-order Linkwitz-Riley crossover: the low and high outputs sum back to an
// all-pass version of the input, so there is no dip or bump at the crossover.
class Crossover
{
public:
    void set (double hz, double sr) noexcept
    {
        const auto l = design::lowpass (hz, 0.70710678, sr);
        const auto h = design::highpass (hz, 0.70710678, sr);
        for (auto& f : lp) f.setCoefs (l);
        for (auto& f : hp) f.setCoefs (h);
    }
    void reset() noexcept
    {
        for (auto& f : lp) f.reset();
        for (auto& f : hp) f.reset();
    }
    void split (float x, float& low, float& high) noexcept
    {
        low = lp[1].process (lp[0].process (x));
        high = hp[1].process (hp[0].process (x));
    }
    void sanitise() noexcept
    {
        for (auto& f : lp) f.sanitise();
        for (auto& f : hp) f.sanitise();
    }

private:
    std::array<BiquadD, 2> lp, hp;
};

// Splits one channel into four bands that sum back flat. Each half of the tree gets
// an all-pass matching the other half's crossover so all four bands stay in phase.
class FourBandSplitter
{
public:
    void setFrequencies (double f1, double f2, double f3, double sr) noexcept
    {
        // Keep the crossovers ordered and at least a third of an octave apart.
        f1 = clampv (f1, 30.0, sr * 0.2);
        f2 = clampv (f2, f1 * 1.26, sr * 0.3);
        f3 = clampv (f3, f2 * 1.26, sr * 0.45);
        mid.set (f2, sr);
        low.set (f1, sr);
        high.set (f3, sr);
        apLow.setCoefs (design::allpass (f1, 0.70710678, sr));
        apHigh.setCoefs (design::allpass (f3, 0.70710678, sr));
        apLow2 = apLow;
        apHigh2 = apHigh;
        freqs = { f1, f2, f3 };
    }
    void reset() noexcept
    {
        mid.reset();
        low.reset();
        high.reset();
        apLow.reset();
        apHigh.reset();
        apLow2.reset();
        apHigh2.reset();
    }
    void process (float x, float* bands) noexcept
    {
        float lowHalf, highHalf;
        mid.split (x, lowHalf, highHalf);
        low.split (lowHalf, bands[0], bands[1]);
        high.split (highHalf, bands[2], bands[3]);
        bands[0] = apHigh.process (bands[0]);
        bands[1] = apHigh2.process (bands[1]);
        bands[2] = apLow.process (bands[2]);
        bands[3] = apLow2.process (bands[3]);
    }
    void sanitise() noexcept
    {
        mid.sanitise();
        low.sanitise();
        high.sanitise();
        apLow.sanitise();
        apHigh.sanitise();
        apLow2.sanitise();
        apHigh2.sanitise();
    }
    std::array<double, 3> freqs { 120.0, 800.0, 5000.0 };

private:
    Crossover low, mid, high;
    BiquadD apLow, apHigh, apLow2, apHigh2;
};

//==============================================================================
// Waveshapers. The smooth curves have unity slope for small signals, so "drive" is
// simply gain in front of them, and every curve is bounded so nothing downstream can
// blow up. Lopsided curves add a DC offset, which the caller filters out.
enum class ShapeStyle
{
    SoftTube = 0, WarmTube, Tape, Transformer, Console, Diode,
    HardClip, Fuzz, Rectify, Foldback, SineFold, Crush, Count
};

inline const char* shapeName (ShapeStyle s) noexcept
{
    static const char* names[] = { "Soft Tube", "Warm Tube", "Tape", "Transformer", "Console", "Diode",
                                   "Hard Clip", "Fuzz", "Rectify", "Foldback", "Sine Fold", "Crush" };
    return names[clampv ((int) s, 0, (int) ShapeStyle::Count - 1)];
}

inline float shape (ShapeStyle style, float x) noexcept
{
    switch (style)
    {
        case ShapeStyle::SoftTube:
        {
            // Slightly offset tanh: mostly odd harmonics with a little second.
            const float b = 0.25f, tb = std::tanh (b);
            return (std::tanh (x + b) - tb) / (1.0f - tb * tb);
        }
        case ShapeStyle::WarmTube:
        {
            // Stronger offset, softer curve: a clear second harmonic.
            const float b = 0.55f;
            auto f = [] (float v) { return v / (1.0f + std::abs (v)); };
            const float slope = 1.0f / ((1.0f + b) * (1.0f + b));
            return (f (x + b) - f (b)) / slope * 0.6f + 0.4f * std::tanh (x);
        }
        case ShapeStyle::Tape:
            return x / std::sqrt (1.0f + x * x);
        case ShapeStyle::Transformer:
            return (2.0f / (float) kPi) * std::atan (x * (float) kPi * 0.5f);
        case ShapeStyle::Console:
        {
            const float a = std::abs (x);
            if (a >= 1.5f)
                return x > 0.0f ? 1.0f : -1.0f;
            return x - (4.0f / 27.0f) * x * x * x;
        }
        case ShapeStyle::Diode:
            return x >= 0.0f ? 1.0f - std::exp (-x) : -std::tanh (1.6f * -x) / 1.6f;
        case ShapeStyle::HardClip:
            return clampv (x, -1.0f, 1.0f);
        case ShapeStyle::Fuzz:
        {
            // Lopsided, and it chokes off very quiet input like a starved transistor.
            const float a = std::abs (x);
            const float gate = a / (a + 0.02f);
            const float y = x >= 0.0f ? 1.0f - std::exp (-x) : -0.8f * (1.0f - std::exp (-1.25f * a));
            return y * gate;
        }
        case ShapeStyle::Rectify:
            // Part full-wave rectified: folds the negative half up, adding an octave above.
            return 0.7f * std::tanh (std::abs (x)) + 0.3f * std::tanh (x);
        case ShapeStyle::Foldback:
        {
            // Triangle fold: rises to 1, then folds back down instead of clipping.
            float t = x * 0.25f + 0.25f;
            t -= std::floor (t);
            return 1.0f - 4.0f * std::abs (t - 0.5f);
        }
        case ShapeStyle::SineFold:
            return std::sin (x);
        case ShapeStyle::Crush:
        {
            const float steps = 6.0f;
            return clampv (std::round (x * steps) / steps, -1.0f, 1.0f);
        }
        case ShapeStyle::Count: break;
    }
    return x;
}

// Works out the gain that brings a waveshaper's output back to the loudness of its
// input, by running a fixed burst of noise at -18 dBFS RMS through it. Cheap enough to
// call whenever a drive setting changes.
template <typename Shaper>
inline float levelMatchGain (Shaper&& shaper, float testRms = 0.125f) noexcept
{
    Random rng (0x51A7u);
    double in = 0.0, out = 0.0, mean = 0.0;
    constexpr int n = 768;
    float y[n];
    for (int i = 0; i < n; ++i)
    {
        // Sum of three uniform values: close to Gaussian, like real programme material.
        const float x = (rng.nextBipolar() + rng.nextBipolar() + rng.nextBipolar()) * testRms;
        y[i] = shaper (x);
        in += (double) x * x;
        mean += y[i];
    }
    mean /= n;
    for (int i = 0; i < n; ++i)
        out += ((double) y[i] - mean) * ((double) y[i] - mean);
    if (out < 1.0e-12)
        return 1.0f;
    return clampv ((float) std::sqrt (in / out), 0.03f, 30.0f);
}

//==============================================================================
// First-order tilt EQ around a pivot: negative values darken, positive brighten.
class TiltFilter
{
public:
    void set (float amount, double pivotHz, double sr) noexcept
    {
        // amount -1..1 maps to +-6 dB at the extremes of the spectrum.
        const float db = 6.0f * clampv (amount, -1.0f, 1.0f);
        lowGain = dbToGain (-db);
        highGain = dbToGain (db);
        lp.setCutoff (pivotHz, sr);
        active = std::abs (amount) > 0.005f;
    }
    void reset() noexcept { lp.reset(); }
    float process (float x) noexcept
    {
        if (! active)
            return x;
        const float low = lp.lowpass (x);
        return low * lowGain + (x - low) * highGain;
    }

private:
    OnePole lp;
    float lowGain = 1.0f, highGain = 1.0f;
    bool active = false;
};
} // namespace gitto
