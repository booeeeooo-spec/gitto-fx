// Gitto FX Distortion - four-band distortion with twelve curves, 4x oversampled.
#pragma once

#include "Blocks.h"
#include <atomic>

namespace gitto
{
struct DistBandParams
{
    bool enabled = true;
    ShapeStyle style = ShapeStyle::SoftTube;
    float drive = 0.3f;     // 0..1 -> 0..40 dB
    float mix = 1.0f;       // 0..1 within the band
    float feedback = 0.0f;  // 0..0.9
    float dynamics = 0.0f;  // -1 expand .. +1 compress, applied before the curve
    float tone = 0.0f;      // -1..1
    float levelDb = 0.0f;   // -24..+12
};

struct DistortionParams
{
    std::array<DistBandParams, 4> bands;
    float xover1 = 150.0f, xover2 = 900.0f, xover3 = 4500.0f;
    float mix = 1.0f;
    float outputDb = 0.0f;
};

class Distortion
{
public:
    static constexpr int kBands = 4;
    static constexpr int kLatency = Oversampler4x::kLatency;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        osr = sr * 4.0;
        for (auto& ch : chans)
            ch.prepare (osr);
        mixSm.reset (sr, 30.0, 1.0f);
        outSm.reset (sr, 30.0, 1.0f);
        envAttack = onePoleCoef (4.0, osr);
        envRelease = onePoleCoef (90.0, osr);
        reset();
        prepared = false; // makes update() rebuild the smoothers for this sample rate
        update();
        mixSm.snap();
        outSm.snap();
        for (auto& b : bandState)
            b.snap();
    }

    void reset() noexcept
    {
        for (auto& ch : chans)
            ch.reset();
    }

    void setParams (const DistortionParams& p) noexcept
    {
        params = p;
        update();
    }

    float getBandActivity (int band) const noexcept { return activity[(size_t) band].load (std::memory_order_relaxed); }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        float* io[2] = { left, right };
        float act[kBands] = { 0.0f, 0.0f, 0.0f, 0.0f };

        for (int i = 0; i < numSamples; ++i)
        {
            const float mx = mixSm.next(), og = outSm.next();
            float drive[kBands], level[kBands], comp[kBands], bmix[kBands];
            for (int b = 0; b < kBands; ++b)
            {
                drive[b] = bandState[(size_t) b].drive.next();
                level[b] = bandState[(size_t) b].level.next();
                comp[b] = bandState[(size_t) b].comp.next();
                bmix[b] = bandState[(size_t) b].mix.next();
            }

            for (int c = 0; c < (stereo ? 2 : 1); ++c)
            {
                auto& ch = chans[(size_t) c];
                float up[4], wet[4], dry[4];
                ch.os.upsample (io[c][i], up);

                for (int k = 0; k < 4; ++k)
                {
                    float bands[kBands];
                    ch.splitter.process (up[k], bands);
                    float wetSum = 0.0f, drySum = 0.0f;
                    for (int b = 0; b < kBands; ++b)
                    {
                        const auto& bp = params.bands[(size_t) b];
                        const float x = bands[b];
                        drySum += x;
                        if (! bp.enabled)
                        {
                            wetSum += x;
                            continue;
                        }
                        auto& st = ch.band[(size_t) b];

                        // Dynamics: evens out (or exaggerates) the level hitting the curve.
                        float in = x;
                        if (dynActive[(size_t) b])
                        {
                            const float a = std::abs (x);
                            st.env = a > st.env ? a + (st.env - a) * envAttack : a + (st.env - a) * envRelease;
                            const float rel = std::max (st.env, 1.0e-4f) * 8.0f; // 1.0 at -18 dBFS
                            in *= clampv (std::pow (rel, dynExponent[(size_t) b]), 0.125f, 8.0f);
                        }

                        // Feedback: a short delay around the curve, which rings at high settings.
                        const float fbIn = in + bp.feedback * st.fbDelay.readInt (fbSamples);
                        float y = shape (bp.style, fbIn * drive[b]);
                        st.fbDelay.write (y);
                        y = st.tilt.process (y * comp[b]);
                        act[b] = std::max (act[b], std::abs (y - x));

                        wetSum += (x + (y - x) * bmix[b]) * level[b];
                    }
                    wet[k] = wetSum;
                    dry[k] = drySum;
                }

                // The dry path goes through the same crossover and resampling, so the
                // overall Mix control blends two signals that are exactly in step.
                // Both paths get the same DC filter too, so they stay in step at the bottom.
                const float w = ch.dc.highpass (ch.os.downsample (wet));
                const float d = ch.dryDc.highpass (ch.dryDown.downsample (dry));
                io[c][i] = (d + (w - d) * mx) * og;
            }
        }

        for (int b = 0; b < kBands; ++b)
            activity[(size_t) b].store (act[b], std::memory_order_relaxed);
        for (auto& ch : chans)
        {
            ch.splitter.sanitise();
            for (auto& st : ch.band)
                st.env = undenorm (st.env);
        }
    }

private:
    struct BandRuntime
    {
        DelayLine fbDelay;
        TiltFilter tilt;
        float env = 0.0f;
    };

    struct Channel
    {
        void prepare (double oversampledRate)
        {
            for (auto& b : band)
                b.fbDelay.resize ((int) (oversampledRate * 0.002) + 8);
            dc.setCutoff (3.0, oversampledRate / 4.0);
            dryDc.setCutoff (3.0, oversampledRate / 4.0);
        }
        void reset() noexcept
        {
            os.reset();
            dryDown.reset();
            splitter.reset();
            dc.reset();
            dryDc.reset();
            for (auto& b : band)
            {
                b.fbDelay.clear();
                b.tilt.reset();
                b.env = 0.0f;
            }
        }
        Oversampler4x os, dryDown;
        FourBandSplitter splitter;
        std::array<BandRuntime, kBands> band;
        OnePole dc, dryDc;
    };

    struct BandSmoothers
    {
        Smoothed drive, level, comp, mix;
        void snap() noexcept
        {
            drive.snap();
            level.snap();
            comp.snap();
            mix.snap();
        }
    };

    void update() noexcept
    {
        for (auto& ch : chans)
            ch.splitter.setFrequencies (params.xover1, params.xover2, params.xover3, osr);
        fbSamples = std::max (1, (int) std::lround (osr * 0.0007));

        const auto& f = chans[0].splitter.freqs;
        const double centres[kBands] = { std::sqrt (30.0 * f[0]), std::sqrt (f[0] * f[1]), std::sqrt (f[1] * f[2]), std::sqrt (f[2] * 16000.0) };

        for (int b = 0; b < kBands; ++b)
        {
            const auto& bp = params.bands[(size_t) b];
            const float g = dbToGain (40.0f * clampv (bp.drive, 0.0f, 1.0f));
            const auto style = bp.style;
            // Each band is trimmed back to roughly its original loudness, so Drive
            // changes the character rather than the level.
            const float comp = levelMatchGain ([style, g] (float x) { return shape (style, x * g); }, 0.06f);
            auto& s = bandState[(size_t) b];
            if (! prepared)
            {
                s.drive.reset (sr, 30.0, g);
                s.level.reset (sr, 30.0, 1.0f);
                s.comp.reset (sr, 60.0, comp);
                s.mix.reset (sr, 30.0, 1.0f);
            }
            s.drive.setTarget (g);
            s.level.setTarget (dbToGain (bp.levelDb));
            s.comp.setTarget (comp);
            s.mix.setTarget (clampv (bp.mix, 0.0f, 1.0f));

            dynActive[(size_t) b] = std::abs (bp.dynamics) > 0.01f;
            dynExponent[(size_t) b] = -0.6f * clampv (bp.dynamics, -1.0f, 1.0f);
            for (auto& ch : chans)
                ch.band[(size_t) b].tilt.set (bp.tone, centres[b], osr);
        }
        prepared = true;
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));
        outSm.setTarget (dbToGain (params.outputDb));
    }

    double sr = 44100.0, osr = 176400.0;
    DistortionParams params;
    std::array<Channel, 2> chans;
    std::array<BandSmoothers, kBands> bandState;
    std::array<bool, kBands> dynActive {};
    std::array<float, kBands> dynExponent {};
    std::array<std::atomic<float>, kBands> activity {};
    Smoothed mixSm, outSm;
    float envAttack = 0.0f, envRelease = 0.0f;
    int fbSamples = 32;
    bool prepared = false;
};
} // namespace gitto
