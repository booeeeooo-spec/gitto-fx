// Gitto FX Space - very large ambiences built from delay networks: anything from
// a single feeding-back echo to an endless wash.
#pragma once

#include "Common.h"

namespace gitto
{
enum class SpaceMode { Drift = 0, Bloom, Cascade, Vapor, Canyon, Glacier, Aurora, Abyss, Count };

struct SpaceParams
{
    SpaceMode mode = SpaceMode::Bloom;
    float mix = 0.35f;
    float width = 1.0f;         // 0..2
    float delayMs = 300.0f;     // 10..2000: the longest delay in the network
    float warp = 0.6f;          // 0..1: how far the other delays spread away from it
    float feedback = 0.6f;      // 0..1
    float density = 0.6f;       // 0..1
    float modRateHz = 0.4f;     // 0.01..10
    float modDepth = 0.3f;      // 0..1
    float lowCutHz = 20.0f;     // 10..2000 inside the loop
    float highCutHz = 9000.0f;  // 200..20000 inside the loop
    bool freeze = false;
};

class Space
{
public:
    static constexpr int kMaxLines = 8;
    static constexpr int kMaxDiffusers = 6;
    static constexpr double kMaxDelayMs = 2000.0;

    enum class Topology { Parallel, Series, Cross };

    struct ModeData
    {
        const char* name;
        Topology topology;
        int lines;
        float ratios[kMaxLines];   // delay of each line relative to the longest, at full warp
        int diffusers;             // all-pass stages on the way in
        float diffuserSpan;        // their length relative to the delay time (longer = slower build)
        float modScale;
        float dark;                // multiplies the high cut
        float trimDb;
    };

    static const ModeData& modeData (SpaceMode m) noexcept
    {
        static const ModeData data[(int) SpaceMode::Count] = {
            { "Drift", Topology::Parallel, 4, { 1.0f, 0.79f, 0.61f, 0.47f }, 2, 0.02f, 1.0f, 1.0f, 0.0f },
            { "Bloom", Topology::Parallel, 8, { 1.0f, 0.91f, 0.83f, 0.74f, 0.66f, 0.57f, 0.49f, 0.41f }, 6, 0.12f, 1.0f, 1.0f, 0.0f },
            { "Cascade", Topology::Series, 4, { 1.0f, 0.71f, 0.53f, 0.37f }, 3, 0.03f, 0.8f, 1.0f, 0.0f },
            { "Vapor", Topology::Parallel, 8, { 1.0f, 0.87f, 0.73f, 0.62f, 0.51f, 0.43f, 0.34f, 0.27f }, 6, 0.05f, 1.4f, 0.8f, 0.0f },
            { "Canyon", Topology::Cross, 2, { 1.0f, 0.67f }, 2, 0.015f, 0.6f, 1.0f, 0.0f },
            { "Glacier", Topology::Parallel, 8, { 1.0f, 0.93f, 0.86f, 0.80f, 0.73f, 0.67f, 0.60f, 0.54f }, 6, 0.3f, 0.8f, 0.7f, 0.0f },
            { "Aurora", Topology::Parallel, 8, { 1.0f, 0.89f, 0.77f, 0.68f, 0.58f, 0.46f, 0.37f, 0.29f }, 4, 0.06f, 3.2f, 1.6f, 0.0f },
            { "Abyss", Topology::Series, 8, { 1.0f, 0.83f, 0.69f, 0.58f, 0.47f, 0.39f, 0.31f, 0.23f }, 5, 0.1f, 1.2f, 0.35f, 0.0f },
        };
        return data[clampv ((int) m, 0, (int) SpaceMode::Count - 1)];
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        const int maxLine = (int) (sr * (kMaxDelayMs * 0.001 + 0.03)) + 64;
        for (auto& l : lines)
            l.resize (maxLine);
        for (auto& ch : diffusers)
            for (auto& d : ch)
                d.resize ((int) (sr * 0.26) + 64);
        mixSm.reset (sr, 30.0, 0.35f);
        delaySm.reset (sr, 250.0, (float) (0.3 * sr));
        warpSm.reset (sr, 250.0, 0.6f);
        fbSm.reset (sr, 40.0, 0.6f);
        for (int i = 0; i < kMaxLines; ++i)
            lfoPhase[(size_t) i] = (float) i / (float) kMaxLines;
        reset();
        first = true;
        update();
    }

    void reset() noexcept
    {
        for (auto& l : lines) l.clear();
        for (auto& ch : diffusers)
            for (auto& d : ch)
                d.clear();
        lp.fill (0.0f);
        hp.fill (0.0f);
    }

    void setParams (const SpaceParams& p) noexcept
    {
        params = p;
        update();
    }

    // Delay of one line in milliseconds with the current settings, for the display.
    float lineDelayMs (int i) const noexcept
    {
        const auto& md = modeData (params.mode);
        return params.delayMs * (1.0f + (md.ratios[clampv (i, 0, md.lines - 1)] - 1.0f) * params.warp);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const auto& md = modeData (params.mode);
        const bool stereo = right != nullptr;
        const int n = md.lines;
        const float inGain = params.freeze ? 0.0f : 1.0f;
        const float norm = 1.0f / std::sqrt ((float) n);

        for (int s = 0; s < numSamples; ++s)
        {
            const float dryL = left[s], dryR = stereo ? right[s] : dryL;
            const float base = delaySm.next(), warp = warpSm.next(), fb = fbSm.next();

            // Diffusion: smears each side before it enters the network.
            float in[2] = { dryL * inGain, dryR * inGain };
            if (diffCoef > 0.005f)
                for (int c = 0; c < 2; ++c)
                    for (int k = 0; k < md.diffusers; ++k)
                        in[c] = allpass (diffusers[(size_t) c][(size_t) k], in[c], diffLen[(size_t) c][(size_t) k]);

            // Read every line (modulated), then filter inside the loop.
            float out[kMaxLines];
            for (int i = 0; i < n; ++i)
            {
                lfoPhase[(size_t) i] += lfoInc * (1.0f + 0.09f * (float) i);
                if (lfoPhase[(size_t) i] >= 1.0f) lfoPhase[(size_t) i] -= 1.0f;
                const float len = base * (1.0f + (md.ratios[i] - 1.0f) * warp);
                // Modulation depth is kept below a third of the line so short lines stay clean.
                const float d = std::max (3.0f, len + std::min (modSamples, 0.3f * len) * std::sin ((float) kTwoPi * lfoPhase[(size_t) i]));
                float y = lines[(size_t) i].readCubic (d);
                if (! params.freeze)
                {
                    lp[(size_t) i] = y + (lp[(size_t) i] - y) * lpCoef;
                    y = lp[(size_t) i];
                    hp[(size_t) i] = y + (hp[(size_t) i] - y) * hpCoef;
                    y -= hp[(size_t) i];
                }
                out[i] = y;
            }

            float wetL = 0.0f, wetR = 0.0f;
            if (md.topology == Topology::Parallel)
            {
                float mixed[kMaxLines];
                for (int i = 0; i < n; ++i)
                    mixed[i] = out[i] * fb;
                // Hadamard mix: every line feeds every other line, keeping total energy.
                for (int half = 1; half < n; half <<= 1)
                    for (int i = 0; i < n; i += half << 1)
                        for (int j = i; j < i + half; ++j)
                        {
                            const float a = mixed[j], b = mixed[j + half];
                            mixed[j] = a + b;
                            mixed[j + half] = a - b;
                        }
                for (int i = 0; i < n; ++i)
                {
                    lines[(size_t) i].write (bound (mixed[i] * norm + in[i & 1]));
                    ((i & 1) ? wetR : wetL) += out[i] * ((i & 2) ? -1.0f : 1.0f);
                }
                wetL *= norm * 1.414f;
                wetR *= norm * 1.414f;
            }
            else if (md.topology == Topology::Series)
            {
                // One long chain: each line feeds the next, the last feeds the first.
                lines[0].write (bound (0.5f * (in[0] + in[1]) + out[n - 1] * fb));
                for (int i = 1; i < n; ++i)
                    lines[(size_t) i].write (bound (out[i - 1]));
                for (int i = 0; i < n; ++i)
                {
                    const float pan = (i & 1) ? 0.8f : -0.8f;
                    const float lvl = 1.0f / std::sqrt ((float) (i + 1));
                    wetL += out[i] * lvl * (1.0f - std::max (0.0f, pan));
                    wetR += out[i] * lvl * (1.0f + std::min (0.0f, pan));
                }
                wetL *= 0.7f;
                wetR *= 0.7f;
            }
            else
            {
                // Two lines feeding each other across the stereo field.
                lines[0].write (bound (in[0] + out[1] * fb));
                lines[1].write (bound (in[1] + out[0] * fb));
                wetL = out[0];
                wetR = out[1];
            }

            wetL *= trim;
            wetR *= trim;
            const float mid = 0.5f * (wetL + wetR), side = 0.5f * (wetL - wetR) * params.width;
            wetL = mid + side;
            wetR = mid - side;

            const float mx = mixSm.next();
            const float dryGain = std::cos (mx * 0.5f * (float) kPi), wetGain = std::sin (mx * 0.5f * (float) kPi);
            if (stereo)
            {
                left[s] = dryL * dryGain + wetL * wetGain;
                right[s] = dryR * dryGain + wetR * wetGain;
            }
            else
            {
                left[s] = dryL * dryGain + mid * wetGain;
            }
        }
        for (auto& v : lp) v = undenorm (v);
        for (auto& v : hp) v = undenorm (v);
    }

private:
    // Transparent below full scale, bounded above it: keeps full feedback from running away.
    static float bound (float x) noexcept
    {
        const float a = std::abs (x);
        if (a > 1.0f)
            x = (x > 0.0f ? 1.0f : -1.0f) * (1.0f + std::tanh (a - 1.0f));
        return undenorm (x);
    }

    float allpass (DelayLine& d, float x, int delaySamples) const noexcept
    {
        const float delayed = d.readInt (delaySamples);
        const float v = x - diffCoef * delayed;
        d.write (undenorm (v));
        return delayed + diffCoef * v;
    }

    void update() noexcept
    {
        const auto& md = modeData (params.mode);
        const double delaySec = clampv ((double) params.delayMs, 5.0, kMaxDelayMs) * 0.001;
        delaySm.setTarget ((float) (delaySec * sr));
        warpSm.setTarget (clampv (params.warp, 0.0f, 1.0f));
        // Feedback tops out just under unity; Freeze takes it the rest of the way.
        fbSm.setTarget (params.freeze ? 1.0f : 0.985f * clampv (params.feedback, 0.0f, 1.0f));
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));

        diffCoef = 0.72f * clampv (params.density, 0.0f, 1.0f);
        static const double spread[2][kMaxDiffusers] = { { 1.0, 0.71, 0.53, 0.37, 0.29, 0.19 }, { 0.93, 0.67, 0.49, 0.41, 0.26, 0.17 } };
        const double span = clampv (delaySec * md.diffuserSpan, 0.002, 0.25);
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < kMaxDiffusers; ++k)
                diffLen[(size_t) c][(size_t) k] = std::max (2, (int) (span * spread[c][k] * sr));

        lfoInc = (float) (clampv ((double) params.modRateHz, 0.005, 20.0) / sr);
        modSamples = (float) (params.modDepth * md.modScale * 0.0012 * sr);
        lpCoef = (float) std::exp (-kTwoPi * clampv ((double) params.highCutHz * md.dark, 100.0, sr * 0.45) / sr);
        hpCoef = (float) std::exp (-kTwoPi * clampv ((double) params.lowCutHz, 5.0, 4000.0) / sr);
        trim = dbToGain (md.trimDb);

        if (first)
        {
            delaySm.snap();
            warpSm.snap();
            fbSm.snap();
            mixSm.snap();
            first = false;
        }
    }

    double sr = 44100.0;
    SpaceParams params;
    bool first = true;
    std::array<DelayLine, kMaxLines> lines;
    std::array<std::array<DelayLine, kMaxDiffusers>, 2> diffusers;
    std::array<std::array<int, kMaxDiffusers>, 2> diffLen {};
    std::array<float, kMaxLines> lp {}, hp {}, lfoPhase {};
    float diffCoef = 0.4f, lfoInc = 0.0f, modSamples = 0.0f, lpCoef = 0.0f, hpCoef = 0.999f, trim = 1.0f;
    Smoothed mixSm, delaySm, warpSm, fbSm;
};
} // namespace gitto
