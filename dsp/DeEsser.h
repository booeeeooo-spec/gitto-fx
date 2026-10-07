// Gitto FX De-Esser - sibilance control with wide-band or split-band reduction.
#pragma once

#include "Blocks.h"
#include <atomic>

namespace gitto
{
enum class DeEssMode { Vocal = 0, Wide, Count };   // how sibilance is detected
enum class DeEssBand { WideBand = 0, SplitBand, Count }; // what gets turned down

struct DeEsserParams
{
    DeEssMode mode = DeEssMode::Vocal;
    DeEssBand band = DeEssBand::SplitBand;
    float thresholdDb = -30.0f; // -60..0
    float rangeDb = 9.0f;       // 0..24 maximum reduction
    float freqHz = 5500.0f;     // 2000..12000: bottom of the detection band (and the split point)
    float topHz = 14000.0f;     // 4000..20000: top of the detection band
    float releaseMs = 60.0f;    // 10..300
    float lookaheadMs = 0.0f;   // 0..15
    float stereoLink = 1.0f;    // 0..1
    bool listen = false;        // hear what the detector hears
};

class DeEsser
{
public:
    static constexpr double kMaxLookaheadMs = 15.0;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        const int maxLook = (int) std::ceil (kMaxLookaheadMs * 0.001 * sr) + 4;
        for (auto& ch : chans)
        {
            ch.low.resize (maxLook);
            ch.high.resize (maxLook);
        }
        attackCoef = onePoleCoef (0.3, sr);
        peakDecay = onePoleCoef (1.5, sr);
        slowCoef = onePoleCoef (300.0, sr);
        reset();
        update();
    }

    void reset() noexcept
    {
        for (auto& ch : chans)
            ch.reset();
    }

    void setParams (const DeEsserParams& p) noexcept
    {
        params = p;
        update();
    }

    int getLatencySamples() const noexcept { return lookahead; }
    float getGainReductionDb() const noexcept { return grMeter.load (std::memory_order_relaxed); }
    float getDetectorDb() const noexcept { return detMeter.load (std::memory_order_relaxed); }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        const int numCh = stereo ? 2 : 1;
        float* io[2] = { left, right };
        float worst = 0.0f, detPeak = -120.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float det[2] = { -120.0f, -120.0f }, sc[2] = { 0.0f, 0.0f };
            for (int c = 0; c < numCh; ++c)
            {
                auto& ch = chans[(size_t) c];
                const float x = io[c][i];
                sc[c] = ch.detLp.process (ch.detHp[1].process (ch.detHp[0].process (x)));
                ch.peak = std::max (std::abs (sc[c]), ch.peak * peakDecay);
                float level = gainToDb (ch.peak);
                if (params.mode == DeEssMode::Vocal)
                {
                    // Judge the "s" band against the voice's own running level, so the
                    // same setting works on quiet and loud takes alike.
                    ch.slow = x * x + (ch.slow - x * x) * slowCoef;
                    const float average = std::max (10.0f * std::log10 (ch.slow + 1.0e-12f), -70.0f);
                    level = level - average - 18.0f;
                }
                det[c] = level;
            }
            const float linked = std::max (det[0], det[1]);

            for (int c = 0; c < numCh; ++c)
            {
                auto& ch = chans[(size_t) c];
                const float level = stereo ? det[c] + (linked - det[c]) * params.stereoLink : det[c];
                detPeak = std::max (detPeak, level);

                // 4:1 above the threshold with a 6 dB knee, limited to Range.
                const float over = level - params.thresholdDb;
                float target = 0.0f;
                if (2.0f * over >= -6.0f)
                    target = 2.0f * std::abs (over) <= 6.0f ? -0.75f * (over + 3.0f) * (over + 3.0f) / 12.0f : -0.75f * over;
                target = std::max (target, -params.rangeDb);
                ch.gr = target < ch.gr ? target + (ch.gr - target) * attackCoef : target + (ch.gr - target) * releaseCoef;
                ch.gr = undenorm (ch.gr);
                worst = std::min (worst, ch.gr);
                const float gain = dbToGain (ch.gr);

                const float x = io[c][i];
                float lo, hi;
                ch.split.split (x, lo, hi);
                if (lookahead > 0)
                {
                    ch.low.write (lo);
                    ch.high.write (hi);
                    lo = ch.low.readInt (lookahead + 1);
                    hi = ch.high.readInt (lookahead + 1);
                }
                float y = params.band == DeEssBand::SplitBand ? lo + hi * gain : (lo + hi) * gain;
                if (params.listen)
                    y = sc[c];
                io[c][i] = y;
            }
        }

        grMeter.store (worst, std::memory_order_relaxed);
        detMeter.store (detPeak, std::memory_order_relaxed);
        for (auto& ch : chans)
        {
            ch.split.sanitise();
            ch.detHp[0].sanitise();
            ch.detHp[1].sanitise();
            ch.detLp.sanitise();
        }
    }

private:
    struct Channel
    {
        void reset() noexcept
        {
            split.reset();
            detHp[0].reset();
            detHp[1].reset();
            detLp.reset();
            low.clear();
            high.clear();
            peak = slow = gr = 0.0f;
        }
        Crossover split;
        std::array<Biquad, 2> detHp;
        Biquad detLp;
        DelayLine low, high;
        float peak = 0.0f, slow = 0.0f, gr = 0.0f;
    };

    void update() noexcept
    {
        const double f = clampv ((double) params.freqHz, 1000.0, sr * 0.4);
        const double top = clampv ((double) params.topHz, f * 1.2, sr * 0.47);
        for (auto& ch : chans)
        {
            ch.split.set (f, sr);
            ch.detHp[0].setCoefs (design::highpass (f, 0.5412, sr));
            ch.detHp[1].setCoefs (design::highpass (f, 1.3066, sr));
            ch.detLp.setCoefs (design::lowpass (top, 0.7071, sr));
        }
        releaseCoef = onePoleCoef (clampv (params.releaseMs, 5.0f, 500.0f), sr);
        lookahead = clampv ((int) std::lround (params.lookaheadMs * 0.001 * sr), 0, (int) std::ceil (kMaxLookaheadMs * 0.001 * sr));
    }

    double sr = 44100.0;
    DeEsserParams params;
    std::array<Channel, 2> chans;
    float attackCoef = 0.0f, releaseCoef = 0.0f, peakDecay = 0.0f, slowCoef = 0.0f;
    int lookahead = 0;
    std::atomic<float> grMeter { 0.0f }, detMeter { -120.0f };
};
} // namespace gitto
