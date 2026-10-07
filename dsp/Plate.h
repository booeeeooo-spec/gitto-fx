// Gitto FX Plate - plate reverb. The tank is the figure-eight network Jon Dattorro
// published in "Effect Design, Part 1" (JAES, 1997), with six voicings on top.
#pragma once

#include "Common.h"

namespace gitto
{
enum class PlateType { Classic = 0, Bright, Dark, Dense, Thin, Vintage, Count };

struct PlateParams
{
    PlateType type = PlateType::Classic;
    float decaySec = 2.0f;     // 0.3..20
    float predelayMs = 10.0f;  // 0..250
    float size = 0.5f;         // 0..1
    float dampHz = 7000.0f;    // 1000..16000
    float lowCutHz = 80.0f;    // 20..500 on the wet signal
    float modDepth = 0.3f;     // 0..1
    float width = 1.0f;        // 0..2
    float mix = 0.3f;
};

class Plate
{
public:
    struct TypeData
    {
        float scale;         // tank size multiplier
        float inDiff1, inDiff2;
        float decayDiff1, decayDiff2;
        float brightness;    // multiplies the damping frequency
        int dispersion;      // number of dispersion stages on the way in
        float modScale;
        float bandwidthHz;   // input bandwidth
        float trimDb;        // level match between types
    };

    static const TypeData& typeData (PlateType t) noexcept
    {
        static const TypeData data[(int) PlateType::Count] = {
            { 1.00f, 0.75f, 0.625f, 0.70f, 0.50f, 1.0f, 0, 1.0f, 18000.0f, -0.6f },  // Classic
            { 0.85f, 0.80f, 0.70f, 0.70f, 0.50f, 1.9f, 0, 0.8f, 20000.0f, -0.7f },   // Bright
            { 1.15f, 0.75f, 0.625f, 0.70f, 0.55f, 0.5f, 0, 1.0f, 9000.0f, -0.1f },   // Dark
            { 0.70f, 0.80f, 0.72f, 0.76f, 0.62f, 1.0f, 12, 0.7f, 16000.0f, -2.9f },  // Dense
            { 0.55f, 0.70f, 0.55f, 0.60f, 0.40f, 1.4f, 0, 0.6f, 20000.0f, -1.8f },   // Thin
            { 1.00f, 0.75f, 0.65f, 0.72f, 0.55f, 0.75f, 28, 1.8f, 8500.0f, -0.2f },  // Vintage
        };
        return data[clampv ((int) t, 0, (int) PlateType::Count - 1)];
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        predelay.resize ((int) (0.26 * sr) + 16);
        for (auto& d : inAp) d.resize ((int) (0.03 * sr) + 16);
        for (auto& h : half)
        {
            h.modAp.resize ((int) (0.09 * sr) + 64);
            h.delay1.resize ((int) (0.36 * sr) + 16);
            h.ap2.resize ((int) (0.21 * sr) + 16);
            h.delay2.resize ((int) (0.30 * sr) + 16);
        }
        mixSm.reset (sr, 30.0, 0.3f);
        sizeSm.reset (sr, 150.0, 1.0f);
        predelaySm.reset (sr, 60.0, 0.0f);
        reset();
        first = true;
        update();
    }

    void reset() noexcept
    {
        predelay.clear();
        for (auto& d : inAp) d.clear();
        for (auto& h : half)
        {
            h.modAp.clear();
            h.delay1.clear();
            h.ap2.clear();
            h.delay2.clear();
            h.damp = 0.0f;
            h.out = 0.0f;
        }
        dispState.fill (0.0f);
        bandwidthState = 0.0f;
        for (auto& f : lowCut) f.reset();
        lfo = { 0.0f, 0.25f };
    }

    void setParams (const PlateParams& p) noexcept
    {
        params = p;
        update();
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const auto& td = typeData (params.type);
        const bool stereo = right != nullptr;

        for (int n = 0; n < numSamples; ++n)
        {
            const float dryL = left[n], dryR = stereo ? right[n] : dryL;
            const float s = sizeSm.next();
            const float pd = predelaySm.next();

            // Mono drive into the plate, as with a real one.
            predelay.write (0.5f * (dryL + dryR));
            float x = predelay.readLinear (pd + 1.0f);
            bandwidthState = x + (bandwidthState - x) * bandwidthCoef;
            x = bandwidthState;

            // Dispersion: highs arrive a touch ahead of lows, the plate's signature "ping".
            for (int k = 0; k < td.dispersion; ++k)
            {
                const float y = -kDispersion * x + dispState[(size_t) (2 * k)] + kDispersion * dispState[(size_t) (2 * k + 1)];
                dispState[(size_t) (2 * k)] = x;
                dispState[(size_t) (2 * k + 1)] = undenorm (y);
                x = y;
            }

            x = allpass (inAp[0], x, kInAp[0] * (float) sr, td.inDiff1);
            x = allpass (inAp[1], x, kInAp[1] * (float) sr, td.inDiff1);
            x = allpass (inAp[2], x, kInAp[2] * (float) sr, td.inDiff2);
            x = allpass (inAp[3], x, kInAp[3] * (float) sr, td.inDiff2);

            // Figure-eight tank: each half feeds the other.
            const float cross[2] = { half[1].out, half[0].out };
            for (int h = 0; h < 2; ++h)
            {
                auto& t = half[(size_t) h];
                lfo[(size_t) h] += lfoInc[(size_t) h];
                if (lfo[(size_t) h] >= 1.0f) lfo[(size_t) h] -= 1.0f;
                const float mod = modSamples * std::sin ((float) kTwoPi * lfo[(size_t) h]);

                // Modulated all-pass (note the inverted sign, as in the published design).
                const float len = kModAp[h] * (float) sr * s;
                const float delayed = moving ? t.modAp.readCubic (std::max (3.0f, len + mod)) : t.modAp.readInt ((int) (len + 0.5f));
                const float in = x + decay * cross[h];
                const float v = in + td.decayDiff1 * delayed;
                t.modAp.write (undenorm (v));
                float y = delayed - td.decayDiff1 * v;

                t.delay1.write (y);
                y = t.delay1.readInt (samples (kDelay1[h], s));
                t.damp = y + (t.damp - y) * dampCoef;
                y = undenorm (t.damp) * decay;
                y = allpass (t.ap2, y, kAp2[h] * (float) sr * s, td.decayDiff2);
                t.delay2.write (y);
                t.out = t.delay2.readInt (samples (kDelay2[h], s));
            }

            // Output taps spread around both halves of the tank.
            auto& a = half[0];
            auto& b = half[1];
            float wetL = b.delay1.readInt (samples (0.00894f, s)) + b.delay1.readInt (samples (0.09993f, s))
                         - b.ap2.readInt (samples (0.06428f, s)) + b.delay2.readInt (samples (0.06707f, s))
                         - a.delay1.readInt (samples (0.06687f, s)) - a.ap2.readInt (samples (0.00628f, s))
                         - a.delay2.readInt (samples (0.03582f, s));
            float wetR = a.delay1.readInt (samples (0.01186f, s)) + a.delay1.readInt (samples (0.12187f, s))
                         - a.ap2.readInt (samples (0.04126f, s)) + a.delay2.readInt (samples (0.08982f, s))
                         - b.delay1.readInt (samples (0.07093f, s)) - b.ap2.readInt (samples (0.01126f, s))
                         - b.delay2.readInt (samples (0.00407f, s));
            wetL *= outGain;
            wetR *= outGain;

            if (lowCutActive)
            {
                wetL = lowCut[0].process (wetL);
                wetR = lowCut[1].process (wetR);
            }
            const float mid = 0.5f * (wetL + wetR), side = 0.5f * (wetL - wetR) * params.width;
            wetL = mid + side;
            wetR = mid - side;

            const float mx = mixSm.next();
            const float dryGain = std::cos (mx * 0.5f * (float) kPi), wetGain = std::sin (mx * 0.5f * (float) kPi);
            if (stereo)
            {
                left[n] = dryL * dryGain + wetL * wetGain;
                right[n] = dryR * dryGain + wetR * wetGain;
            }
            else
            {
                left[n] = dryL * dryGain + mid * wetGain;
            }
        }
        for (auto& f : lowCut)
            f.sanitise();
        bandwidthState = undenorm (bandwidthState);
    }

private:
    // Delay lengths in seconds, converted from the published sample counts.
    static constexpr float kInAp[4] = { 0.004771f, 0.003595f, 0.012735f, 0.009307f };
    static constexpr float kModAp[2] = { 0.022580f, 0.030510f };
    static constexpr float kDelay1[2] = { 0.149625f, 0.141696f };
    static constexpr float kAp2[2] = { 0.060482f, 0.089244f };
    static constexpr float kDelay2[2] = { 0.124996f, 0.106280f };
    static constexpr float kDispersion = 0.7f;
    static constexpr int kMaxDispersion = 32;

    struct Half
    {
        DelayLine modAp, delay1, ap2, delay2;
        float damp = 0.0f, out = 0.0f;
    };

    int samples (float seconds, float scale) const noexcept { return std::max (1, (int) (seconds * (float) sr * scale + 0.5f)); }

    static float allpass (DelayLine& d, float x, float delaySamples, float g) noexcept
    {
        const float delayed = d.readInt (std::max (1, (int) (delaySamples + 0.5f)));
        const float v = x - g * delayed;
        d.write (undenorm (v));
        return delayed + g * v;
    }

    void update() noexcept
    {
        const auto& td = typeData (params.type);
        const float scale = td.scale * (0.6f + 0.8f * clampv (params.size, 0.0f, 1.0f));
        sizeSm.setTarget (scale);
        predelaySm.setTarget ((float) (clampv (params.predelayMs, 0.0f, 250.0f) * 0.001 * sr));
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));

        // One lap of the figure-eight passes the decay gain four times.
        double lap = 0.0;
        for (int h = 0; h < 2; ++h)
            lap += kModAp[h] + kDelay1[h] + kAp2[h] + kDelay2[h];
        lap *= scale * 1.1; // the all-passes in the loop ring on a little longer than their length
        const double rt = clampv ((double) params.decaySec, 0.1, 60.0);
        decay = (float) std::pow (10.0, -3.0 * lap / (4.0 * rt));

        const double damp = clampv ((double) params.dampHz * td.brightness, 400.0, sr * 0.45);
        dampCoef = (float) std::exp (-kTwoPi * damp / sr);
        bandwidthCoef = (float) std::exp (-kTwoPi * std::min ((double) td.bandwidthHz, sr * 0.45) / sr);

        modSamples = (float) (params.modDepth * td.modScale * 0.00054 * sr);
        moving = modSamples > 0.01f;
        lfoInc = { (float) (1.0 / sr), (float) (0.71 / sr) };

        lowCutActive = params.lowCutHz > 21.0f;
        const auto hp = design::highpass (params.lowCutHz, 0.7071, sr);
        lowCut[0].setCoefs (hp);
        lowCut[1].setCoefs (hp);

        // Short decays put less energy in the tank; lift them so Mix feels consistent.
        outGain = (float) (0.6 * dbToGainD (td.trimDb) / std::sqrt (clampv (rt, 0.3, 10.0)) * 1.4);

        if (first)
        {
            sizeSm.snap();
            predelaySm.snap();
            mixSm.snap();
            first = false;
        }
    }

    double sr = 44100.0;
    PlateParams params;
    bool first = true, moving = false, lowCutActive = false;
    DelayLine predelay;
    std::array<DelayLine, 4> inAp;
    std::array<Half, 2> half;
    std::array<float, 2 * kMaxDispersion> dispState {};
    std::array<float, 2> lfo { 0.0f, 0.25f }, lfoInc { 0.0f, 0.0f };
    std::array<Biquad, 2> lowCut;
    float decay = 0.5f, dampCoef = 0.0f, bandwidthCoef = 0.0f, bandwidthState = 0.0f;
    float modSamples = 0.0f, outGain = 0.5f;
    Smoothed mixSm, sizeSm, predelaySm;
};
} // namespace gitto
