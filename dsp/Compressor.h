// Gitto FX Compressor - six-style stereo compressor with lookahead and sidechain.
#pragma once

#include "Common.h"
#include <atomic>

namespace gitto
{
enum class CompStyle { Clean = 0, Punch, Glue, Opto, VariMu, Fet, Count };

struct CompParams
{
    CompStyle style = CompStyle::Clean;
    float thresholdDb = -18.0f; // -60..0
    float ratio = 4.0f;         // 1..20, 20 behaves as a limiter
    float kneeDb = 6.0f;        // 0..36
    float attackMs = 10.0f;     // 0.01..250
    float releaseMs = 120.0f;   // 5..2500
    bool autoRelease = false;
    float lookaheadMs = 0.0f;   // 0..20
    float holdMs = 0.0f;        // 0..500
    float rangeDb = 60.0f;      // max gain reduction, 0..60
    float makeupDb = 0.0f;      // -12..+36
    bool autoMakeup = false;
    float mix = 1.0f;           // 0..1
    float scHpfHz = 20.0f;      // 20..500 (20 = off)
    float stereoLink = 1.0f;    // 0..1
    bool externalSidechain = false;
};

class Compressor
{
public:
    static constexpr double kMaxLookaheadMs = 20.0;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        const int maxLook = (int) std::ceil (kMaxLookaheadMs * 0.001 * sr) + 4;
        for (auto& d : delay)
            d.resize (maxLook);
        makeup.reset (sr, 30.0, 1.0f);
        mixSm.reset (sr, 30.0, 1.0f);
        inMeter.prepare (sr);
        outMeter.prepare (sr);
        reset();
        updateDerived();
        // Start on the current settings instead of gliding to them.
        makeup.snap();
        mixSm.snap();
    }

    void reset() noexcept
    {
        for (auto& d : delay)
            d.clear();
        for (auto& h : scHpf)
            h.reset();
        for (auto& b : dcBlock)
            b.reset();
        gr = { 0.0f, 0.0f };
        slowGr = { 0.0f, 0.0f };
        rmsState = { 0.0f, 0.0f };
        peakEnv = { 0.0f, 0.0f };
        holdCount = { 0, 0 };
        lastOut = { 0.0f, 0.0f };
    }

    void setParams (const CompParams& p) noexcept
    {
        params = p;
        updateDerived();
    }

    int getLatencySamples() const noexcept { return lookaheadSamples; }

    // Static transfer curve: gain reduction (dB, <= 0) for a detector level in dB.
    float gainReductionFor (float levelDb) const noexcept
    {
        const float T = params.thresholdDb;
        float knee = params.kneeDb;
        float slope = 1.0f - 1.0f / std::max (1.0f, params.ratio); // 0..1
        if (params.ratio >= 19.9f)
            slope = 1.0f;

        float over = levelDb - T;
        float out;
        if (style == CompStyle::VariMu)
        {
            // Ratio grows with level: gentle at the threshold, firm well above it.
            knee = std::max (knee, 12.0f) + 12.0f;
        }
        else if (style == CompStyle::Opto)
        {
            knee = std::max (knee, 9.0f);
        }

        if (2.0f * over < -knee)
            out = 0.0f;
        else if (knee > 0.0f && 2.0f * std::abs (over) <= knee)
            out = -slope * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);
        else
            out = -slope * over;

        return std::max (out, -params.rangeDb);
    }

    // sideL / sideR may be nullptr (uses the main input). right may be nullptr for mono.
    void process (float* left, float* right, const float* sideL, const float* sideR, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        const bool useExt = params.externalSidechain && sideL != nullptr;
        const bool feedback = style == CompStyle::Glue || style == CompStyle::Fet;

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = left[i];
            const float inR = stereo ? right[i] : inL;
            inMeter.push (std::max (std::abs (inL), std::abs (inR)));

            // --- detector input -------------------------------------------------
            float dL, dR;
            if (useExt)
            {
                dL = sideL[i];
                dR = sideR != nullptr ? sideR[i] : dL;
            }
            else if (feedback)
            {
                dL = lastOut[0];
                dR = lastOut[1];
            }
            else
            {
                dL = inL;
                dR = inR;
            }

            if (hpfActive)
            {
                dL = scHpf[0].process (dL);
                dR = scHpf[1].process (dR);
            }

            float lvl[2] = { std::abs (dL), std::abs (dR) };

            if (rmsCoef > 0.0f)
            {
                for (int c = 0; c < 2; ++c)
                {
                    rmsState[(size_t) c] = lvl[c] * lvl[c] + (rmsState[(size_t) c] - lvl[c] * lvl[c]) * rmsCoef;
                    lvl[c] = std::sqrt (rmsState[(size_t) c]);
                }
            }
            else
            {
                // Peak detector with a short hold-off, so the level does not collapse to
                // zero between the half-cycles of low notes.
                for (int c = 0; c < 2; ++c)
                {
                    peakEnv[(size_t) c] = std::max (lvl[c], peakEnv[(size_t) c] * peakDecay);
                    lvl[c] = peakEnv[(size_t) c];
                }
            }

            const float linked = std::max (lvl[0], lvl[1]);
            lvl[0] += (linked - lvl[0]) * params.stereoLink;
            lvl[1] += (linked - lvl[1]) * params.stereoLink;

            // --- gain computer + ballistics ------------------------------------
            float gainLin[2];
            for (int c = 0; c < (stereo ? 2 : 1); ++c)
            {
                const float levelDb = gainToDb (lvl[c]);
                float target;
                if (feedback && ! useExt)
                {
                    // The detector sees the output, so the curve is solved for output level.
                    const float over = levelDb - params.thresholdDb;
                    const float k = fbSlope;
                    float t = 0.0f;
                    const float knee = params.kneeDb;
                    if (2.0f * over < -knee)
                        t = 0.0f;
                    else if (knee > 0.0f && 2.0f * std::abs (over) <= knee)
                        t = -k * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);
                    else
                        t = -k * over;
                    target = std::max (t, -params.rangeDb);
                }
                else
                {
                    target = gainReductionFor (levelDb);
                }

                float& g = gr[(size_t) c];
                if (target < g)
                {
                    float a = attackCoef;
                    if (style == CompStyle::Punch)
                    {
                        // Slower onset for the first few dB keeps the front of the transient.
                        const float depth = clampv ((g - target) / 12.0f, 0.0f, 1.0f);
                        a = lerp (punchSlowCoef, attackCoef, depth);
                    }
                    g = target + (g - target) * a;
                    holdCount[(size_t) c] = holdSamples;
                }
                else if (holdCount[(size_t) c] > 0)
                {
                    --holdCount[(size_t) c];
                }
                else
                {
                    float r = releaseCoef;
                    if (programRelease)
                    {
                        // Two-stage release: the first part recovers quickly, the remainder
                        // depends on how hard and how long the compressor has been working.
                        float& s = slowGr[(size_t) c];
                        s = g < s ? g + (s - g) * slowAttackCoef : g + (s - g) * slowReleaseCoef;
                        const float sustained = clampv (-s / 10.0f, 0.0f, 1.0f);
                        r = lerp (fastReleaseCoef, slowestReleaseCoef, sustained);
                    }
                    g = target + (g - target) * r;
                }
                g = undenorm (g);
                gainLin[c] = dbToGain (g);
            }
            if (! stereo)
                gainLin[1] = gainLin[0];

            // --- audio path -----------------------------------------------------
            float xL = inL, xR = inR;
            if (lookaheadSamples > 0)
            {
                delay[0].write (inL);
                delay[1].write (inR);
                xL = delay[0].readInt (lookaheadSamples + 1);
                xR = delay[1].readInt (lookaheadSamples + 1);
            }

            float yL = xL * gainLin[0];
            float yR = xR * gainLin[1];

            // Style colour, kept subtle and tied to how hard the unit is working.
            if (style == CompStyle::Fet)
            {
                const float amt = 0.06f * clampv (-gr[0] / 12.0f, 0.0f, 1.0f) + 0.01f;
                yL -= amt * yL * yL * yL * 0.3333f;
                yR -= amt * yR * yR * yR * 0.3333f;
            }
            else if (style == CompStyle::VariMu)
            {
                const float amt = 0.05f * clampv (-gr[0] / 10.0f, 0.0f, 1.0f) + 0.005f;
                yL = dcBlock[0].highpass (yL + amt * yL * yL);
                yR = dcBlock[1].highpass (yR + amt * yR * yR);
            }

            lastOut[0] = yL;
            lastOut[1] = yR;

            const float mk = makeup.next();
            const float mx = mixSm.next();
            yL = xL + (yL * mk - xL) * mx;
            yR = xR + (yR * mk - xR) * mx;

            left[i] = yL;
            if (stereo)
                right[i] = yR;
            outMeter.push (std::max (std::abs (yL), std::abs (yR)));
        }

        const float worst = std::min (gr[0], stereo ? gr[1] : gr[0]);
        grMeter.store (worst, std::memory_order_relaxed);
        inLevel.store (inMeter.value, std::memory_order_relaxed);
        outLevel.store (outMeter.value, std::memory_order_relaxed);
        for (auto& h : scHpf)
            h.sanitise();
    }

    float getGainReductionDb() const noexcept { return grMeter.load (std::memory_order_relaxed); }
    float getInputLevel() const noexcept { return inLevel.load (std::memory_order_relaxed); }
    float getOutputLevel() const noexcept { return outLevel.load (std::memory_order_relaxed); }

private:
    void updateDerived() noexcept
    {
        style = params.style;
        lookaheadSamples = clampv ((int) std::lround (params.lookaheadMs * 0.001 * sr), 0,
                                   (int) std::ceil (kMaxLookaheadMs * 0.001 * sr));
        holdSamples = (int) std::lround (params.holdMs * 0.001 * sr);

        double attack = params.attackMs, release = params.releaseMs;
        programRelease = params.autoRelease;
        rmsCoef = 0.0f;

        switch (style)
        {
            case CompStyle::Clean: break;
            case CompStyle::Punch: break;
            case CompStyle::Glue:
                rmsCoef = onePoleCoef (10.0, sr);
                break;
            case CompStyle::Opto:
                attack = std::max (attack, 5.0);
                rmsCoef = onePoleCoef (12.0, sr);
                programRelease = true;
                break;
            case CompStyle::VariMu:
                attack = std::max (attack, 1.0) * 1.5;
                release *= 1.5;
                rmsCoef = onePoleCoef (8.0, sr);
                break;
            case CompStyle::Fet:
                attack *= 0.08; // the attack control covers a far faster range in this style
                break;
            case CompStyle::Count: break;
        }

        attackCoef = onePoleCoef (attack, sr);
        punchSlowCoef = onePoleCoef (attack * 3.0 + 2.0, sr);
        releaseCoef = onePoleCoef (release, sr);
        fastReleaseCoef = onePoleCoef (std::max (20.0, release * 0.35), sr);
        slowestReleaseCoef = onePoleCoef (release * 4.0 + 300.0, sr);
        slowAttackCoef = onePoleCoef (400.0, sr);
        slowReleaseCoef = onePoleCoef (2000.0, sr);

        // Feedback detection: gain reduction = -k * (output over threshold). A k of R - 1
        // gives the same static curve as a feed-forward ratio of R. The one-sample loop is
        // stable while k * (1 - attackCoef) < 1, so very fast attacks are eased back.
        // Loop gain is capped: a feedback compressor cannot reach a true limiting ratio,
        // and very high loop gain turns detector ripple into audible gain flutter.
        const float R = std::max (1.0f, params.ratio);
        fbSlope = std::min (R - 1.0f, style == CompStyle::Glue ? 9.0f : 19.0f);
        peakDecay = onePoleCoef (std::min (12.0, release * 0.5), sr);
        if (style == CompStyle::Glue || style == CompStyle::Fet)
        {
            const float maxStep = 0.85f / std::max (fbSlope, 1.0e-3f);
            if (1.0f - attackCoef > maxStep)
                attackCoef = 1.0f - maxStep;
        }

        hpfActive = params.scHpfHz > 21.0f;
        if (hpfActive)
        {
            const auto c = design::highpass (params.scHpfHz, 0.7071, sr);
            scHpf[0].setCoefs (c);
            scHpf[1].setCoefs (c);
        }
        dcBlock[0].setCutoff (8.0, sr);
        dcBlock[1].setCutoff (8.0, sr);

        float mk = params.makeupDb;
        if (params.autoMakeup)
            mk += std::min (24.0f, -0.5f * gainReductionFor (0.0f));
        makeup.setTarget (dbToGain (mk));
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));
    }

    double sr = 44100.0;
    CompParams params;
    CompStyle style = CompStyle::Clean;

    std::array<DelayLine, 2> delay;
    std::array<Biquad, 2> scHpf;
    std::array<OnePole, 2> dcBlock;
    std::array<float, 2> gr { 0.0f, 0.0f }, slowGr { 0.0f, 0.0f }, rmsState { 0.0f, 0.0f }, peakEnv { 0.0f, 0.0f }, lastOut { 0.0f, 0.0f };
    std::array<int, 2> holdCount { 0, 0 };

    int lookaheadSamples = 0, holdSamples = 0;
    float attackCoef = 0.0f, releaseCoef = 0.0f, punchSlowCoef = 0.0f;
    float fastReleaseCoef = 0.0f, slowestReleaseCoef = 0.0f, slowAttackCoef = 0.0f, slowReleaseCoef = 0.0f;
    float rmsCoef = 0.0f, fbSlope = 3.0f, peakDecay = 0.0f;
    bool programRelease = false, hpfActive = false;

    Smoothed makeup, mixSm;
    LevelFollower inMeter, outMeter;
    std::atomic<float> grMeter { 0.0f }, inLevel { 0.0f }, outLevel { 0.0f };
};
} // namespace gitto
