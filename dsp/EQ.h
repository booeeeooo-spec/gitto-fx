// Gitto FX EQ - 8-band parametric / dynamic equaliser.
#pragma once

#include "Common.h"
#include <atomic>

namespace gitto
{
enum class EqType { Bell = 0, LowShelf, HighShelf, LowCut, HighCut, Notch, BandPass, Tilt, Count };
enum class EqStereo { Stereo = 0, Mid, Side, Left, Right, Count };

struct EqBandParams
{
    bool enabled = false;
    EqType type = EqType::Bell;
    float freq = 1000.0f;      // Hz
    float gainDb = 0.0f;       // -30..+30
    float q = 1.0f;            // 0.1..30
    int slope = 1;             // cuts only: 0=12, 1=24, 2=36, 3=48 dB/oct
    EqStereo stereo = EqStereo::Stereo;
    float dynRangeDb = 0.0f;   // 0 = static band. Negative ducks, positive expands.
    float dynThreshDb = -30.0f;
};

class EqBand
{
public:
    static constexpr int kMaxSections = 4;
    static constexpr int kChunk = 16;

    // Designs the filter sections for a band. Returns the number of sections used.
    static int design (const EqBandParams& p, double gainDb, double sr, BiquadCoefs* out) noexcept
    {
        const double f = clampv ((double) p.freq, 10.0, sr * 0.49);
        const double q = clampv ((double) p.q, 0.05, 40.0);
        switch (p.type)
        {
            case EqType::Bell:      out[0] = design::matchedPeak (f, q, gainDb, sr); return 1;
            case EqType::LowShelf:  out[0] = design::matchedLowShelf (f, clampv (q, 0.1, 2.0), gainDb, sr); return 1;
            case EqType::HighShelf: out[0] = design::matchedHighShelf (f, clampv (q, 0.1, 2.0), gainDb, sr); return 1;
            case EqType::Notch:     out[0] = design::notch (f, q, sr); return 1;
            case EqType::BandPass:  out[0] = design::bandpass (f, q, sr); return 1;
            case EqType::Tilt:      out[0] = design::matchedTilt (f, gainDb, sr); return 1;
            case EqType::LowCut:
            case EqType::HighCut:
            {
                // Butterworth section Qs for orders 2, 4, 6, 8. The Q control scales the
                // resonance of the final section so Q = 0.71 is maximally flat.
                static const double qs[4][4] = { { 0.70710678, 0, 0, 0 },
                                                 { 0.54119610, 1.30656296, 0, 0 },
                                                 { 0.51763809, 0.70710678, 1.93185165, 0 },
                                                 { 0.50979558, 0.60134489, 0.89997622, 2.56291545 } };
                const int n = clampv (p.slope, 0, 3) + 1;
                const double resScale = clampv (q / 0.70710678, 0.2, 8.0);
                for (int i = 0; i < n; ++i)
                {
                    const double sq = (i == n - 1) ? qs[n - 1][i] * resScale : qs[n - 1][i];
                    out[i] = p.type == EqType::LowCut ? design::highpass (f, sq, sr) : design::lowpass (f, sq, sr);
                }
                return n;
            }
            case EqType::Count: break;
        }
        out[0] = BiquadCoefs();
        return 1;
    }

    static bool typeHasGain (EqType t) noexcept
    {
        return t == EqType::Bell || t == EqType::LowShelf || t == EqType::HighShelf || t == EqType::Tilt;
    }
    static bool typeCanBeDynamic (EqType t) noexcept
    {
        return t == EqType::Bell || t == EqType::LowShelf || t == EqType::HighShelf;
    }

    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate;
        smoothCoef = onePoleCoef (25.0, sr / kChunk);
        mixCoef = onePoleCoef (8.0, sr);
        envAttack = onePoleCoef (3.0, sr);
        envRelease = onePoleCoef (140.0, sr);
        reset();
    }

    void reset() noexcept
    {
        for (auto& ch : filters)
            for (auto& f : ch)
                f.reset();
        for (auto& d : detector)
            d.reset();
        env = 0.0f;
        dynGain = 0.0f;
        counter = 0;
        first = true;
        justReset = true;
        mix = 0.0f;
    }

    void setParams (const EqBandParams& p) noexcept
    {
        if (p.type != params.type || p.slope != params.slope || p.stereo != params.stereo)
            structureChanged = true;
        params = p;
    }

    const EqBandParams& getParams() const noexcept { return params; }
    float getDynamicGainDb() const noexcept { return dynGainForDisplay.load (std::memory_order_relaxed); }

    // right may be nullptr for mono.
    void process (float* left, float* right, int numSamples) noexcept
    {
        // A gain-type band sitting at 0 dB with no dynamics does nothing, so it is skipped
        // entirely: the plugin is bit-transparent until a band is actually moved.
        const bool wantsDynamics = typeCanBeDynamic (params.type) && std::abs (params.dynRangeDb) > 0.05f;
        const bool idle = typeHasGain (params.type) && std::abs (params.gainDb) < 0.005f && ! wantsDynamics;
        const float mixTarget = (params.enabled && ! idle) ? 1.0f : 0.0f;
        if (justReset)
        {
            mix = mixTarget; // no fade-in when playback starts
            justReset = false;
        }
        if (mixTarget == 0.0f && mix < 1.0e-4f)
        {
            mix = 0.0f;
            first = true;
            dynGainForDisplay.store (0.0f, std::memory_order_relaxed);
            return;
        }

        if (structureChanged)
        {
            structureChanged = false;
            for (auto& ch : filters)
                for (auto& f : ch)
                    f.reset();
            first = true;
        }

        const bool dynamic = wantsDynamics;
        const EqStereo mode = right == nullptr ? EqStereo::Left : params.stereo;
        if (right == nullptr && (params.stereo == EqStereo::Side || params.stereo == EqStereo::Right))
            return;

        for (int i = 0; i < numSamples; ++i)
        {
            if (counter == 0)
                updateCoefs (dynamic);
            counter = (counter + 1) % kChunk;

            mix = mixTarget + (mix - mixTarget) * mixCoef;

            float l = left[i];
            float r = right != nullptr ? right[i] : l;

            switch (mode)
            {
                case EqStereo::Stereo:
                {
                    if (dynamic)
                        detect (0.5f * (l + r));
                    left[i] = l + (run (0, l) - l) * mix;
                    right[i] = r + (run (1, r) - r) * mix;
                    break;
                }
                case EqStereo::Mid:
                case EqStereo::Side:
                {
                    float m = 0.5f * (l + r), s = 0.5f * (l - r);
                    float& target = mode == EqStereo::Mid ? m : s;
                    if (dynamic)
                        detect (target);
                    target += (run (0, target) - target) * mix;
                    left[i] = m + s;
                    right[i] = m - s;
                    break;
                }
                case EqStereo::Left:
                {
                    if (dynamic)
                        detect (l);
                    left[i] = l + (run (0, l) - l) * mix;
                    break;
                }
                case EqStereo::Right:
                {
                    if (dynamic)
                        detect (r);
                    right[i] = r + (run (0, r) - r) * mix;
                    break;
                }
                case EqStereo::Count: break;
            }
        }

        for (auto& ch : filters)
            for (auto& f : ch)
                f.sanitise();
        env = undenorm (env);
    }

private:
    float run (int ch, float x) noexcept
    {
        for (int s = 0; s < numSections; ++s)
            x = filters[(size_t) ch][(size_t) s].process (x);
        return x;
    }

    void detect (float x) noexcept
    {
        const float d = std::abs (detector[1].process (detector[0].process (x)));
        env = d > env ? d + (env - d) * envAttack : d + (env - d) * envRelease;
    }

    void updateCoefs (bool dynamic) noexcept
    {
        const float logF = std::log (clampv (params.freq, 10.0f, (float) (sr * 0.49)));
        if (first)
        {
            sFreqLog = logF;
            sGain = params.gainDb;
            sQ = params.q;
        }
        else
        {
            sFreqLog = logF + (sFreqLog - logF) * smoothCoef;
            sGain = params.gainDb + (sGain - params.gainDb) * smoothCoef;
            sQ = params.q + (sQ - params.q) * smoothCoef;
        }

        float dynTarget = 0.0f;
        if (dynamic)
        {
            const float over = gainToDb (env) - params.dynThreshDb;
            if (over > 0.0f)
                dynTarget = params.dynRangeDb * (1.0f - std::exp (-over / 6.0f));
        }
        dynGain = first ? dynTarget : dynTarget + (dynGain - dynTarget) * 0.6f;
        dynGainForDisplay.store (dynGain, std::memory_order_relaxed);

        const bool moving = first || dynamic || std::abs (dynGain) > 1.0e-3f
                            || std::abs (sFreqLog - logF) > 1.0e-5f || std::abs (sGain - params.gainDb) > 1.0e-4f
                            || std::abs (sQ - params.q) > 1.0e-5f || lastType != params.type || lastSlope != params.slope;
        if (! moving)
            return;

        if (std::abs (sFreqLog - logF) <= 1.0e-5f) sFreqLog = logF;
        if (std::abs (sGain - params.gainDb) <= 1.0e-4f) sGain = params.gainDb;
        if (std::abs (sQ - params.q) <= 1.0e-5f) sQ = params.q;

        EqBandParams p = params;
        p.freq = std::exp (sFreqLog);
        p.q = sQ;
        BiquadCoefs c[kMaxSections];
        numSections = design (p, (double) (sGain + dynGain), sr, c);
        for (int s = 0; s < numSections; ++s)
        {
            filters[0][(size_t) s].setCoefs (c[s]);
            filters[1][(size_t) s].setCoefs (c[s]);
        }

        if (dynamic)
        {
            // The detector listens to the part of the spectrum the band acts on.
            BiquadCoefs d0, d1;
            if (params.type == EqType::Bell)
            {
                d0 = design::bandpass (p.freq, clampv (p.q, 0.3f, 12.0f), sr);
                d1 = BiquadCoefs();
            }
            else if (params.type == EqType::LowShelf)
            {
                d0 = design::lowpass (p.freq, 0.7071, sr);
                d1 = BiquadCoefs();
            }
            else
            {
                d0 = design::highpass (p.freq, 0.7071, sr);
                d1 = BiquadCoefs();
            }
            detector[0].setCoefs (d0);
            detector[1].setCoefs (d1);
        }

        lastType = params.type;
        lastSlope = params.slope;
        first = false;
    }

    double sr = 44100.0;
    EqBandParams params;
    std::array<std::array<Biquad, kMaxSections>, 2> filters;
    std::array<Biquad, 2> detector;
    int numSections = 1;
    int counter = 0;
    bool first = true, justReset = true, structureChanged = false;
    EqType lastType = EqType::Count;
    int lastSlope = -1;
    float smoothCoef = 0.0f, mixCoef = 0.0f, envAttack = 0.0f, envRelease = 0.0f;
    float sFreqLog = 0.0f, sGain = 0.0f, sQ = 1.0f;
    float env = 0.0f, dynGain = 0.0f, mix = 0.0f;
    std::atomic<float> dynGainForDisplay { 0.0f };
};

//==============================================================================
class Equaliser
{
public:
    static constexpr int kNumBands = 8;

    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate;
        for (auto& b : bands)
            b.prepare (sr);
        outGain.reset (sr, 20.0, outGainTarget); // start on the current setting
    }
    void reset() noexcept
    {
        for (auto& b : bands)
            b.reset();
    }
    void setBand (int index, const EqBandParams& p) noexcept { bands[(size_t) index].setParams (p); }
    const EqBand& getBand (int index) const noexcept { return bands[(size_t) index]; }
    void setOutputGainDb (float db) noexcept
    {
        outGainTarget = dbToGain (db);
        outGain.setTarget (outGainTarget);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        for (auto& b : bands)
            b.process (left, right, numSamples);
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = outGain.next();
            left[i] *= g;
            if (right != nullptr)
                right[i] *= g;
        }
    }

    // Magnitude response in dB of one band at the given frequency.
    static double bandMagnitudeDb (const EqBandParams& p, double extraGainDb, double hz, double sr) noexcept
    {
        BiquadCoefs c[EqBand::kMaxSections];
        const int n = EqBand::design (p, (double) p.gainDb + extraGainDb, sr, c);
        const double w = kTwoPi * hz / sr;
        double mag = 1.0;
        for (int i = 0; i < n; ++i)
            mag *= c[i].magnitude (w);
        return 20.0 * std::log10 (std::max (mag, 1.0e-9));
    }

private:
    double sr = 44100.0;
    std::array<EqBand, kNumBands> bands;
    Smoothed outGain;
    float outGainTarget = 1.0f;
};
} // namespace gitto
