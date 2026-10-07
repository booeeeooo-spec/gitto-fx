// Gitto FX Multiband - four-band dynamics processor.
#pragma once

#include "Blocks.h"
#include <atomic>

namespace gitto
{
enum class BandPlacement { Stereo = 0, Mid, Side, Count };

struct MultibandBandParams
{
    bool enabled = true;
    float thresholdDb = -24.0f; // -60..0
    float rangeDb = -12.0f;     // -30..+30: negative compresses, positive expands upward
    float ratio = 3.0f;         // 1..20
    float attackMs = 15.0f;     // 0.1..250
    float releaseMs = 150.0f;   // 5..2500
    float gainDb = 0.0f;        // -18..+18 band output level
    BandPlacement placement = BandPlacement::Stereo;
};

struct MultibandParams
{
    std::array<MultibandBandParams, 4> bands;
    float xover1 = 120.0f, xover2 = 800.0f, xover3 = 5000.0f;
    float lookaheadMs = 0.0f;   // 0..20
    float mix = 1.0f;
    float outputDb = 0.0f;
};

class Multiband
{
public:
    static constexpr int kBands = 4;
    static constexpr double kMaxLookaheadMs = 20.0;
    static constexpr float kKnee = 6.0f;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        const int maxLook = (int) std::ceil (kMaxLookaheadMs * 0.001 * sr) + 4;
        for (auto& ch : delay)
            for (auto& d : ch)
                d.resize (maxLook);
        outSm.reset (sr, 30.0, 1.0f);
        mixSm.reset (sr, 30.0, 1.0f);
        for (auto& g : bandGain)
            g.reset (sr, 30.0, 1.0f);
        reset();
        update();
        outSm.snap();
        mixSm.snap();
        for (auto& g : bandGain)
            g.snap();
    }

    void reset() noexcept
    {
        for (auto& s : splitter) s.reset();
        for (auto& ch : delay)
            for (auto& d : ch)
                d.clear();
        gr.fill (0.0f);
        peak.fill (0.0f);
    }

    void setParams (const MultibandParams& p) noexcept
    {
        params = p;
        update();
    }

    int getLatencySamples() const noexcept { return lookahead; }
    float getBandGainDb (int band) const noexcept { return meters[(size_t) band].load (std::memory_order_relaxed); }

    // Gain change in dB for a detector level, for one band's settings.
    static float curve (const MultibandBandParams& b, float levelDb) noexcept
    {
        const float over = levelDb - b.thresholdDb;
        const float slope = 1.0f - 1.0f / clampv (b.ratio, 1.0f, 20.0f);
        float amount; // dB of gain change magnitude before the range limit
        if (2.0f * over < -kKnee)
            amount = 0.0f;
        else if (2.0f * std::abs (over) <= kKnee)
            amount = slope * (over + kKnee * 0.5f) * (over + kKnee * 0.5f) / (2.0f * kKnee);
        else
            amount = slope * over;
        if (b.rangeDb < 0.0f)
            return -std::min (amount, -b.rangeDb);
        return std::min (amount, b.rangeDb);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = left[i], inR = stereo ? right[i] : inL;
            float bl[kBands], br[kBands];
            splitter[0].process (inL, bl);
            if (stereo)
                splitter[1].process (inR, br);
            else
                for (int b = 0; b < kBands; ++b)
                    br[b] = bl[b];

            // The dry signal for Mix is the unprocessed bands summed back together, so it
            // carries the same crossover phase as the processed path and blends cleanly.
            float sumL = 0.0f, sumR = 0.0f, dryL = 0.0f, dryR = 0.0f;
            for (int b = 0; b < kBands; ++b)
            {
                const auto& bp = params.bands[(size_t) b];
                float mid = 0.5f * (bl[b] + br[b]), side = 0.5f * (bl[b] - br[b]);

                // Detector: whichever part of the band this band is set to work on.
                float lvl;
                switch (bp.placement)
                {
                    case BandPlacement::Mid:  lvl = std::abs (mid); break;
                    case BandPlacement::Side: lvl = std::abs (side); break;
                    default:                  lvl = std::max (std::abs (bl[b]), std::abs (br[b])); break;
                }
                peak[(size_t) b] = std::max (lvl, peak[(size_t) b] * peakDecay[(size_t) b]);
                const float target = bp.enabled ? curve (bp, gainToDb (peak[(size_t) b])) : 0.0f;
                float& g = gr[(size_t) b];
                // "Attack" is always the move away from 0 dB, whether that is down or up.
                const bool engaging = std::abs (target) > std::abs (g);
                g = target + (g - target) * (engaging ? attackCoef[(size_t) b] : releaseCoef[(size_t) b]);
                g = undenorm (g);
                const float gain = dbToGain (g);

                float dl = bl[b], dr = br[b];
                if (lookahead > 0)
                {
                    delay[0][(size_t) b].write (dl);
                    delay[1][(size_t) b].write (dr);
                    dl = delay[0][(size_t) b].readInt (lookahead + 1);
                    dr = delay[1][(size_t) b].readInt (lookahead + 1);
                    mid = 0.5f * (dl + dr);
                    side = 0.5f * (dl - dr);
                }

                dryL += dl;
                dryR += dr;
                const float level = bandGain[(size_t) b].next();
                switch (bp.placement)
                {
                    case BandPlacement::Mid:  mid *= gain; dl = mid + side; dr = mid - side; break;
                    case BandPlacement::Side: side *= gain; dl = mid + side; dr = mid - side; break;
                    default:                  dl *= gain; dr *= gain; break;
                }
                sumL += dl * level;
                sumR += dr * level;
            }

            const float mx = mixSm.next(), og = outSm.next();
            left[i] = (dryL + (sumL - dryL) * mx) * og;
            if (stereo)
                right[i] = (dryR + (sumR - dryR) * mx) * og;
        }
        for (int b = 0; b < kBands; ++b)
            meters[(size_t) b].store (gr[(size_t) b], std::memory_order_relaxed);
        for (auto& s : splitter)
            s.sanitise();
    }

private:
    void update() noexcept
    {
        for (auto& s : splitter)
            s.setFrequencies (params.xover1, params.xover2, params.xover3, sr);
        lookahead = clampv ((int) std::lround (params.lookaheadMs * 0.001 * sr), 0, (int) std::ceil (kMaxLookaheadMs * 0.001 * sr));
        for (int b = 0; b < kBands; ++b)
        {
            const auto& bp = params.bands[(size_t) b];
            attackCoef[(size_t) b] = onePoleCoef (clampv (bp.attackMs, 0.1f, 250.0f), sr);
            releaseCoef[(size_t) b] = onePoleCoef (clampv (bp.releaseMs, 5.0f, 2500.0f), sr);
            // Hold-off long enough to bridge one cycle at the bottom of each band.
            static const double holdMs[kBands] = { 25.0, 12.0, 4.0, 1.5 };
            peakDecay[(size_t) b] = onePoleCoef (holdMs[b], sr);
            bandGain[(size_t) b].setTarget (bp.enabled ? dbToGain (bp.gainDb) : 1.0f);
        }
        outSm.setTarget (dbToGain (params.outputDb));
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));
    }

    double sr = 44100.0;
    MultibandParams params;
    std::array<FourBandSplitter, 2> splitter;
    std::array<std::array<DelayLine, kBands>, 2> delay;
    std::array<float, kBands> gr {}, peak {}, attackCoef {}, releaseCoef {}, peakDecay {};
    std::array<Smoothed, kBands> bandGain;
    std::array<std::atomic<float>, kBands> meters {};
    Smoothed outSm, mixSm;
    int lookahead = 0;
};
} // namespace gitto
