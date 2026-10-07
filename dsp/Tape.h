// Gitto FX Tape - tape machine colour: saturation, head bump, speed-dependent tone,
// wow and flutter, and hiss.
#pragma once

#include "Blocks.h"
#include <atomic>

namespace gitto
{
enum class TapeSpeed { Ips7 = 0, Ips15, Ips30, Count };
enum class TapeFormula { Modern = 0, Vintage, Cassette, Count };

struct TapeParams
{
    float inputDb = 0.0f;     // -12..+24: how hard the tape is hit
    float outputDb = 0.0f;    // -24..+12
    bool autoGain = true;
    TapeSpeed speed = TapeSpeed::Ips15;
    TapeFormula formula = TapeFormula::Modern;
    float bias = 0.0f;        // -1 under-biased (brighter, grittier) .. +1 over-biased (duller, cleaner)
    float headBump = 0.5f;    // 0..1
    float wow = 0.1f;         // 0..1
    float flutter = 0.1f;     // 0..1
    float hiss = 0.0f;        // 0..1
    float mix = 1.0f;
};

class Tape
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        baseDelay = (int) std::lround (0.005 * sr);
        for (int c = 0; c < 2; ++c)
        {
            transport[(size_t) c].resize (baseDelay * 2 + 64);
            dry[(size_t) c].resize (baseDelay + Oversampler4x::kLatency + 16);
        }
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
            transport[(size_t) c].clear();
            dry[(size_t) c].clear();
            bump[(size_t) c].reset();
            lowRoll[(size_t) c].reset();
            highRoll[(size_t) c].reset();
            biasShelf[(size_t) c].reset();
            hissFilter[(size_t) c].reset();
            dc[(size_t) c].reset();
            magnet[(size_t) c] = 0.0f;
        }
        wowPhase = flutterPhase = 0.0f;
        drift = driftTarget = 0.0f;
    }

    void setParams (const TapeParams& p) noexcept
    {
        params = p;
        update();
    }

    int getLatencySamples() const noexcept { return Oversampler4x::kLatency + baseDelay; }

    // Static curve of the saturation stage (ignores the level-dependent smoothing).
    float transfer (float x) const noexcept { return saturate (x * driveGain) * makeup; }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        float* io[2] = { left, right };
        const int latency = getLatencySamples();

        for (int i = 0; i < numSamples; ++i)
        {
            const float g = driveSm.next(), comp = compSm.next(), mx = mixSm.next(), og = outSm.next();

            // One transport for both channels: they speed up and slow down together.
            float wobble = 0.0f;
            if (wowDepth > 0.0f || flutterDepth > 0.0f)
            {
                wowPhase += wowInc;
                if (wowPhase >= 1.0f) wowPhase -= 1.0f;
                flutterPhase += flutterInc;
                if (flutterPhase >= 1.0f)
                {
                    flutterPhase -= 1.0f;
                    driftTarget = rng.nextBipolar();
                }
                drift += (driftTarget - drift) * driftCoef;
                wobble = wowDepth * std::sin ((float) kTwoPi * wowPhase)
                         + flutterDepth * (std::sin ((float) kTwoPi * flutterPhase) + 0.5f * drift);
            }

            float peakIn = 0.0f, peakOut = 0.0f;
            for (int c = 0; c < (stereo ? 2 : 1); ++c)
            {
                const float in = io[c][i];
                peakIn = std::max (peakIn, std::abs (in));

                float up[4];
                os[(size_t) c].upsample (in, up);
                float& m = magnet[(size_t) c];
                for (float& u : up)
                {
                    // The tape follows the signal more sluggishly the closer it is to
                    // saturation, so loud highs are squashed more than loud lows.
                    const float target = saturate (u * g);
                    const float follow = std::max (0.12f, 1.0f - hysteresis * std::abs (target));
                    m += (target - m) * follow;
                    u = m;
                }
                float y = dc[(size_t) c].highpass (os[(size_t) c].downsample (up)) * comp;

                y = lowRoll[(size_t) c].process (y);
                if (bumpActive)
                    y = bump[(size_t) c].process (y);
                y = highRoll[(size_t) c].process (y);
                if (biasActive)
                    y = biasShelf[(size_t) c].process (y);

                transport[(size_t) c].write (y);
                y = transport[(size_t) c].readCubic (std::max (3.0f, (float) baseDelay + 1.0f + wobble));

                if (hissGain > 0.0f)
                    y += hissFilter[(size_t) c].process (rng.nextBipolar()) * hissGain;

                dry[(size_t) c].write (in);
                const float d = dry[(size_t) c].readInt (latency + 1);
                const float out = (d + (y - d) * mx) * og;
                io[c][i] = out;
                peakOut = std::max (peakOut, std::abs (out));
                m = undenorm (m);
            }
            inMeter.push (peakIn);
            outMeter.push (peakOut);
        }

        inLevel.store (inMeter.value, std::memory_order_relaxed);
        outLevel.store (outMeter.value, std::memory_order_relaxed);
        for (int c = 0; c < 2; ++c)
        {
            bump[(size_t) c].sanitise();
            lowRoll[(size_t) c].sanitise();
            highRoll[(size_t) c].sanitise();
            biasShelf[(size_t) c].sanitise();
            hissFilter[(size_t) c].sanitise();
        }
    }

    float getInputLevel() const noexcept { return inLevel.load (std::memory_order_relaxed); }
    float getOutputLevel() const noexcept { return outLevel.load (std::memory_order_relaxed); }

private:
    float saturate (float x) const noexcept
    {
        // Modern tape stays clean longer and then bends smoothly; the older formulas
        // start bending earlier and a little unevenly.
        switch (params.formula)
        {
            case TapeFormula::Modern:   return shape (ShapeStyle::Tape, x * 0.8f) * 1.25f;
            case TapeFormula::Vintage:  return std::tanh (x + 0.08f * x * x);
            case TapeFormula::Cassette: return shape (ShapeStyle::SoftTube, x * 1.3f) * (1.0f / 1.3f);
            case TapeFormula::Count:    break;
        }
        return x;
    }

    void update() noexcept
    {
        const int sp = clampv ((int) params.speed, 0, 2);
        const bool cassette = params.formula == TapeFormula::Cassette;
        const float bias = clampv (params.bias, -1.0f, 1.0f);

        // Under-biasing pushes the tape harder for the same input; over-biasing eases off.
        driveGain = dbToGain (params.inputDb - 3.0f * bias);
        makeup = params.autoGain ? levelMatchGain ([this] (float x) { return saturate (x * driveGain); }) : 1.0f;
        driveSm.setTarget (driveGain);
        compSm.setTarget (makeup);
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));
        outSm.setTarget (dbToGain (params.outputDb));
        hysteresis = cassette ? 0.8f : (params.formula == TapeFormula::Vintage ? 0.7f : 0.55f);

        static const double bumpHz[3] = { 55.0, 80.0, 115.0 };
        static const double topHz[3] = { 11500.0, 17500.0, 23000.0 };
        static const float bumpDb[3] = { 4.5f, 3.5f, 2.5f };
        const double top = std::min (topHz[sp] * (cassette ? 0.62 : 1.0), sr * 0.47);
        bumpActive = params.headBump > 0.01f;
        biasActive = std::abs (bias) > 0.01f;
        for (int c = 0; c < 2; ++c)
        {
            bump[(size_t) c].setCoefs (design::peak (bumpHz[sp], 1.1, bumpDb[sp] * clampv (params.headBump, 0.0f, 1.0f), sr));
            lowRoll[(size_t) c].setCoefs (design::highpass (bumpHz[sp] * 0.3, 0.7071, sr));
            highRoll[(size_t) c].setCoefs (design::lowpass (top, 0.62, sr));
            biasShelf[(size_t) c].setCoefs (design::highShelf (std::min (7000.0, sr * 0.4), 0.7, -2.5 * bias, sr));
            hissFilter[(size_t) c].setCoefs (design::highShelf (3000.0, 0.7, 9.0, sr));
            dc[(size_t) c].setCutoff (6.0, sr);
        }

        // Slower transports wander more.
        static const float speedWobble[3] = { 1.6f, 1.0f, 0.6f };
        static const double wowHz[3] = { 0.45, 0.7, 1.1 };
        static const double flutterHz[3] = { 6.0, 8.5, 12.0 };
        const float scale = speedWobble[sp] * (cassette ? 1.8f : 1.0f);
        wowDepth = (float) (params.wow * params.wow * 0.0009 * sr) * scale;
        flutterDepth = (float) (params.flutter * params.flutter * 0.00009 * sr) * scale;
        wowInc = (float) (wowHz[sp] / sr);
        flutterInc = (float) (flutterHz[sp] / sr);
        driftCoef = 1.0f - (float) std::exp (-kTwoPi * 20.0 / sr);

        // Hiss: silent at 0, about -46 dBFS at full on the noisiest setting.
        static const float speedHiss[3] = { 4.0f, 0.0f, -3.0f };
        hissGain = params.hiss > 0.005f
                       ? dbToGain (-86.0f + 34.0f * clampv (params.hiss, 0.0f, 1.0f) + speedHiss[sp] + (cassette ? 6.0f : 0.0f))
                       : 0.0f;
    }

    double sr = 44100.0;
    TapeParams params;
    int baseDelay = 240;
    float driveGain = 1.0f, makeup = 1.0f, hysteresis = 0.55f;
    float wowDepth = 0.0f, flutterDepth = 0.0f, wowInc = 0.0f, flutterInc = 0.0f, wowPhase = 0.0f, flutterPhase = 0.0f;
    float drift = 0.0f, driftTarget = 0.0f, driftCoef = 0.0f, hissGain = 0.0f;
    bool bumpActive = false, biasActive = false;
    std::array<Oversampler4x, 2> os;
    std::array<DelayLine, 2> transport, dry;
    std::array<Biquad, 2> bump, lowRoll, highRoll, biasShelf, hissFilter;
    std::array<OnePole, 2> dc;
    std::array<float, 2> magnet { 0.0f, 0.0f };
    Smoothed driveSm, compSm, mixSm, outSm;
    Random rng { 0x7A9E11u };
    LevelFollower inMeter, outMeter;
    std::atomic<float> inLevel { 0.0f }, outLevel { 0.0f };
};
} // namespace gitto
