// Gitto FX - shared DSP building blocks.
// Plain C++17, no framework dependency, so every algorithm can be tested offline.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gitto
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) noexcept { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }
inline double dbToGainD (double db) noexcept { return std::pow (10.0, db * 0.05); }

template <typename T>
inline T clampv (T v, T lo, T hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

inline float lerp (float a, float b, float t) noexcept { return a + (b - a) * t; }

// Coefficient for a one-pole smoother that reaches ~63% in timeMs.
inline float onePoleCoef (double timeMs, double sampleRate) noexcept
{
    if (timeMs <= 0.0)
        return 0.0f;
    return (float) std::exp (-1.0 / (0.001 * timeMs * sampleRate));
}

// Kills denormals without branching on the hot path.
inline float undenorm (float x) noexcept { return (std::abs (x) < 1.0e-20f) ? 0.0f : x; }

//==============================================================================
// Cheap, good-enough random source for modulation, dither and noise.
class Random
{
public:
    explicit Random (uint32_t seed = 0x9E3779B9u) noexcept : state (seed ? seed : 1u) {}
    void seed (uint32_t s) noexcept { state = s ? s : 1u; }

    uint32_t nextInt() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    // 0..1
    float next01() noexcept { return (float) (nextInt() >> 8) * (1.0f / 16777216.0f); }
    // -1..1
    float nextBipolar() noexcept { return next01() * 2.0f - 1.0f; }

private:
    uint32_t state;
};

//==============================================================================
// Exponential parameter smoother.
class Smoothed
{
public:
    void reset (double sampleRate, double timeMs, float initial) noexcept
    {
        coef = onePoleCoef (timeMs, sampleRate);
        current = target = initial;
    }
    void setTarget (float t) noexcept { target = t; }
    void snap() noexcept { current = target; }
    float next() noexcept
    {
        current = target + (current - target) * coef;
        return current;
    }
    float get() const noexcept { return current; }
    float getTarget() const noexcept { return target; }
    bool isSmoothing() const noexcept { return std::abs (current - target) > 1.0e-6f; }

private:
    float coef = 0.0f, current = 0.0f, target = 0.0f;
};

//==============================================================================
// Fractional delay line with a power-of-two ring buffer.
class DelayLine
{
public:
    void resize (int maxDelaySamples)
    {
        int size = 16;
        while (size < maxDelaySamples + 8)
            size <<= 1;
        buffer.assign ((size_t) size, 0.0f);
        mask = size - 1;
        writePos = 0;
    }
    void clear() noexcept { std::fill (buffer.begin(), buffer.end(), 0.0f); }
    int capacity() const noexcept { return mask - 7; }

    void write (float x) noexcept
    {
        buffer[(size_t) writePos] = x;
        writePos = (writePos + 1) & mask;
    }

    // Reads a sample written `delay` samples ago. delay >= 1 reads fully written data
    // when called before write() for the current sample.
    float readInt (int delay) const noexcept
    {
        return buffer[(size_t) ((writePos - delay) & mask)];
    }

    float readLinear (float delay) const noexcept
    {
        const int d = (int) delay;
        const float f = delay - (float) d;
        const float a = buffer[(size_t) ((writePos - d) & mask)];
        const float b = buffer[(size_t) ((writePos - d - 1) & mask)];
        return a + (b - a) * f;
    }

    // 4-point Hermite interpolation. Needs delay >= 2.
    float readCubic (float delay) const noexcept
    {
        const int d = (int) delay;
        const float f = delay - (float) d;
        const float ym1 = buffer[(size_t) ((writePos - d + 1) & mask)];
        const float y0 = buffer[(size_t) ((writePos - d) & mask)];
        const float y1 = buffer[(size_t) ((writePos - d - 1) & mask)];
        const float y2 = buffer[(size_t) ((writePos - d - 2) & mask)];
        const float c0 = y0;
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + c0;
    }

private:
    std::vector<float> buffer { std::vector<float> (16, 0.0f) };
    int mask = 15;
    int writePos = 0;
};

//==============================================================================
// One-pole low-pass / high-pass.
class OnePole
{
public:
    void setCutoff (double hz, double sampleRate) noexcept
    {
        hz = clampv (hz, 1.0, sampleRate * 0.49);
        a = (float) std::exp (-kTwoPi * hz / sampleRate);
    }
    void reset() noexcept { z = 0.0f; }
    float lowpass (float x) noexcept
    {
        z = x + (z - x) * a;
        z = undenorm (z);
        return z;
    }
    float highpass (float x) noexcept { return x - lowpass (x); }

private:
    float a = 0.0f, z = 0.0f;
};

//==============================================================================
struct BiquadCoefs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    // |H(e^jw)| at normalised angular frequency w (radians/sample).
    double magnitude (double w) const noexcept
    {
        const std::complex<double> z1 = std::polar (1.0, -w);
        const std::complex<double> z2 = std::polar (1.0, -2.0 * w);
        return std::abs ((b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2));
    }
};

// Transposed direct form II biquad.
class Biquad
{
public:
    void setCoefs (const BiquadCoefs& c) noexcept
    {
        b0 = (float) c.b0;
        b1 = (float) c.b1;
        b2 = (float) c.b2;
        a1 = (float) c.a1;
        a2 = (float) c.a2;
    }
    void reset() noexcept { z1 = z2 = 0.0f; }
    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void sanitise() noexcept
    {
        z1 = undenorm (z1);
        z2 = undenorm (z2);
    }

private:
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;
};

// Double-precision version, for filters tuned very low relative to the sample rate
// (crossovers running inside an oversampler), where single precision loses accuracy.
class BiquadD
{
public:
    void setCoefs (const BiquadCoefs& c) noexcept { k = c; }
    void reset() noexcept { z1 = z2 = 0.0; }
    float process (float x) noexcept
    {
        const double y = k.b0 * x + z1;
        z1 = k.b1 * x - k.a1 * y + z2;
        z2 = k.b2 * x - k.a2 * y;
        return (float) y;
    }
    void sanitise() noexcept
    {
        if (std::abs (z1) < 1.0e-30) z1 = 0.0;
        if (std::abs (z2) < 1.0e-30) z2 = 0.0;
    }

private:
    BiquadCoefs k;
    double z1 = 0.0, z2 = 0.0;
};

//==============================================================================
// Standard bilinear-transform designs (Bristow-Johnson "cookbook").
namespace design
{
inline BiquadCoefs normalise (double b0, double b1, double b2, double a0, double a1, double a2) noexcept
{
    BiquadCoefs c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}

inline double safeFreq (double hz, double sr) noexcept { return clampv (hz, 5.0, sr * 0.49); }

inline BiquadCoefs lowpass (double hz, double q, double sr) noexcept
{
    const double w = kTwoPi * safeFreq (hz, sr) / sr, cs = std::cos (w), al = std::sin (w) / (2.0 * q);
    return normalise ((1 - cs) * 0.5, 1 - cs, (1 - cs) * 0.5, 1 + al, -2 * cs, 1 - al);
}
inline BiquadCoefs highpass (double hz, double q, double sr) noexcept
{
    const double w = kTwoPi * safeFreq (hz, sr) / sr, cs = std::cos (w), al = std::sin (w) / (2.0 * q);
    return normalise ((1 + cs) * 0.5, -(1 + cs), (1 + cs) * 0.5, 1 + al, -2 * cs, 1 - al);
}
// Band-pass with 0 dB peak gain.
inline BiquadCoefs bandpass (double hz, double q, double sr) noexcept
{
    const double w = kTwoPi * safeFreq (hz, sr) / sr, cs = std::cos (w), al = std::sin (w) / (2.0 * q);
    return normalise (al, 0.0, -al, 1 + al, -2 * cs, 1 - al);
}
inline BiquadCoefs notch (double hz, double q, double sr) noexcept
{
    const double w = kTwoPi * safeFreq (hz, sr) / sr, cs = std::cos (w), al = std::sin (w) / (2.0 * q);
    return normalise (1.0, -2 * cs, 1.0, 1 + al, -2 * cs, 1 - al);
}
inline BiquadCoefs allpass (double hz, double q, double sr) noexcept
{
    const double w = kTwoPi * safeFreq (hz, sr) / sr, cs = std::cos (w), al = std::sin (w) / (2.0 * q);
    return normalise (1 - al, -2 * cs, 1 + al, 1 + al, -2 * cs, 1 - al);
}
inline BiquadCoefs peak (double hz, double q, double gainDb, double sr) noexcept
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w = kTwoPi * safeFreq (hz, sr) / sr, cs = std::cos (w), al = std::sin (w) / (2.0 * q);
    return normalise (1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A);
}
inline BiquadCoefs lowShelf (double hz, double q, double gainDb, double sr) noexcept
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w = kTwoPi * safeFreq (hz, sr) / sr, cs = std::cos (w), al = std::sin (w) / (2.0 * q);
    const double sq = 2.0 * std::sqrt (A) * al;
    return normalise (A * ((A + 1) - (A - 1) * cs + sq), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - sq),
                      (A + 1) + (A - 1) * cs + sq, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - sq);
}
inline BiquadCoefs highShelf (double hz, double q, double gainDb, double sr) noexcept
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w = kTwoPi * safeFreq (hz, sr) / sr, cs = std::cos (w), al = std::sin (w) / (2.0 * q);
    const double sq = 2.0 * std::sqrt (A) * al;
    return normalise (A * ((A + 1) + (A - 1) * cs + sq), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - sq),
                      (A + 1) - (A - 1) * cs + sq, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - sq);
}

//==============================================================================
// "Matched" designs. The bilinear transform squeezes a filter's shape as it
// approaches Nyquist, which makes high-frequency bells and shelves narrower and
// duller than their analog counterparts. These designs place the poles with the
// impulse-invariant mapping and then solve the numerator so the magnitude equals
// the analog prototype at DC, at Nyquist and at the centre frequency.
//
// The analog prototype is (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0) with s in
// radians per sample. Returns false if no valid minimum-phase solution exists.
inline double analogMagSq (double n2, double n1, double n0, double d2, double d1, double d0, double w) noexcept
{
    const double nr = n0 - n2 * w * w, ni = n1 * w;
    const double dr = d0 - d2 * w * w, di = d1 * w;
    return (nr * nr + ni * ni) / (dr * dr + di * di);
}

inline bool matchedSecondOrder (double n2, double n1, double n0, double d2, double d1, double d0,
                                double matchW, BiquadCoefs& out) noexcept
{
    if (d2 <= 0.0 || d0 <= 0.0)
        return false;

    const double wn = std::sqrt (d0 / d2);
    const double zeta = d1 / (2.0 * d2 * wn);
    if (! (zeta > 0.0))
        return false;

    double a1;
    if (zeta <= 1.0)
        a1 = -2.0 * std::exp (-zeta * wn) * std::cos (wn * std::sqrt (1.0 - zeta * zeta));
    else
        a1 = -2.0 * std::exp (-zeta * wn) * std::cosh (wn * std::sqrt (zeta * zeta - 1.0));
    const double a2 = std::exp (-2.0 * zeta * wn);

    const double A0 = (1.0 + a1 + a2) * (1.0 + a1 + a2);
    const double A1 = (1.0 - a1 + a2) * (1.0 - a1 + a2);
    const double A2 = -4.0 * a2;

    matchW = clampv (matchW, 1.0e-4, 0.92 * kPi);
    const double phi1 = std::sin (matchW * 0.5) * std::sin (matchW * 0.5);
    const double phi0 = 1.0 - phi1;
    const double phi2 = 4.0 * phi0 * phi1;

    const double B0 = A0 * analogMagSq (n2, n1, n0, d2, d1, d0, 0.0);
    const double B1 = A1 * analogMagSq (n2, n1, n0, d2, d1, d0, kPi);
    const double target = (A0 * phi0 + A1 * phi1 + A2 * phi2) * analogMagSq (n2, n1, n0, d2, d1, d0, matchW);
    const double B2 = (target - B0 * phi0 - B1 * phi1) / phi2;

    if (B0 < 0.0 || B1 < 0.0)
        return false;

    const double sB0 = std::sqrt (B0), sB1 = std::sqrt (B1);
    const double W = 0.5 * (sB0 + sB1);
    const double disc = W * W + B2;
    if (disc < 0.0)
        return false;

    const double b0 = 0.5 * (W + std::sqrt (disc));
    if (! (std::abs (b0) > 1.0e-12))
        return false;

    out.b0 = b0;
    out.b1 = 0.5 * (sB0 - sB1);
    out.b2 = -B2 / (4.0 * b0);
    out.a1 = a1;
    out.a2 = a2;
    return std::isfinite (out.b0) && std::isfinite (out.b1) && std::isfinite (out.b2);
}

// Swaps numerator and denominator. Valid because the designs above are minimum phase.
inline BiquadCoefs invert (const BiquadCoefs& c) noexcept
{
    BiquadCoefs r;
    r.b0 = 1.0 / c.b0;
    r.b1 = c.a1 / c.b0;
    r.b2 = c.a2 / c.b0;
    r.a1 = c.b1 / c.b0;
    r.a2 = c.b2 / c.b0;
    return r;
}

inline bool isStable (const BiquadCoefs& c) noexcept
{
    return std::abs (c.a2) < 1.0 && std::abs (c.a1) < 1.0 + c.a2;
}

// A cut is designed as the exact inverse of the matching boost, so boost and cut
// curves mirror each other and both follow the analog shape.
inline BiquadCoefs matchedPeak (double hz, double q, double gainDb, double sr) noexcept
{
    hz = safeFreq (hz, sr);
    const double A = std::pow (10.0, std::abs (gainDb) / 40.0);
    const double w0 = kTwoPi * hz / sr;
    BiquadCoefs c;
    // (s^2 + s w0 A/Q + w0^2) / (s^2 + s w0/(A Q) + w0^2)
    if (matchedSecondOrder (1.0, w0 * A / q, w0 * w0, 1.0, w0 / (A * q), w0 * w0, w0, c))
    {
        if (gainDb >= 0.0)
            return c;
        const BiquadCoefs inv = invert (c);
        if (isStable (inv))
            return inv;
    }
    return peak (hz, q, gainDb, sr);
}

inline BiquadCoefs matchedLowShelf (double hz, double q, double gainDb, double sr) noexcept
{
    hz = safeFreq (hz, sr);
    const double A = std::pow (10.0, std::abs (gainDb) / 40.0), sA = std::sqrt (A);
    const double w0 = kTwoPi * hz / sr;
    BiquadCoefs c;
    // Boost prototype: A (s^2 + (sqrt(A)/Q) w0 s + A w0^2) / (A s^2 + (sqrt(A)/Q) w0 s + w0^2)
    if (matchedSecondOrder (A, A * sA / q * w0, A * A * w0 * w0, A, sA / q * w0, w0 * w0, w0, c))
    {
        if (gainDb >= 0.0)
            return c;
        const BiquadCoefs inv = invert (c);
        if (isStable (inv))
            return inv;
    }
    return lowShelf (hz, q, gainDb, sr);
}

inline BiquadCoefs matchedHighShelf (double hz, double q, double gainDb, double sr) noexcept
{
    hz = safeFreq (hz, sr);
    // Cut prototype (A < 1), which is the well-conditioned direction for a high shelf.
    const double A = std::pow (10.0, -std::abs (gainDb) / 40.0), sA = std::sqrt (A);
    const double w0 = kTwoPi * hz / sr;
    BiquadCoefs c;
    // A (A s^2 + (sqrt(A)/Q) w0 s + w0^2) / (s^2 + (sqrt(A)/Q) w0 s + A w0^2)
    if (matchedSecondOrder (A * A, A * sA / q * w0, A * w0 * w0, 1.0, sA / q * w0, A * w0 * w0, w0, c))
    {
        if (gainDb <= 0.0)
            return c;
        const BiquadCoefs inv = invert (c);
        if (isStable (inv))
            return inv;
    }
    return highShelf (hz, q, gainDb, sr);
}

// First-order tilt: -gain/2 at DC, +gain/2 at high frequencies, unity at the pivot.
// Matched at DC and Nyquist.
inline BiquadCoefs matchedTilt (double hz, double gainDb, double sr) noexcept
{
    hz = safeFreq (hz, sr);
    const double k = std::pow (10.0, gainDb / 40.0); // sqrt of total linear tilt
    const double w0 = kTwoPi * hz / sr;
    // H(s) = (k s + w0) / (s + k w0)
    const double a1 = -std::exp (-k * w0);
    const double dc = 1.0 / k;
    const double ny = std::sqrt ((k * k * kPi * kPi + w0 * w0) / (kPi * kPi + k * k * w0 * w0));
    const double sum = (1.0 + a1) * dc;   // b0 + b1
    const double diff = (1.0 - a1) * ny;  // b0 - b1
    BiquadCoefs c;
    c.b0 = 0.5 * (sum + diff);
    c.b1 = 0.5 * (sum - diff);
    c.b2 = 0.0;
    c.a1 = a1;
    c.a2 = 0.0;
    return c;
}
} // namespace design

//==============================================================================
// In-place radix-2 FFT (size must be a power of two).
inline void fft (std::vector<std::complex<float>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -kTwoPi / (double) len;
        const std::complex<float> wl ((float) std::cos (ang), (float) std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<float> w (1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

//==============================================================================
// 8x polyphase interpolator used to estimate inter-sample ("true") peaks.
// Kaiser-windowed sinc, 32 taps per phase (256 in all). Group delay is kLatency input samples.
class TruePeakDetector
{
public:
    static constexpr int kPhases = 8;
    #ifndef GITTO_TP_TAPS
 #define GITTO_TP_TAPS 32
#endif
    static constexpr int kTapsPerPhase = GITTO_TP_TAPS;
    static constexpr int kLatency = kTapsPerPhase / 2; // group delay in samples at the base rate, rounded up

    TruePeakDetector()
    {
        const int total = kPhases * kTapsPerPhase;
        const double centre = (total - 1) * 0.5;
        const double beta = 5.0;
        auto bessel0 = [] (double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 30; ++k)
            {
                term *= (x * 0.5 / k) * (x * 0.5 / k);
                sum += term;
            }
            return sum;
        };
        for (int i = 0; i < total; ++i)
        {
            const double t = ((double) i - centre) / (double) kPhases;
            const double sinc = std::abs (t) < 1.0e-9 ? 1.0 : std::sin (kPi * t) / (kPi * t);
            const double r = ((double) i - centre) / centre;
            const double win = bessel0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / bessel0 (beta);
            coefs[(size_t) (i % kPhases)][(size_t) (i / kPhases)] = (float) (sinc * win);
        }
        // Normalise each phase to unity DC gain.
        for (auto& ph : coefs)
        {
            float sum = 0.0f;
            for (float c : ph)
                sum += c;
            for (float& c : ph)
                c /= sum;
        }
        reset();
    }

    void reset() noexcept
    {
        history.fill (0.0f);
        pos = 0;
    }

    // Pushes one sample, returns the largest absolute value of the interpolated points.
    float process (float x) noexcept
    {
        // The history is written twice so each phase reads a contiguous run of samples.
        history[(size_t) pos] = x;
        history[(size_t) (pos + kTapsPerPhase)] = x;
        const float* h = history.data() + pos + 1; // oldest sample first
        float peak = 0.0f;
        for (int ph = 0; ph < kPhases; ++ph)
        {
            const auto& c = coefs[(size_t) ph];
            float acc = 0.0f;
            for (int t = 0; t < kTapsPerPhase; ++t)
                acc += c[(size_t) (kTapsPerPhase - 1 - t)] * h[t];
            peak = std::max (peak, std::abs (acc));
        }
        pos = (pos + 1) % kTapsPerPhase;
        return peak;
    }

private:
    std::array<std::array<float, kTapsPerPhase>, kPhases> coefs {};
    std::array<float, 2 * kTapsPerPhase> history {};
    int pos = 0;
};

//==============================================================================
// Peak-hold level meter value with decay, written on the audio thread.
struct LevelFollower
{
    void prepare (double sampleRate) noexcept { decay = onePoleCoef (300.0, sampleRate); }
    void push (float absValue) noexcept { value = absValue > value ? absValue : value * decay; }
    float value = 0.0f, decay = 0.999f;
};
} // namespace gitto
