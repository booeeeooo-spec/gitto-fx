// Gitto FX Chorus - four chorus architectures with preset modes and a manual mode.
#pragma once

#include "Common.h"

namespace gitto
{
enum class ChorusType { Dimension = 0, Classic, Ensemble, Modern, Count };
enum class ChorusMode { I = 0, II, III, IV, Manual, Count };
enum class ChorusShape { Triangle = 0, Sine, Random, Count };

struct ChorusParams
{
    ChorusType type = ChorusType::Dimension;
    ChorusMode mode = ChorusMode::II;
    ChorusShape shape = ChorusShape::Triangle;
    float rateHz = 0.5f;     // 0.05..10 (Manual)
    float depth = 0.5f;      // 0..1 (Manual)
    float delayMs = 10.0f;   // 1..30 (Manual)
    float feedback = 0.0f;   // -0.9..0.9
    float toneHz = 12000.0f; // 1000..20000
    float warmth = 0.3f;     // 0..1
    float width = 1.0f;      // 0..2
    float mix = 0.5f;        // 0..1
    float outputDb = 0.0f;
};

class Chorus
{
public:
    static constexpr int kMaxVoices = 6;

    struct Voice
    {
        float source;      // 0 = left, 1 = right, 0.5 = mono sum
        float phase;       // LFO phase offset 0..1
        float rateMul;
        float delayOffsetMs;
        float outL, outR;
    };

    struct TypeData
    {
        int numVoices;
        Voice voices[kMaxVoices];
        float wetHighpassHz;
        float fastLfoDepth;  // amount of a second, faster LFO (ensemble shimmer)
        float fastLfoRatio;
        // Preset modes I..IV: rate (Hz), depth (ms, peak), base delay (ms).
        float presetRate[4], presetDepthMs[4], presetDelayMs[4];
    };

    static const TypeData& typeData (ChorusType t) noexcept
    {
        static const TypeData data[(int) ChorusType::Count] = {
            // Dimension: two anti-phase lines, each bleeding inverted into the other side.
            { 2,
              { { 0.5f, 0.0f, 1.0f, 0.0f, 1.0f, -0.3f }, { 0.5f, 0.5f, 1.0f, 0.0f, -0.3f, 1.0f } },
              160.0f, 0.0f, 1.0f,
              { 0.26f, 0.5f, 0.5f, 1.0f }, { 0.9f, 1.1f, 2.2f, 2.7f }, { 11.0f, 11.0f, 12.0f, 12.5f } },
            // Classic: one modulated line per side, moving in opposite directions.
            { 2,
              { { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f }, { 1.0f, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f } },
              40.0f, 0.0f, 1.0f,
              { 0.5f, 0.83f, 9.2f, 1.2f }, { 1.9f, 1.9f, 0.16f, 2.6f }, { 3.6f, 3.6f, 3.3f, 4.2f } },
            // Ensemble: three lines 120 degrees apart with a slow and a fast LFO.
            { 3,
              { { 0.5f, 0.0f, 1.0f, 0.0f, 1.0f, 0.2f }, { 0.5f, 0.3333f, 1.0f, 0.6f, 0.6f, 0.6f }, { 0.5f, 0.6667f, 1.0f, 1.2f, 0.2f, 1.0f } },
              60.0f, 0.2f, 9.7f,
              { 0.6f, 0.75f, 0.45f, 1.1f }, { 1.6f, 2.4f, 3.4f, 2.2f }, { 6.0f, 6.5f, 7.5f, 6.0f } },
            // Modern: three voices per side with staggered phases, rates and delays.
            { 6,
              { { 0.0f, 0.0f, 1.0f, 0.0f, 0.62f, 0.0f }, { 0.0f, 0.37f, 1.13f, 3.1f, 0.62f, 0.0f }, { 0.0f, 0.71f, 0.89f, 6.3f, 0.62f, 0.0f },
                { 1.0f, 0.19f, 1.07f, 1.4f, 0.0f, 0.62f }, { 1.0f, 0.53f, 0.93f, 4.6f, 0.0f, 0.62f }, { 1.0f, 0.87f, 1.19f, 7.9f, 0.0f, 0.62f } },
              30.0f, 0.0f, 1.0f,
              { 0.35f, 0.6f, 0.8f, 0.12f }, { 1.2f, 2.4f, 4.2f, 5.5f }, { 9.0f, 12.0f, 15.0f, 18.0f } },
        };
        return data[clampv ((int) t, 0, (int) ChorusType::Count - 1)];
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        for (auto& l : lines)
            l.resize ((int) (0.07 * sr) + 64);
        rateSm.reset (sr, 80.0, 0.5f);
        depthSm.reset (sr, 80.0, 0.0f);
        delaySm.reset (sr, 120.0, (float) (0.01 * sr));
        mixSm.reset (sr, 30.0, 0.5f);
        outSm.reset (sr, 30.0, 1.0f);
        fbSm.reset (sr, 30.0, 0.0f);
        reset();
        first = true;
        update();
    }

    void reset() noexcept
    {
        for (auto& l : lines)
            l.clear();
        for (auto& f : wetHp) f.reset();
        for (auto& f : toneLp) f.reset();
        for (auto& f : warmLp) f.reset();
        phase = fastPhase = 0.0f;
        randPhase.fill (0.0f);
        randValue.fill (0.0f);
        randTarget.fill (0.0f);
        fbState = { 0.0f, 0.0f };
    }

    void setParams (const ChorusParams& p) noexcept
    {
        params = p;
        update();
    }

    // Effective rate (Hz), depth (ms) and delay (ms) after the mode is applied, for display.
    void getEffective (float& rateHz, float& depthMs, float& delayMs) const noexcept
    {
        rateHz = effRate;
        depthMs = effDepthMs;
        delayMs = effDelayMs;
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const auto& td = typeData (params.type);
        const bool stereo = right != nullptr;
        const float drive = 1.0f + 2.5f * params.warmth;

        for (int n = 0; n < numSamples; ++n)
        {
            const float dryL = left[n];
            const float dryR = stereo ? right[n] : dryL;
            const float rate = rateSm.next();
            const float depth = depthSm.next();
            const float baseDelay = delaySm.next();
            const float fb = fbSm.next();

            phase += rate * invSr;
            if (phase >= 1.0f) phase -= 1.0f;
            fastPhase += rate * td.fastLfoRatio * invSr;
            if (fastPhase >= 1.0f) fastPhase -= 1.0f;

            const float inL = dryL + fb * fbState[0];
            const float inR = dryR + fb * fbState[1];

            float wetL = 0.0f, wetR = 0.0f;
            for (int v = 0; v < td.numVoices; ++v)
            {
                const auto& voice = td.voices[v];
                float lfo = lfoValue (v, voice.phase, voice.rateMul, rate);
                if (td.fastLfoDepth > 0.0f)
                {
                    float fp = fastPhase + voice.phase;
                    fp -= std::floor (fp);
                    lfo += td.fastLfoDepth * std::sin ((float) kTwoPi * fp);
                }
                const float d = std::max (3.0f, baseDelay + voice.delayOffsetMs * msToSamples + depth * lfo);
                const float y = lines[(size_t) v].readCubic (d);
                lines[(size_t) v].write (inL + (inR - inL) * voice.source);
                wetL += y * voice.outL;
                wetR += y * voice.outR;
            }

            // Wet tone: keep the low end out of the modulation, then soften the top the
            // way a bucket-brigade line does.
            wetL = wetHp[0].highpass (wetL);
            wetR = wetHp[1].highpass (wetR);
            wetL = toneLp[0].lowpass (wetL);
            wetR = toneLp[1].lowpass (wetR);
            if (params.warmth > 0.01f)
            {
                wetL = warmLp[0].lowpass (std::tanh (wetL * drive) / drive);
                wetR = warmLp[1].lowpass (std::tanh (wetR * drive) / drive);
            }
            fbState[0] = clampv (wetL, -2.0f, 2.0f);
            fbState[1] = clampv (wetR, -2.0f, 2.0f);

            const float mid = 0.5f * (wetL + wetR), side = 0.5f * (wetL - wetR) * params.width;
            wetL = mid + side;
            wetR = mid - side;

            const float mx = mixSm.next();
            const float dryGain = std::cos (mx * 0.5f * (float) kPi);
            const float wetGain = std::sin (mx * 0.5f * (float) kPi);
            const float og = outSm.next();
            if (stereo)
            {
                left[n] = (dryL * dryGain + wetL * wetGain) * og;
                right[n] = (dryR * dryGain + wetR * wetGain) * og;
            }
            else
            {
                left[n] = (dryL * dryGain + mid * wetGain) * og;
            }
        }
    }

private:
    float lfoValue (int v, float offset, float rateMul, float rate) noexcept
    {
        if (params.shape == ChorusShape::Random)
        {
            // Smoothed random steps, one new target per cycle.
            float& p = randPhase[(size_t) v];
            p += rate * rateMul * invSr;
            if (p >= 1.0f)
            {
                p -= 1.0f;
                randTarget[(size_t) v] = rng.nextBipolar();
            }
            const float coef = clampv (rate * rateMul * invSr * 6.0f, 0.0f, 1.0f);
            randValue[(size_t) v] += (randTarget[(size_t) v] - randValue[(size_t) v]) * coef;
            return randValue[(size_t) v];
        }

        float p = phase * rateMul + offset;
        p -= std::floor (p);
        if (params.shape == ChorusShape::Sine)
            return std::sin ((float) kTwoPi * p);
        // Triangle with slightly rounded corners, as an analog LFO produces.
        const float tri = 4.0f * std::abs (p - 0.5f) - 1.0f;
        return tri * (1.12f - 0.12f * tri * tri);
    }

    void update() noexcept
    {
        const auto& td = typeData (params.type);
        invSr = (float) (1.0 / sr);
        msToSamples = (float) (0.001 * sr);

        if (params.mode == ChorusMode::Manual)
        {
            effRate = clampv (params.rateHz, 0.02f, 12.0f);
            effDelayMs = clampv (params.delayMs, 1.0f, 30.0f);
            effDepthMs = clampv (params.depth, 0.0f, 1.0f) * std::min (effDelayMs * 0.9f, 6.0f);
        }
        else
        {
            const int m = clampv ((int) params.mode, 0, 3);
            effRate = td.presetRate[m];
            effDepthMs = td.presetDepthMs[m];
            effDelayMs = td.presetDelayMs[m];
        }

        rateSm.setTarget (effRate);
        depthSm.setTarget (effDepthMs * msToSamples);
        delaySm.setTarget (effDelayMs * msToSamples);
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));
        outSm.setTarget (dbToGain (params.outputDb));
        fbSm.setTarget (clampv (params.feedback, -0.9f, 0.9f));

        for (int c = 0; c < 2; ++c)
        {
            wetHp[(size_t) c].setCutoff (td.wetHighpassHz, sr);
            toneLp[(size_t) c].setCutoff (clampv ((double) params.toneHz, 500.0, sr * 0.45), sr);
            warmLp[(size_t) c].setCutoff (clampv (18000.0 - 12500.0 * params.warmth, 3000.0, sr * 0.45), sr);
        }

        if (first)
        {
            rateSm.snap();
            depthSm.snap();
            delaySm.snap();
            mixSm.snap();
            outSm.snap();
            fbSm.snap();
            first = false;
        }
    }

    double sr = 44100.0;
    ChorusParams params;
    bool first = true;
    std::array<DelayLine, kMaxVoices> lines;
    std::array<OnePole, 2> wetHp, toneLp, warmLp;
    std::array<float, kMaxVoices> randPhase {}, randValue {}, randTarget {};
    std::array<float, 2> fbState { 0.0f, 0.0f };
    float phase = 0.0f, fastPhase = 0.0f, invSr = 1.0f / 44100.0f, msToSamples = 44.1f;
    float effRate = 0.5f, effDepthMs = 1.0f, effDelayMs = 10.0f;
    Smoothed rateSm, depthSm, delaySm, mixSm, outSm, fbSm;
    Random rng { 0xABCDEF01u };
};
} // namespace gitto
