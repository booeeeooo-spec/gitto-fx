// Gitto FX Channel Strip - console-style strip: input drive, filters, four-band EQ,
// compressor, gate/expander and output.
#pragma once

#include "Common.h"
#include <atomic>

namespace gitto
{
enum class StripEqType { Smooth = 0, Tight, Count };
enum class StripOrder { EqThenDynamics = 0, DynamicsThenEq, Count };

struct ChannelStripParams
{
    // Input
    float inputDb = 0.0f;       // -20..+20
    float drive = 0.0f;         // 0..1
    bool phaseInvert = false;
    // Filters
    float hpfHz = 16.0f;        // 16..350 (16 = off)
    float lpfHz = 22000.0f;     // 3000..22000 (22000 = off)
    // EQ
    StripEqType eqType = StripEqType::Smooth;
    float lfHz = 80.0f, lfGainDb = 0.0f;     // 30..450
    bool lfBell = false;
    float lmfHz = 500.0f, lmfGainDb = 0.0f, lmfQ = 1.0f;   // 200..2500, Q 0.5..3
    float hmfHz = 3000.0f, hmfGainDb = 0.0f, hmfQ = 1.0f;  // 600..7000
    float hfHz = 8000.0f, hfGainDb = 0.0f;   // 1500..16000
    bool hfBell = false;
    // Compressor
    float compThresholdDb = 0.0f; // -50..0
    float compRatio = 3.0f;       // 1..20
    float compReleaseMs = 300.0f; // 100..4000
    bool compFastAttack = false;
    float compMakeupDb = 0.0f;    // 0..20
    // Gate / expander
    float gateThresholdDb = -70.0f; // -70..0 (-70 = off)
    float gateRangeDb = 20.0f;      // 0..40
    float gateReleaseMs = 300.0f;   // 100..4000
    bool gateExpander = false;      // false = gate, true = 2:1 expander
    // Routing and output
    StripOrder order = StripOrder::EqThenDynamics;
    float outputDb = 0.0f;        // -20..+20
};

class ChannelStrip
{
public:
    static constexpr int kEqSections = 6; // HPF, LPF, LF, LMF, HMF, HF

    // Builds the filter and EQ sections for a set of parameters. Used for audio and display.
    static void design (const ChannelStripParams& p, double sr, BiquadCoefs* out, bool* active) noexcept
    {
        const bool tight = p.eqType == StripEqType::Tight;
        const double shelfQ = tight ? 0.95 : 0.6;
        // "Tight" bells narrow as they are pushed further, the way some consoles behave.
        auto bellQ = [tight] (float q, float gain) { return tight ? (double) q * (1.0 + std::abs (gain) / 9.0) : (double) q; };

        active[0] = p.hpfHz > 16.5f;
        active[1] = p.lpfHz < 21900.0f;
        out[0] = design::highpass (p.hpfHz, 0.7071, sr);
        out[1] = design::lowpass (std::min ((double) p.lpfHz, sr * 0.47), 0.7071, sr);

        active[2] = std::abs (p.lfGainDb) > 0.01f;
        out[2] = p.lfBell ? design::matchedPeak (p.lfHz, bellQ (1.0f, p.lfGainDb), p.lfGainDb, sr)
                          : design::matchedLowShelf (p.lfHz, shelfQ, p.lfGainDb, sr);
        active[3] = std::abs (p.lmfGainDb) > 0.01f;
        out[3] = design::matchedPeak (p.lmfHz, bellQ (p.lmfQ, p.lmfGainDb), p.lmfGainDb, sr);
        active[4] = std::abs (p.hmfGainDb) > 0.01f;
        out[4] = design::matchedPeak (p.hmfHz, bellQ (p.hmfQ, p.hmfGainDb), p.hmfGainDb, sr);
        active[5] = std::abs (p.hfGainDb) > 0.01f;
        out[5] = p.hfBell ? design::matchedPeak (p.hfHz, bellQ (1.0f, p.hfGainDb), p.hfGainDb, sr)
                          : design::matchedHighShelf (p.hfHz, shelfQ, p.hfGainDb, sr);
    }

    static double responseDb (const ChannelStripParams& p, double hz, double sr) noexcept
    {
        BiquadCoefs c[kEqSections];
        bool on[kEqSections];
        design (p, sr, c, on);
        double mag = 1.0;
        const double w = kTwoPi * hz / sr;
        for (int i = 0; i < kEqSections; ++i)
            if (on[i])
                mag *= c[i].magnitude (w);
        return 20.0 * std::log10 (std::max (mag, 1.0e-9));
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        inSm.reset (sr, 30.0, 1.0f);
        outSm.reset (sr, 30.0, 1.0f);
        makeupSm.reset (sr, 30.0, 1.0f);
        inMeter.prepare (sr);
        outMeter.prepare (sr);
        compAttackFast = onePoleCoef (1.0, sr);
        compAttackSlow = onePoleCoef (18.0, sr);
        gateAttack = onePoleCoef (0.5, sr);
        peakDecay = onePoleCoef (8.0, sr);
        gateHold = (int) (0.02 * sr);
        reset();
        update();
        inSm.snap();
        outSm.snap();
        makeupSm.snap();
    }

    void reset() noexcept
    {
        for (auto& ch : filters)
            for (auto& f : ch)
                f.reset();
        lastDrive = { 0.0f, 0.0f };
        compGr = gateGr = 0.0f;
        compPeak = gatePeak = 0.0f;
        gateOpen = false;
        gateHoldCount = 0;
    }

    void setParams (const ChannelStripParams& p) noexcept
    {
        params = p;
        update();
    }

    float getCompReductionDb() const noexcept { return compMeter.load (std::memory_order_relaxed); }
    float getGateReductionDb() const noexcept { return gateMeter.load (std::memory_order_relaxed); }
    float getInputLevel() const noexcept { return inLevel.load (std::memory_order_relaxed); }
    float getOutputLevel() const noexcept { return outLevel.load (std::memory_order_relaxed); }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        const bool eqFirst = params.order == StripOrder::EqThenDynamics;
        const float polarity = params.phaseInvert ? -1.0f : 1.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float ig = inSm.next() * polarity;
            float x[2] = { left[i] * ig, stereo ? right[i] * ig : 0.0f };
            const int numCh = stereo ? 2 : 1;
            inMeter.push (std::max (std::abs (x[0]), std::abs (x[1])));

            for (int c = 0; c < numCh; ++c)
            {
                if (driveActive)
                    x[c] = driveStage (c, x[c]);
                // The two pass filters always sit first, as on a console.
                if (active[0]) x[c] = filters[(size_t) c][0].process (x[c]);
                if (active[1]) x[c] = filters[(size_t) c][1].process (x[c]);
                if (eqFirst)
                    x[c] = runEq (c, x[c]);
            }

            // Dynamics: one linked detector, gate and compressor side by side.
            const float lvl = std::max (std::abs (x[0]), std::abs (x[1]));
            const float gain = dynamics (lvl) * makeupSm.next();
            const float og = outSm.next();
            for (int c = 0; c < numCh; ++c)
            {
                x[c] *= gain;
                if (! eqFirst)
                    x[c] = runEq (c, x[c]);
                x[c] *= og;
            }

            left[i] = x[0];
            if (stereo)
                right[i] = x[1];
            outMeter.push (std::max (std::abs (x[0]), std::abs (x[1])));
        }

        compMeter.store (compGr, std::memory_order_relaxed);
        gateMeter.store (gateGr, std::memory_order_relaxed);
        inLevel.store (inMeter.value, std::memory_order_relaxed);
        outLevel.store (outMeter.value, std::memory_order_relaxed);
        for (auto& ch : filters)
            for (auto& f : ch)
                f.sanitise();
    }

private:
    float runEq (int c, float x) noexcept
    {
        for (int s = 2; s < kEqSections; ++s)
            if (active[s])
                x = filters[(size_t) c][(size_t) s].process (x);
        return x;
    }

    // tanh saturation with first-order antiderivative anti-aliasing: far less aliasing
    // than a plain waveshaper, with no oversampling and no added latency.
    float driveStage (int c, float x) noexcept
    {
        auto lnCosh = [] (float v)
        {
            const float a = std::abs (v);
            return a + std::log1p (std::exp (-2.0f * a)) - 0.69314718f;
        };
        const float u = x * driveGain;
        float& prev = lastDrive[(size_t) c];
        const float diff = u - prev;
        const float y = std::abs (diff) > 1.0e-4f ? (lnCosh (u) - lnCosh (prev)) / diff : std::tanh (0.5f * (u + prev));
        prev = u;
        return y * driveNorm;
    }

    float dynamics (float level) noexcept
    {
        // --- compressor ---
        compPeak = std::max (level, compPeak * peakDecay);
        float target = 0.0f;
        if (compActive)
        {
            const float over = gainToDb (compPeak) - params.compThresholdDb;
            const float knee = 6.0f;
            if (2.0f * over >= -knee)
                target = 2.0f * std::abs (over) <= knee ? -compSlope * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee)
                                                         : -compSlope * over;
        }
        const float attack = params.compFastAttack ? compAttackFast : compAttackSlow;
        compGr = target < compGr ? target + (compGr - target) * attack : target + (compGr - target) * compRelease;
        compGr = undenorm (compGr);

        // --- gate / expander ---
        float gTarget = 0.0f;
        if (gateActive)
        {
            gatePeak = std::max (level, gatePeak * peakDecay);
            const float db = gainToDb (gatePeak);
            if (params.gateExpander)
            {
                // 2:1 below the threshold, down to Range.
                gTarget = db < params.gateThresholdDb ? std::max (db - params.gateThresholdDb, -params.gateRangeDb) : 0.0f;
            }
            else
            {
                // Opens at the threshold, closes 3 dB below it after a short hold.
                if (db >= params.gateThresholdDb)
                {
                    gateOpen = true;
                    gateHoldCount = gateHold;
                }
                else if (db < params.gateThresholdDb - 3.0f)
                {
                    if (gateHoldCount > 0)
                        --gateHoldCount;
                    else
                        gateOpen = false;
                }
                gTarget = gateOpen ? 0.0f : -params.gateRangeDb;
            }
        }
        gateGr = gTarget > gateGr ? gTarget + (gateGr - gTarget) * gateAttack : gTarget + (gateGr - gTarget) * gateRelease;
        gateGr = undenorm (gateGr);

        return dbToGain (compGr + gateGr);
    }

    void update() noexcept
    {
        BiquadCoefs c[kEqSections];
        design (params, sr, c, active.data());
        for (auto& ch : filters)
            for (int s = 0; s < kEqSections; ++s)
                ch[(size_t) s].setCoefs (c[s]);

        driveActive = params.drive > 0.005f;
        driveGain = 1.0f + 2.0f * params.drive * params.drive + 1.0f * params.drive; // up to 12 dB into the curve
        driveNorm = 1.0f / driveGain;

        compActive = params.compThresholdDb < -0.05f && params.compRatio > 1.01f;
        compSlope = 1.0f - 1.0f / clampv (params.compRatio, 1.0f, 20.0f);
        compRelease = onePoleCoef (clampv (params.compReleaseMs, 50.0f, 5000.0f), sr);
        gateActive = params.gateThresholdDb > -69.5f && params.gateRangeDb > 0.05f;
        gateRelease = onePoleCoef (clampv (params.gateReleaseMs, 50.0f, 5000.0f), sr);

        inSm.setTarget (dbToGain (params.inputDb));
        outSm.setTarget (dbToGain (params.outputDb));
        makeupSm.setTarget (dbToGain (params.compMakeupDb));
    }

    double sr = 44100.0;
    ChannelStripParams params;
    std::array<std::array<Biquad, kEqSections>, 2> filters;
    std::array<bool, kEqSections> active {};
    std::array<float, 2> lastDrive { 0.0f, 0.0f };
    float driveGain = 1.0f, driveNorm = 1.0f;
    bool driveActive = false, compActive = false, gateActive = false, gateOpen = false;
    float compGr = 0.0f, gateGr = 0.0f, compPeak = 0.0f, gatePeak = 0.0f, compSlope = 0.0f;
    float compAttackFast = 0.0f, compAttackSlow = 0.0f, compRelease = 0.0f, gateAttack = 0.0f, gateRelease = 0.0f, peakDecay = 0.0f;
    int gateHold = 0, gateHoldCount = 0;
    Smoothed inSm, outSm, makeupSm;
    LevelFollower inMeter, outMeter;
    std::atomic<float> compMeter { 0.0f }, gateMeter { 0.0f }, inLevel { 0.0f }, outLevel { 0.0f };
};
} // namespace gitto
