// Gitto FX Saturator - five-style analog-flavoured saturation, 4x oversampled.
#pragma once

#include "Blocks.h"
#include <atomic>

namespace gitto
{
enum class SatStyle { Warm = 0, Tape, Console, Crunch, Fuzz, Count };

struct SaturatorParams
{
    SatStyle style = SatStyle::Warm;
    float drive = 0.35f;        // 0..1 -> 0..36 dB of gain into the curve
    bool boost = false;         // a further 18 dB
    float lowCutHz = 20.0f;     // 20..500, before the saturation
    float tone = 0.0f;          // -1 dark .. +1 bright
    float highCutHz = 20000.0f; // 2000..20000, after the saturation
    float mix = 1.0f;
    float outputDb = 0.0f;
    bool autoGain = true;
};

class Saturator
{
public:
    static constexpr int kLatency = Oversampler4x::kLatency;

    static ShapeStyle curveFor (SatStyle s) noexcept
    {
        switch (s)
        {
            case SatStyle::Warm:    return ShapeStyle::WarmTube;
            case SatStyle::Tape:    return ShapeStyle::Tape;
            case SatStyle::Console: return ShapeStyle::Console;
            case SatStyle::Crunch:  return ShapeStyle::Diode;
            case SatStyle::Fuzz:    return ShapeStyle::Fuzz;
            case SatStyle::Count:   break;
        }
        return ShapeStyle::Tape;
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        for (auto& d : dry) d.resize (kLatency + 8);
        driveSm.reset (sr, 30.0, 1.0f);
        compSm.reset (sr, 60.0, 1.0f);
        mixSm.reset (sr, 30.0, 1.0f);
        outSm.reset (sr, 30.0, 1.0f);
        inMeter.prepare (sr);
        outMeter.prepare (sr);
        reset();
        update();
        driveSm.snap();
        compSm.snap();
        mixSm.snap();
        outSm.snap();
    }

    void reset() noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            os[(size_t) c].reset();
            dry[(size_t) c].clear();
            lowCut[(size_t) c].reset();
            highCut[(size_t) c].reset();
            tilt[(size_t) c].reset();
            dc[(size_t) c].reset();
        }
    }

    void setParams (const SaturatorParams& p) noexcept
    {
        params = p;
        update();
    }

    float driveDb() const noexcept { return 36.0f * clampv (params.drive, 0.0f, 1.0f) + (params.boost ? 18.0f : 0.0f); }

    // Static input-to-output curve with the current settings, for the display.
    float transfer (float x) const noexcept { return shape (curve, x * dbToGain (driveDb())) * makeup; }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        float* io[2] = { left, right };
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = driveSm.next(), comp = compSm.next(), mx = mixSm.next(), og = outSm.next();
            float peakIn = 0.0f, peakOut = 0.0f;
            for (int c = 0; c < (stereo ? 2 : 1); ++c)
            {
                const float in = io[c][i];
                peakIn = std::max (peakIn, std::abs (in));
                float x = lowCutActive ? lowCut[(size_t) c].process (in) : in;

                float up[4];
                os[(size_t) c].upsample (x, up);
                for (float& u : up)
                    u = shape (curve, u * g);
                float y = os[(size_t) c].downsample (up);

                y = dc[(size_t) c].highpass (y) * comp;
                y = tilt[(size_t) c].process (y);
                if (highCutActive)
                    y = highCut[(size_t) c].process (y);

                dry[(size_t) c].write (in);
                const float d = dry[(size_t) c].readInt (kLatency + 1);
                const float out = (d + (y - d) * mx) * og;
                io[c][i] = out;
                peakOut = std::max (peakOut, std::abs (out));
            }
            inMeter.push (peakIn);
            outMeter.push (peakOut);
        }
        inLevel.store (inMeter.value, std::memory_order_relaxed);
        outLevel.store (outMeter.value, std::memory_order_relaxed);
        for (int c = 0; c < 2; ++c)
        {
            lowCut[(size_t) c].sanitise();
            highCut[(size_t) c].sanitise();
        }
    }

    float getInputLevel() const noexcept { return inLevel.load (std::memory_order_relaxed); }
    float getOutputLevel() const noexcept { return outLevel.load (std::memory_order_relaxed); }

private:
    void update() noexcept
    {
        curve = curveFor (params.style);
        const float g = dbToGain (driveDb());
        makeup = params.autoGain ? levelMatchGain ([this, g] (float x) { return shape (curve, x * g); }) : 1.0f;
        driveSm.setTarget (g);
        compSm.setTarget (makeup);
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));
        outSm.setTarget (dbToGain (params.outputDb));

        lowCutActive = params.lowCutHz > 21.0f;
        highCutActive = params.highCutHz < 19900.0f;
        const auto hp = design::highpass (params.lowCutHz, 0.7071, sr);
        const auto lp = design::lowpass (std::min ((double) params.highCutHz, sr * 0.47), 0.7071, sr);
        for (int c = 0; c < 2; ++c)
        {
            lowCut[(size_t) c].setCoefs (hp);
            highCut[(size_t) c].setCoefs (lp);
            tilt[(size_t) c].set (params.tone, 1200.0, sr);
            dc[(size_t) c].setCutoff (4.0, sr);
        }
    }

    double sr = 44100.0;
    SaturatorParams params;
    ShapeStyle curve = ShapeStyle::WarmTube;
    float makeup = 1.0f;
    bool lowCutActive = false, highCutActive = false;
    std::array<Oversampler4x, 2> os;
    std::array<DelayLine, 2> dry;
    std::array<Biquad, 2> lowCut, highCut;
    std::array<TiltFilter, 2> tilt;
    std::array<OnePole, 2> dc;
    Smoothed driveSm, compSm, mixSm, outSm;
    LevelFollower inMeter, outMeter;
    std::atomic<float> inLevel { 0.0f }, outLevel { 0.0f };
};
} // namespace gitto
