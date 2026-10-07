// Gitto FX Delay - character delay with eight echo styles and four routing modes.
#pragma once

#include "Common.h"

namespace gitto
{
enum class DelayMode { Single = 0, Dual, PingPong, Rhythm, Count };
enum class DelayStyle { Digital = 0, StudioTape, WornTape, Analog, Tube, LoFi, Telephone, Diffuse, Count };

struct DelayParams
{
    DelayMode mode = DelayMode::Single;
    DelayStyle style = DelayStyle::StudioTape;
    float timeLMs = 375.0f;     // 1..2000 (already resolved from tempo sync)
    float timeRMs = 375.0f;
    float feedback = 0.4f;      // 0..1.1
    float mix = 0.3f;           // 0..1
    float lowCutHz = 80.0f;     // 20..2000
    float highCutHz = 8000.0f;  // 500..20000
    float saturation = 0.3f;    // 0..1
    float wobble = 0.2f;        // 0..1
    float wobbleRateHz = 0.8f;  // 0.1..5
    float diffusion = 0.0f;     // 0..1
    float groove = 0.0f;        // -1 (rushed) .. +1 (shuffled)
    float feel = 0.0f;          // -1 (ahead) .. +1 (behind)
    float width = 1.0f;         // 0..2
    float ducking = 0.0f;       // 0..1
    float outputDb = 0.0f;      // -24..+12
    int pattern = 0;            // Rhythm mode pattern
};

struct RhythmPattern
{
    const char* name;
    int steps;          // loop length in multiples of the delay time
    int numTaps;
    float position[8];  // in steps
    float level[8];
    float pan[8];       // -1..1
};

inline const RhythmPattern& rhythmPattern (int index) noexcept
{
    static const RhythmPattern patterns[] = {
        { "Straight 4", 4, 4, { 1, 2, 3, 4 }, { 1.0f, 0.8f, 0.65f, 0.5f }, { -0.7f, 0.7f, -0.4f, 0.4f } },
        { "Dotted Gallop", 4, 5, { 0.75f, 1.5f, 2.25f, 3.0f, 4.0f }, { 1.0f, 0.85f, 0.7f, 0.6f, 0.5f }, { -0.6f, 0.6f, -0.3f, 0.3f, 0.0f } },
        { "Triplet Roll", 2, 6, { 0.3333f, 0.6667f, 1.0f, 1.3333f, 1.6667f, 2.0f }, { 1.0f, 0.85f, 0.75f, 0.62f, 0.52f, 0.45f }, { -0.8f, 0.0f, 0.8f, -0.5f, 0.0f, 0.5f } },
        { "Swing 8ths", 2, 4, { 0.6667f, 1.0f, 1.6667f, 2.0f }, { 0.8f, 1.0f, 0.6f, 0.75f }, { -0.6f, 0.6f, -0.6f, 0.6f } },
        { "Push", 4, 5, { 0.5f, 1.5f, 2.0f, 3.5f, 4.0f }, { 0.9f, 1.0f, 0.7f, 0.75f, 0.55f }, { 0.5f, -0.5f, 0.0f, 0.7f, -0.7f } },
        { "Stutter", 1, 4, { 0.25f, 0.5f, 0.75f, 1.0f }, { 0.45f, 0.6f, 0.8f, 1.0f }, { -0.3f, 0.3f, -0.6f, 0.6f } },
        { "Swell", 4, 4, { 1, 2, 3, 4 }, { 0.3f, 0.5f, 0.75f, 1.0f }, { 0.6f, -0.6f, 0.3f, -0.3f } },
        { "Wide 3", 3, 3, { 1, 2, 3 }, { 1.0f, 0.8f, 0.65f }, { -1.0f, 1.0f, 0.0f } },
    };
    return patterns[clampv (index, 0, 7)];
}
constexpr int kNumRhythmPatterns = 8;

//==============================================================================
// The tone-shaping applied each time audio is written into a delay segment, so
// every repeat picks up a little more of the style.
class DelayCharacter
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        ap[0].resize ((int) (0.02 * sr) + 16);
        ap[1].resize ((int) (0.03 * sr) + 16);
        reset();
    }

    void reset() noexcept
    {
        hp.reset();
        lp[0].reset();
        lp[1].reset();
        bump.reset();
        dc.reset();
        ap[0].clear();
        ap[1].clear();
        hold = 0.0f;
        holdPhase = 0.0f;
    }

    void configure (DelayStyle s, float lowCut, float highCut, float saturation, float diffusion) noexcept
    {
        style = s;
        double lc = lowCut, hc = highCut;
        steepLp = false;
        bumpActive = false;
        asym = 0.0f;
        lofi = false;
        float minDrive = 0.0f, minDiffusion = 0.0f;

        switch (style)
        {
            case DelayStyle::Digital: break;
            case DelayStyle::StudioTape:
                hc = std::min (hc, 12000.0);
                bumpActive = true;
                bump.setCoefs (design::peak (85.0, 0.9, 1.8, sr));
                minDrive = 0.15f;
                break;
            case DelayStyle::WornTape:
                hc = std::min (hc, 5500.0);
                lc = std::max (lc, 60.0);
                bumpActive = true;
                bump.setCoefs (design::peak (110.0, 0.8, 2.5, sr));
                minDrive = 0.3f;
                asym = 0.08f;
                break;
            case DelayStyle::Analog:
                hc = std::min (hc, 3200.0);
                lc = std::max (lc, 90.0);
                steepLp = true;
                minDrive = 0.2f;
                asym = 0.12f;
                break;
            case DelayStyle::Tube:
                hc = std::min (hc, 7000.0);
                lc = std::max (lc, 110.0);
                minDrive = 0.25f;
                asym = 0.3f;
                break;
            case DelayStyle::LoFi:
                hc = std::min (hc, 4500.0);
                lofi = true;
                break;
            case DelayStyle::Telephone:
                hc = std::min (hc, 3000.0);
                lc = std::max (lc, 400.0);
                steepLp = true;
                minDrive = 0.45f;
                break;
            case DelayStyle::Diffuse:
                hc = std::min (hc, 9000.0);
                minDiffusion = 0.65f;
                break;
            case DelayStyle::Count: break;
        }

        hp.setCoefs (design::highpass (lc, 0.7071, sr));
        hpActive = lc > 21.0;
        hc = std::min (hc, sr * 0.45);
        lpActive = hc < 19500.0;
        if (steepLp)
        {
            lp[0].setCoefs (design::lowpass (hc, 0.5412, sr));
            lp[1].setCoefs (design::lowpass (hc, 1.3066, sr));
        }
        else
        {
            lp[0].setCoefs (design::lowpass (hc, 0.7071, sr));
        }

        const float sat = std::max (saturation, minDrive);
        drive = 1.0f + 5.0f * sat * sat + 1.5f * sat;
        driveActive = sat > 0.01f;
        apCoef = 0.7f * clampv (std::max (diffusion, minDiffusion), 0.0f, 1.0f);
        dc.setCutoff (6.0, sr);
        lofiInc = (float) (9000.0 / sr);
    }

    float process (float x) noexcept
    {
        if (hpActive)
            x = hp.process (x);
        if (bumpActive)
            x = bump.process (x);

        if (driveActive)
        {
            // Unity gain for quiet signals, progressively squashed as level rises.
            float d = x * drive;
            if (asym > 0.0f)
                d += asym * d * d; // even harmonics for tube and bucket-brigade colour
            x = std::tanh (d) / drive;
            if (asym > 0.0f)
                x = dc.highpass (x);
        }

        if (lofi)
        {
            holdPhase += lofiInc;
            if (holdPhase >= 1.0f)
            {
                holdPhase -= 1.0f;
                hold = std::round (x * 512.0f) * (1.0f / 512.0f);
            }
            x = hold;
        }

        if (lpActive)
        {
            x = lp[0].process (x);
            if (steepLp)
                x = lp[1].process (x);
        }

        if (apCoef > 0.01f)
        {
            x = allpass (ap[0], x, (float) (0.0073 * sr));
            x = allpass (ap[1], x, (float) (0.0117 * sr));
        }

        // Safety limiter: transparent below full scale, bounded above it, so feedback
        // settings over 100% build into saturation instead of running away.
        const float a = std::abs (x);
        if (a > 1.0f)
            x = (x > 0.0f ? 1.0f : -1.0f) * (1.0f + std::tanh (a - 1.0f));
        return undenorm (x);
    }

    void sanitise() noexcept
    {
        hp.sanitise();
        lp[0].sanitise();
        lp[1].sanitise();
        bump.sanitise();
    }

private:
    float allpass (DelayLine& d, float x, float delaySamples) noexcept
    {
        const float delayed = d.readLinear (delaySamples);
        const float v = x - apCoef * delayed;
        d.write (undenorm (v));
        return delayed + apCoef * v;
    }

    double sr = 44100.0;
    DelayStyle style = DelayStyle::Digital;
    Biquad hp, bump;
    std::array<Biquad, 2> lp;
    OnePole dc;
    std::array<DelayLine, 2> ap;
    float drive = 1.0f, asym = 0.0f, apCoef = 0.0f, hold = 0.0f, holdPhase = 0.0f, lofiInc = 0.2f;
    bool hpActive = false, lpActive = false, steepLp = false, bumpActive = false, driveActive = false, lofi = false;
};

//==============================================================================
class Delay
{
public:
    static constexpr double kMaxTimeMs = 2000.0;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        // Segment 0 also serves Rhythm mode, whose loop can be four delay times long.
        seg[0].line.resize ((int) (sr * (kMaxTimeMs * 0.001 * 4.0 * 1.04 + 0.05)) + 64);
        for (int i = 1; i < 4; ++i)
            seg[(size_t) i].line.resize ((int) (sr * (kMaxTimeMs * 0.001 * 1.6 + 0.05)) + 64);
        for (auto& s : seg)
            s.character.prepare (sr);

        const float t = (float) (0.375 * sr);
        timeL.reset (sr, 180.0, t);
        timeR.reset (sr, 180.0, t);
        grooveSm.reset (sr, 180.0, 0.0f);
        mixSm.reset (sr, 30.0, 0.3f);
        fbSm.reset (sr, 30.0, 0.4f);
        outSm.reset (sr, 30.0, 1.0f);
        duckAttack = onePoleCoef (4.0, sr);
        duckRelease = onePoleCoef (280.0, sr);
        reset();
        first = true;
        update();
    }

    void reset() noexcept
    {
        for (auto& s : seg)
        {
            s.line.clear();
            s.character.reset();
        }
        duckEnv = 0.0f;
        wowPhase = flutterPhase = 0.0f;
        drift = driftTarget = 0.0f;
    }

    void setParams (const DelayParams& p) noexcept
    {
        params = p;
        update();
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        const DelayMode mode = params.mode;
        const auto& pat = rhythmPattern (params.pattern);

        for (int n = 0; n < numSamples; ++n)
        {
            const float dryL = left[n];
            const float dryR = stereo ? right[n] : dryL;
            const float fb = fbSm.next();
            const float tL = timeL.next();
            const float tR = timeR.next();
            const float g = grooveSm.next();

            // --- tape-style speed wobble -------------------------------------------
            float wob = 0.0f;
            if (wobbleDepth > 0.0f)
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
                wob = wobbleDepth * (std::sin ((float) kTwoPi * wowPhase)
                                     + 0.12f * std::sin ((float) kTwoPi * flutterPhase) + driftAmount * drift);
            }

            float wetL = 0.0f, wetR = 0.0f;

            if (mode == DelayMode::Rhythm)
            {
                const float in = 0.5f * (dryL + dryR);
                const float loop = tL * (float) pat.steps;
                const float end = seg[0].line.readCubic (std::max (3.0f, loop + wob * (float) pat.steps));
                for (int k = 0; k < pat.numTaps; ++k)
                {
                    const float v = seg[0].line.readLinear (std::max (1.0f, (tL + wob) * pat.position[k])) * pat.level[k];
                    wetL += v * (1.0f - std::max (0.0f, pat.pan[k]));
                    wetR += v * (1.0f + std::min (0.0f, pat.pan[k]));
                }
                seg[0].line.write (seg[0].character.process (in + fb * end));
            }
            else if (mode == DelayMode::PingPong)
            {
                const float in = 0.5f * (dryL + dryR);
                const float a = seg[0].line.readCubic (std::max (3.0f, tL * (1.0f + g) + wob));
                const float b = seg[1].line.readCubic (std::max (3.0f, tR * (1.0f - g) + wob));
                seg[0].line.write (seg[0].character.process (in + fb * b));
                seg[1].line.write (seg[1].character.process (fb * a));
                wetL = a;
                wetR = b;
            }
            else
            {
                // Single and Dual: each channel is a two-segment loop. With Groove at zero
                // the segments are equal and it behaves as one plain delay; moving Groove
                // shifts every other repeat off the grid.
                const float tRight = mode == DelayMode::Single ? tL : tR;
                const float la = seg[0].line.readCubic (std::max (3.0f, tL * (1.0f + g) + wob));
                const float lb = seg[1].line.readCubic (std::max (3.0f, tL * (1.0f - g) + wob));
                const float ra = seg[2].line.readCubic (std::max (3.0f, tRight * (1.0f + g) + wob));
                const float rb = seg[3].line.readCubic (std::max (3.0f, tRight * (1.0f - g) + wob));
                seg[0].line.write (seg[0].character.process (dryL + fb * lb));
                seg[1].line.write (seg[1].character.process (fb * la));
                seg[2].line.write (seg[2].character.process (dryR + fb * rb));
                seg[3].line.write (seg[3].character.process (fb * ra));
                wetL = la + lb;
                wetR = ra + rb;
            }

            // --- ducking: the echoes step back while the dry signal is playing ----
            if (params.ducking > 0.0f)
            {
                const float lvl = std::max (std::abs (dryL), std::abs (dryR));
                duckEnv = lvl > duckEnv ? lvl + (duckEnv - lvl) * duckAttack : lvl + (duckEnv - lvl) * duckRelease;
                const float amount = clampv (duckEnv * 5.0f, 0.0f, 1.0f);
                const float duck = 1.0f - params.ducking * amount;
                wetL *= duck;
                wetR *= duck;
            }

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

        for (auto& s : seg)
            s.character.sanitise();
        duckEnv = undenorm (duckEnv);
    }

private:
    struct Segment
    {
        DelayLine line;
        DelayCharacter character;
    };

    void update() noexcept
    {
        const float feelScale = 1.0f + 0.035f * clampv (params.feel, -1.0f, 1.0f);
        const float maxSamples = (float) (kMaxTimeMs * 0.001 * sr);
        timeL.setTarget (clampv ((float) (params.timeLMs * 0.001 * sr) * feelScale, 8.0f, maxSamples * 1.04f));
        timeR.setTarget (clampv ((float) (params.timeRMs * 0.001 * sr) * feelScale, 8.0f, maxSamples * 1.04f));
        grooveSm.setTarget (0.5f * clampv (params.groove, -1.0f, 1.0f));
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));
        fbSm.setTarget (clampv (params.feedback, 0.0f, 1.1f));
        outSm.setTarget (dbToGain (params.outputDb));

        for (auto& s : seg)
            s.character.configure (params.style, params.lowCutHz, params.highCutHz, params.saturation, params.diffusion);

        // How much each style wobbles for a given setting of the Wobble control.
        float styleWobble = 1.0f;
        driftAmount = 0.0f;
        switch (params.style)
        {
            case DelayStyle::Digital:    styleWobble = 0.6f; break;
            case DelayStyle::StudioTape: styleWobble = 1.0f; driftAmount = 0.15f; break;
            case DelayStyle::WornTape:   styleWobble = 2.2f; driftAmount = 0.6f; break;
            case DelayStyle::Analog:     styleWobble = 0.8f; break;
            case DelayStyle::Tube:       styleWobble = 1.2f; driftAmount = 0.3f; break;
            case DelayStyle::LoFi:       styleWobble = 1.5f; driftAmount = 0.4f; break;
            case DelayStyle::Telephone:  styleWobble = 0.5f; break;
            case DelayStyle::Diffuse:    styleWobble = 1.0f; break;
            case DelayStyle::Count: break;
        }
        wobbleDepth = (float) (params.wobble * params.wobble * styleWobble * 0.0011 * sr);
        const double rate = clampv ((double) params.wobbleRateHz, 0.05, 10.0);
        wowInc = (float) (rate / sr);
        flutterInc = (float) (rate * 8.3 / sr);
        driftCoef = 1.0f - (float) std::exp (-kTwoPi * rate * 2.0 / sr);

        if (first)
        {
            timeL.snap();
            timeR.snap();
            grooveSm.snap();
            mixSm.snap();
            fbSm.snap();
            outSm.snap();
            first = false;
        }
    }

    double sr = 44100.0;
    DelayParams params;
    bool first = true;
    std::array<Segment, 4> seg;
    Smoothed timeL, timeR, grooveSm, mixSm, fbSm, outSm;
    float wobbleDepth = 0.0f, wowInc = 0.0f, flutterInc = 0.0f, wowPhase = 0.0f, flutterPhase = 0.0f;
    float drift = 0.0f, driftTarget = 0.0f, driftCoef = 0.0f, driftAmount = 0.0f;
    float duckEnv = 0.0f, duckAttack = 0.0f, duckRelease = 0.0f;
    Random rng { 0xBEEF1234u };
};
} // namespace gitto
