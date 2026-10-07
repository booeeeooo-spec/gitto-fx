// Gitto FX Reverb - algorithmic reverb built on an 8-line feedback delay network.
#pragma once

#include "Common.h"

namespace gitto
{
enum class ReverbMode
{
    ConcertHall = 0, BrightHall, Plate, Room, Chamber, Ambience, Cathedral,
    RandomHall, ChorusHall, DarkChamber, LoFiHall, Gated, Count
};
enum class ReverbColor { Vintage = 0, Warm, Modern, Count };

struct ReverbParams
{
    ReverbMode mode = ReverbMode::ConcertHall;
    ReverbColor color = ReverbColor::Modern;
    float mix = 0.3f;            // 0..1
    float predelayMs = 20.0f;    // 0..500
    float decaySec = 2.0f;       // 0.2..60
    float size = 0.5f;           // 0..1
    float early = 0.5f;          // early reflection level 0..1
    float bassMult = 1.2f;       // 0.25..4
    float bassXoverHz = 400.0f;  // 100..2000
    float highMult = 0.5f;       // 0.1..1
    float dampHz = 6000.0f;      // 1000..20000
    float earlyDiff = 0.7f;      // 0..1
    float lateDiff = 0.6f;       // 0..1
    float modRateHz = 0.5f;      // 0.05..5
    float modDepth = 0.3f;       // 0..1
    float lowCutHz = 20.0f;      // 20..1000
    float highCutHz = 20000.0f;  // 1000..20000
    float width = 1.0f;          // 0..2
    bool freeze = false;
};

class Reverb
{
public:
    static constexpr int kLines = 8;
    static constexpr int kEarlyTaps = 8;
    static constexpr int kInputAllpasses = 4;

    struct ModeData
    {
        float lineMs[kLines];
        float earlyMs[kEarlyTaps];
        float earlyGain[kEarlyTaps];
        float inputDiffusion;   // scales the Early Diffusion control
        float loopDiffusion;    // scales the Late Diffusion control
        float modScale;         // modulation depth multiplier
        bool randomMod;         // random drift instead of sine
        float earlyLevel;       // mode's own early reflection weight
        float brightness;       // multiplies the damping frequency
        float sizeScale;        // overall scale applied to all times
    };

    static const ModeData& modeData (ReverbMode m) noexcept
    {
        static const ModeData data[(int) ReverbMode::Count] = {
            // Concert Hall
            { { 43.7f, 47.9f, 53.3f, 59.9f, 67.1f, 73.3f, 79.7f, 89.3f },
              { 11.3f, 17.9f, 23.1f, 31.7f, 38.3f, 47.1f, 58.9f, 71.3f },
              { 0.85f, 0.72f, 0.66f, 0.55f, 0.48f, 0.40f, 0.31f, 0.24f }, 1.0f, 1.0f, 1.0f, false, 0.7f, 1.0f, 1.0f },
            // Bright Hall
            { { 39.1f, 44.3f, 49.7f, 55.1f, 61.3f, 68.9f, 74.9f, 83.1f },
              { 9.7f, 15.1f, 21.7f, 28.9f, 35.3f, 43.7f, 52.1f, 63.7f },
              { 0.88f, 0.76f, 0.69f, 0.58f, 0.50f, 0.42f, 0.33f, 0.26f }, 0.95f, 0.9f, 0.9f, false, 0.75f, 1.6f, 0.95f },
            // Plate
            { { 13.1f, 15.7f, 17.9f, 20.3f, 23.3f, 25.7f, 28.1f, 31.9f },
              { 1.3f, 2.9f, 4.1f, 5.9f, 7.3f, 9.1f, 11.3f, 13.7f },
              { 0.5f, 0.5f, 0.45f, 0.45f, 0.4f, 0.4f, 0.35f, 0.3f }, 1.15f, 1.2f, 0.6f, false, 0.25f, 1.4f, 1.0f },
            // Room
            { { 11.3f, 13.9f, 16.7f, 19.1f, 22.3f, 25.1f, 27.7f, 30.7f },
              { 3.1f, 5.3f, 8.9f, 12.7f, 16.1f, 20.3f, 24.7f, 29.9f },
              { 0.95f, 0.85f, 0.74f, 0.66f, 0.55f, 0.47f, 0.38f, 0.30f }, 0.85f, 0.8f, 0.5f, false, 1.0f, 1.1f, 1.0f },
            // Chamber
            { { 21.1f, 24.7f, 27.1f, 30.7f, 34.3f, 37.9f, 41.3f, 45.7f },
              { 5.9f, 9.7f, 14.3f, 19.1f, 24.1f, 30.1f, 36.7f, 43.3f },
              { 0.9f, 0.8f, 0.7f, 0.62f, 0.52f, 0.44f, 0.36f, 0.28f }, 1.0f, 1.0f, 0.7f, false, 0.8f, 1.0f, 1.0f },
            // Ambience
            { { 5.3f, 6.7f, 7.9f, 9.1f, 10.7f, 12.1f, 13.3f, 14.9f },
              { 2.3f, 4.7f, 7.1f, 10.3f, 13.9f, 17.3f, 21.1f, 25.3f },
              { 1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f }, 0.8f, 0.7f, 0.4f, false, 1.2f, 1.2f, 1.0f },
            // Cathedral
            { { 61.3f, 67.9f, 74.3f, 81.7f, 89.9f, 97.3f, 105.1f, 113.9f },
              { 19.7f, 29.3f, 41.9f, 53.3f, 67.1f, 79.9f, 94.3f, 109.7f },
              { 0.7f, 0.62f, 0.55f, 0.48f, 0.42f, 0.36f, 0.30f, 0.24f }, 1.0f, 1.0f, 1.0f, false, 0.5f, 0.85f, 1.15f },
            // Random Hall
            { { 41.9f, 46.1f, 52.3f, 58.1f, 65.3f, 71.9f, 78.7f, 87.1f },
              { 10.7f, 16.9f, 22.3f, 30.1f, 37.3f, 46.3f, 57.1f, 69.7f },
              { 0.85f, 0.72f, 0.66f, 0.55f, 0.48f, 0.40f, 0.31f, 0.24f }, 1.0f, 1.0f, 2.2f, true, 0.6f, 1.0f, 1.0f },
            // Chorus Hall
            { { 37.3f, 42.1f, 47.3f, 53.9f, 60.7f, 66.1f, 72.7f, 81.1f },
              { 10.1f, 15.7f, 22.9f, 29.3f, 36.1f, 45.1f, 55.7f, 67.9f },
              { 0.8f, 0.7f, 0.62f, 0.52f, 0.45f, 0.38f, 0.30f, 0.22f }, 1.0f, 0.9f, 3.5f, false, 0.5f, 1.0f, 1.0f },
            // Dark Chamber
            { { 23.3f, 26.9f, 29.9f, 33.7f, 37.1f, 40.9f, 44.3f, 49.1f },
              { 6.7f, 10.9f, 15.7f, 20.9f, 26.3f, 32.3f, 38.9f, 46.1f },
              { 0.9f, 0.8f, 0.7f, 0.62f, 0.52f, 0.44f, 0.36f, 0.28f }, 1.0f, 1.0f, 0.8f, false, 0.7f, 0.45f, 1.0f },
            // Lo-Fi Hall
            { { 40.3f, 45.7f, 51.1f, 57.7f, 64.3f, 70.9f, 77.3f, 85.9f },
              { 12.1f, 18.7f, 24.7f, 32.9f, 40.1f, 49.3f, 60.1f, 72.7f },
              { 0.85f, 0.72f, 0.66f, 0.55f, 0.48f, 0.40f, 0.31f, 0.24f }, 0.9f, 0.85f, 1.6f, true, 0.6f, 0.7f, 1.0f },
            // Gated
            { { 9.7f, 11.9f, 14.3f, 16.9f, 19.3f, 22.1f, 24.9f, 27.7f },
              { 2.1f, 4.3f, 6.7f, 9.7f, 12.3f, 15.1f, 18.7f, 22.3f },
              { 0.9f, 0.9f, 0.85f, 0.85f, 0.8f, 0.8f, 0.75f, 0.7f }, 1.15f, 1.2f, 0.5f, false, 0.6f, 1.3f, 1.0f },
        };
        return data[clampv ((int) m, 0, (int) ReverbMode::Count - 1)];
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        const int maxLine = (int) (0.42 * sr) + 64;
        for (auto& l : lines)
            l.resize (maxLine);
        for (auto& a : loopAp)
            a.resize ((int) (0.16 * sr) + 64);
        for (auto& ch : inputAp)
            for (auto& a : ch)
                a.resize ((int) (0.05 * sr) + 64);
        for (auto& p : predelay)
            p.resize ((int) (0.78 * sr) + 64);

        mixSm.reset (sr, 30.0, 0.3f);
        predelaySm.reset (sr, 60.0, 0.0f);
        sizeSm.reset (sr, 120.0, 1.0f);
        lfoInc = 0.0f;
        for (int i = 0; i < kLines; ++i)
            lfoPhase[(size_t) i] = (float) i / (float) kLines;
        gateOpenCoef = onePoleCoef (2.0, sr);
        gateCloseCoef = onePoleCoef (25.0, sr);
        envFastCoef = onePoleCoef (3.0, sr);
        envSlowCoef = onePoleCoef (120.0, sr);
        reset();
        firstUpdate = true;
        update();
    }

    void reset() noexcept
    {
        for (auto& l : lines)
            l.clear();
        for (auto& a : loopAp)
            a.clear();
        for (auto& ch : inputAp)
            for (auto& a : ch)
                a.clear();
        for (auto& p : predelay)
            p.clear();
        lowState.fill (0.0f);
        highState.fill (0.0f);
        randValue.fill (0.0f);
        randTarget.fill (0.0f);
        for (auto& f : wetFilters)
            for (auto& b : f)
                b.reset();
        for (auto& b : colorFilter)
            b.reset();
        gateGain = 0.0f;
        gateTimer = 0;
        envFast = envSlow = 0.0f;
        holdL = holdR = 0.0f;
        holdPhase = 0.0f;
    }

    void setParams (const ReverbParams& p) noexcept
    {
        params = p;
        update();
    }

    // right may be nullptr for mono in / mono out.
    void process (float* left, float* right, int numSamples) noexcept
    {
        const auto& md = modeData (params.mode);
        const bool stereo = right != nullptr;
        const bool gated = params.mode == ReverbMode::Gated;
        const bool lofi = params.mode == ReverbMode::LoFiHall;
        const float inGain = params.freeze ? 0.0f : 1.0f;
        const float earlyLevel = params.early * md.earlyLevel;

        for (int n = 0; n < numSamples; ++n)
        {
            const float dryL = left[n];
            const float dryR = stereo ? right[n] : dryL;

            // --- pre-delay ---------------------------------------------------------
            predelay[0].write (dryL);
            predelay[1].write (dryR);
            const float pd = predelaySm.next();
            const float size = sizeSm.next();
            float inL = predelay[0].readLinear (pd + 1.0f) * inGain;
            float inR = predelay[1].readLinear (pd + 1.0f) * inGain;

            // --- early reflections -------------------------------------------------
            float earlyL = 0.0f, earlyR = 0.0f;
            if (earlyLevel > 0.0f)
            {
                for (int t = 0; t < kEarlyTaps; ++t)
                {
                    // Left and right use interleaved tap sets so the image is wide.
                    const float dL = pd + earlySamples[(size_t) t] * size + 1.0f;
                    const float dR = pd + earlySamples[(size_t) ((t + 3) % kEarlyTaps)] * size * 1.07f + 1.0f;
                    const float sign = (t & 1) ? -1.0f : 1.0f;
                    earlyL += predelay[0].readLinear (dL) * md.earlyGain[t] * sign;
                    earlyR += predelay[1].readLinear (dR) * md.earlyGain[(t + 3) % kEarlyTaps] * sign;
                }
                earlyL *= 0.35f * earlyLevel * inGain;
                earlyR *= 0.35f * earlyLevel * inGain;
            }

            // --- input diffusion ---------------------------------------------------
            for (int a = 0; a < kInputAllpasses; ++a)
            {
                inL = allpass (inputAp[0][(size_t) a], inL, inputApSamples[0][(size_t) a], inputCoef);
                inR = allpass (inputAp[1][(size_t) a], inR, inputApSamples[1][(size_t) a], inputCoef);
            }

            // --- feedback delay network --------------------------------------------
            const bool moving = modSamples > 0.01f || sizeSm.isSmoothing();
            float out[kLines];
            for (int i = 0; i < kLines; ++i)
            {
                float mod;
                if (md.randomMod)
                {
                    // Slow random drift: a new target every LFO cycle, heavily smoothed.
                    lfoPhase[(size_t) i] += lfoInc * (0.8f + 0.05f * (float) i);
                    if (lfoPhase[(size_t) i] >= 1.0f)
                    {
                        lfoPhase[(size_t) i] -= 1.0f;
                        randTarget[(size_t) i] = rng.nextBipolar();
                    }
                    randValue[(size_t) i] += (randTarget[(size_t) i] - randValue[(size_t) i]) * randSmooth;
                    mod = randValue[(size_t) i];
                }
                else
                {
                    lfoPhase[(size_t) i] += lfoInc * (1.0f + 0.07f * (float) i);
                    if (lfoPhase[(size_t) i] >= 1.0f)
                        lfoPhase[(size_t) i] -= 1.0f;
                    mod = fastSin (lfoPhase[(size_t) i]);
                }
                const float d = lineSamples[(size_t) i] * size + modSamples * mod + 3.0f;
                // Interpolate only while the delay is actually moving, so a still network
                // is lossless and the decay time is exactly what was asked for.
                out[i] = moving ? lines[(size_t) i].readCubic (d) : lines[(size_t) i].readInt ((int) (d + 0.5f));
            }

            // Frequency-dependent decay per line.
            float fb[kLines];
            for (int i = 0; i < kLines; ++i)
            {
                float y = out[i] * gMid[(size_t) i];
                float& ls = lowState[(size_t) i];
                ls = y + (ls - y) * lowCoef;
                y += (kLow[(size_t) i] - 1.0f) * ls;
                float& hs = highState[(size_t) i];
                hs = y + (hs - y) * highCoef;
                y = hs + kHigh[(size_t) i] * (y - hs);
                fb[i] = y;
            }

            // Hadamard mix (fast Walsh-Hadamard transform, energy preserving).
            for (int half = 1; half < kLines; half <<= 1)
                for (int i = 0; i < kLines; i += half << 1)
                    for (int j = i; j < i + half; ++j)
                    {
                        const float a = fb[j], b = fb[j + half];
                        fb[j] = a + b;
                        fb[j + half] = a - b;
                    }
            const float norm = 0.35355339f; // 1/sqrt(8)

            for (int i = 0; i < kLines; ++i)
            {
                float x = fb[i] * norm + ((i & 1) ? inR : inL) * 0.5f;
                x = allpass (loopAp[(size_t) i], x, loopApSamples[(size_t) i] * size, loopCoef);
                lines[(size_t) i].write (undenorm (x));
            }

            float lateL = out[0] - out[2] + out[4] - out[6] + 0.5f * (out[1] - out[5]);
            float lateR = out[1] - out[3] + out[5] - out[7] + 0.5f * (out[0] - out[4]);
            lateL *= lateGain;
            lateR *= lateGain;

            if (gated)
            {
                // Open on each new hit, hold for the gate time, then shut quickly.
                const float lvl = std::max (std::abs (dryL), std::abs (dryR));
                envFast = lvl + (envFast - lvl) * envFastCoef;
                envSlow = lvl + (envSlow - lvl) * envSlowCoef;
                if (envFast > 1.6f * envSlow + 1.0e-4f)
                    gateTimer = gateSamples;
                const float target = gateTimer > 0 ? 1.0f : 0.0f;
                if (gateTimer > 0)
                    --gateTimer;
                gateGain = target + (gateGain - target) * (target > gateGain ? gateOpenCoef : gateCloseCoef);
                lateL *= gateGain;
                lateR *= gateGain;
            }

            float wetL = (lateL + earlyL) * modeTrim;
            float wetR = (lateR + earlyR) * modeTrim;

            if (lofi)
            {
                // Early digital reverbs ran at low sample rates: hold and quantise the output.
                holdPhase += lofiInc;
                if (holdPhase >= 1.0f)
                {
                    holdPhase -= 1.0f;
                    holdL = std::round (wetL * 2048.0f) * (1.0f / 2048.0f);
                    holdR = std::round (wetR * 2048.0f) * (1.0f / 2048.0f);
                }
                wetL = holdL;
                wetR = holdR;
            }

            // --- wet tone and width ------------------------------------------------
            if (colorActive)
            {
                wetL = colorFilter[0].process (wetL);
                wetR = colorFilter[1].process (wetR);
            }
            if (lowCutActive)
            {
                wetL = wetFilters[0][0].process (wetL);
                wetR = wetFilters[1][0].process (wetR);
            }
            if (highCutActive)
            {
                wetL = wetFilters[0][1].process (wetL);
                wetR = wetFilters[1][1].process (wetR);
            }

            const float mid = 0.5f * (wetL + wetR), side = 0.5f * (wetL - wetR) * params.width;
            wetL = mid + side;
            wetR = mid - side;

            const float mx = mixSm.next();
            const float dryGain = std::cos (mx * 0.5f * (float) kPi);
            const float wetGain = std::sin (mx * 0.5f * (float) kPi);
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

        for (auto& f : wetFilters)
            for (auto& b : f)
                b.sanitise();
        for (auto& b : colorFilter)
            b.sanitise();
        for (auto& s : lowState)
            s = undenorm (s);
        for (auto& s : highState)
            s = undenorm (s);
    }

private:
    static float fastSin (float phase01) noexcept
    {
        // Parabolic sine approximation, plenty for an LFO.
        const float x = phase01 * 2.0f - 1.0f; // -1..1
        const float y = 4.0f * x * (1.0f - std::abs (x));
        return y * (0.775f + 0.225f * std::abs (y));
    }

    // Schroeder allpass. The delay is a whole number of samples: interpolating here
    // would shave high frequencies on every pass round the loop.
    static float allpass (DelayLine& d, float x, float delaySamples, float g) noexcept
    {
        const float delayed = d.readInt (std::max (1, (int) (delaySamples + 0.5f)));
        const float v = x - g * delayed;
        d.write (undenorm (v));
        return delayed + g * v;
    }

    void update() noexcept
    {
        const auto& md = modeData (params.mode);
        const double scale = md.sizeScale;

        for (int i = 0; i < kLines; ++i)
        {
            lineSamples[(size_t) i] = (float) (md.lineMs[i] * scale * 0.001 * sr);
            loopApSamples[(size_t) i] = (float) (md.lineMs[(i * 3 + 5) % kLines] * 0.31 * scale * 0.001 * sr);
        }
        for (int t = 0; t < kEarlyTaps; ++t)
            earlySamples[(size_t) t] = (float) (md.earlyMs[t] * scale * 0.001 * sr);

        static const double apMs[2][kInputAllpasses] = { { 4.77, 3.59, 12.73, 9.31 }, { 5.11, 3.83, 11.89, 8.69 } };
        for (int c = 0; c < 2; ++c)
            for (int a = 0; a < kInputAllpasses; ++a)
                inputApSamples[(size_t) c][(size_t) a] = (float) (apMs[c][a] * 0.001 * sr);

        // Size 0..1 maps to 0.4x..1.6x of the mode's nominal dimensions.
        const float sizeScale = 0.4f + 1.2f * clampv (params.size, 0.0f, 1.0f);
        sizeSm.setTarget (sizeScale);
        predelaySm.setTarget ((float) (clampv (params.predelayMs, 0.0f, 500.0f) * 0.001 * sr));
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));

        inputCoef = clampv (params.earlyDiff * 0.72f * md.inputDiffusion, 0.0f, 0.85f);
        loopCoef = clampv (params.lateDiff * 0.6f * md.loopDiffusion, 0.0f, 0.8f);

        // Decay: each line loses exactly enough per pass to reach -60 dB after decaySec.
        const double rt = params.mode == ReverbMode::Gated ? 4.0 : clampv ((double) params.decaySec, 0.1, 100.0);
        const double rtLow = rt * clampv ((double) params.bassMult, 0.25, 4.0);
        const double rtHigh = rt * clampv ((double) params.highMult, 0.05, 1.0);
        for (int i = 0; i < kLines; ++i)
        {
            const double len = (lineSamples[(size_t) i] + loopApSamples[(size_t) i]) * sizeScale;
            double gm = std::pow (10.0, -3.0 * len / (rt * sr));
            double gl = std::pow (10.0, -3.0 * len / (rtLow * sr));
            double gh = std::pow (10.0, -3.0 * len / (rtHigh * sr));
            if (params.freeze)
                gm = gl = gh = 1.0;
            gMid[(size_t) i] = (float) gm;
            kLow[(size_t) i] = (float) (gl / gm);
            kHigh[(size_t) i] = (float) (gh / gm);
        }
        lowCoef = (float) std::exp (-kTwoPi * clampv ((double) params.bassXoverHz, 50.0, 4000.0) / sr);
        const double damp = clampv ((double) params.dampHz * md.brightness, 500.0, sr * 0.45);
        highCoef = (float) std::exp (-kTwoPi * damp / sr);

        // Per-mode level trim (dB), measured so every mode sits at a similar loudness.
        static const float trimDb[(int) ReverbMode::Count] = { 0.7f, 0.0f, -2.1f, -1.3f, -1.6f, -5.5f, 3.4f, 1.4f, 0.9f, -1.4f, 1.3f, 0.5f };
        modeTrim = dbToGain (trimDb[clampv ((int) params.mode, 0, (int) ReverbMode::Count - 1)]);

        // Louder for short decays, quieter for long ones, so Mix behaves consistently.
        lateGain = (float) (0.42 / std::sqrt (clampv (rt, 0.3, 12.0)) * 1.4);

        lfoInc = (float) (clampv ((double) params.modRateHz, 0.01, 10.0) / sr);
        modSamples = (float) (params.modDepth * md.modScale * 0.00045 * sr);
        randSmooth = 1.0f - (float) std::exp (-kTwoPi * clampv ((double) params.modRateHz * 1.5, 0.05, 20.0) / sr);

        gateSamples = (int) (clampv ((double) params.decaySec, 0.05, 2.0) * 0.25 * sr);
        lofiInc = (float) (11000.0 / sr);

        lowCutActive = params.lowCutHz > 21.0f;
        highCutActive = params.highCutHz < 19900.0f;
        const auto hp = design::highpass (params.lowCutHz, 0.7071, sr);
        const auto lp = design::lowpass (std::min ((double) params.highCutHz, sr * 0.48), 0.7071, sr);
        for (int c = 0; c < 2; ++c)
        {
            wetFilters[(size_t) c][0].setCoefs (hp);
            wetFilters[(size_t) c][1].setCoefs (lp);
        }

        colorActive = params.color != ReverbColor::Modern;
        if (colorActive)
        {
            const double cutoff = params.color == ReverbColor::Vintage ? 6500.0 : 11000.0;
            const auto c = design::lowpass (std::min (cutoff, sr * 0.45), 0.6, sr);
            colorFilter[0].setCoefs (c);
            colorFilter[1].setCoefs (c);
        }

        if (firstUpdate)
        {
            sizeSm.snap();
            predelaySm.snap();
            mixSm.snap();
            firstUpdate = false;
        }
    }

    double sr = 44100.0;
    ReverbParams params;
    bool firstUpdate = true;

    std::array<DelayLine, kLines> lines, loopAp;
    std::array<std::array<DelayLine, kInputAllpasses>, 2> inputAp;
    std::array<DelayLine, 2> predelay;

    std::array<float, kLines> lineSamples {}, loopApSamples {}, gMid {}, kLow {}, kHigh {};
    std::array<float, kLines> lowState {}, highState {}, lfoPhase {}, randValue {}, randTarget {};
    std::array<float, kEarlyTaps> earlySamples {};
    std::array<std::array<float, kInputAllpasses>, 2> inputApSamples {};

    float inputCoef = 0.5f, loopCoef = 0.4f, lowCoef = 0.0f, highCoef = 0.0f;
    float lateGain = 0.3f, modeTrim = 1.0f, lfoInc = 0.0f, modSamples = 0.0f, randSmooth = 0.001f;
    float gateGain = 0.0f, gateOpenCoef = 0.0f, gateCloseCoef = 0.0f, envFast = 0.0f, envSlow = 0.0f;
    float envFastCoef = 0.0f, envSlowCoef = 0.0f;
    int gateTimer = 0, gateSamples = 0;
    float holdL = 0.0f, holdR = 0.0f, holdPhase = 0.0f, lofiInc = 0.25f;
    bool lowCutActive = false, highCutActive = false, colorActive = false;

    std::array<std::array<Biquad, 2>, 2> wetFilters;
    std::array<Biquad, 2> colorFilter;
    Smoothed mixSm, predelaySm, sizeSm;
    Random rng { 0xC0FFEEu };
};
} // namespace gitto
