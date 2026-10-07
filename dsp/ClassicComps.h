// Gitto FX Opto Comp, FET Comp and Bus Comp - three single-purpose compressors, each
// built around one classic gain-reduction behaviour.
#pragma once

#include "Common.h"
#include <atomic>

namespace gitto
{
// Pieces all three share: sidechain high-pass, meters, and a smoothed dry/wet and gain.
struct CompShell
{
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        inMeter.prepare (sr);
        outMeter.prepare (sr);
        gainSm.reset (sr, 30.0, 1.0f);
        mixSm.reset (sr, 30.0, 1.0f);
        for (auto& f : hpf) f.reset();
        for (auto& d : dc) { d.reset(); d.setCutoff (8.0, sr); }
    }
    void setSidechainHpf (float hz) noexcept
    {
        hpfActive = hz > 21.0f;
        if (hpfActive)
        {
            const auto c = design::highpass (hz, 0.7071, sr);
            hpf[0].setCoefs (c);
            hpf[1].setCoefs (c);
        }
    }
    float detectorFilter (int ch, float x) noexcept { return hpfActive ? hpf[(size_t) ch].process (x) : x; }
    void publish (float grDb) noexcept
    {
        grMeter.store (grDb, std::memory_order_relaxed);
        inLevel.store (inMeter.value, std::memory_order_relaxed);
        outLevel.store (outMeter.value, std::memory_order_relaxed);
        hpf[0].sanitise();
        hpf[1].sanitise();
    }
    float getGainReductionDb() const noexcept { return grMeter.load (std::memory_order_relaxed); }
    float getInputLevel() const noexcept { return inLevel.load (std::memory_order_relaxed); }
    float getOutputLevel() const noexcept { return outLevel.load (std::memory_order_relaxed); }

    // Soft-knee gain computer: gain change in dB (<= 0) for a level `over` dB above threshold.
    static float curve (float over, float slope, float knee) noexcept
    {
        if (2.0f * over < -knee)
            return 0.0f;
        if (knee > 0.0f && 2.0f * std::abs (over) <= knee)
            return -slope * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);
        return -slope * over;
    }
    // How steeply that curve is falling at `over`: dB of extra reduction per dB of extra level.
    static float curveSlope (float over, float slope, float knee) noexcept
    {
        if (2.0f * over < -knee)
            return 0.0f;
        if (knee > 0.0f && 2.0f * std::abs (over) <= knee)
            return slope * (over + knee * 0.5f) / knee;
        return slope;
    }

    double sr = 44100.0;
    std::array<Biquad, 2> hpf;
    std::array<OnePole, 2> dc;
    bool hpfActive = false;
    Smoothed gainSm, mixSm;
    LevelFollower inMeter, outMeter;
    std::atomic<float> grMeter { 0.0f }, inLevel { 0.0f }, outLevel { 0.0f };
};

//==============================================================================
// Opto: a light-dependent cell. Reduction comes on gently and lets go in two stages,
// half of it quickly and the rest more slowly the longer it has been working.
struct OptoParams
{
    float reduction = 0.4f;   // 0..1 "peak reduction"
    float gainDb = 0.0f;      // -6..+36
    float ratio = 3.0f;       // 2..10
    bool manual = false;      // false = fixed program-dependent timing
    float attackMs = 10.0f;   // manual: 0.5..300
    float releaseMs = 500.0f; // manual: 50..10000
    float scHpfHz = 20.0f;
    float mix = 1.0f;
};

class OptoComp : public CompShell
{
public:
    void prepare (double sampleRate)
    {
        CompShell::prepare (sampleRate);
        rmsCoef = onePoleCoef (10.0, sr);
        reset();
        update();
        gainSm.snap();
        mixSm.snap();
    }
    void reset() noexcept
    {
        fast = slow = charge = rms = 0.0f;
    }
    void setParams (const OptoParams& p) noexcept
    {
        params = p;
        update();
    }
    float thresholdDb() const noexcept { return -48.0f * clampv (params.reduction, 0.0f, 1.0f); }
    float staticCurve (float levelDb) const noexcept
    {
        return curve (levelDb - thresholdDb(), 1.0f - 1.0f / clampv (params.ratio, 1.0f, 20.0f), 12.0f);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = left[i], inR = stereo ? right[i] : inL;
            inMeter.push (std::max (std::abs (inL), std::abs (inR)));

            const float dL = detectorFilter (0, inL), dR = detectorFilter (1, inR);
            const float sq = std::max (dL * dL, dR * dR);
            rms = sq + (rms - sq) * rmsCoef;
            const float target = staticCurve (gainToDb (std::sqrt (rms)));

            float grDb;
            if (params.manual)
            {
                fast = target < fast ? target + (fast - target) * manAttack : target + (fast - target) * manRelease;
                grDb = fast;
            }
            else
            {
                // Fast half: about 10 ms on, 60 ms off.
                fast = target < fast ? target + (fast - target) * fastAttack : target + (fast - target) * fastRelease;
                // Slow half: its release stretches from half a second to five seconds as
                // the cell "charges" under sustained reduction.
                charge = -fast * 0.1f + (charge + fast * 0.1f) * chargeCoef;
                const float c = clampv (charge, 0.0f, 1.0f);
                const float rel = slowReleaseShort + (slowReleaseLong - slowReleaseShort) * c;
                slow = target < slow ? target + (slow - target) * slowAttack : target + (slow - target) * rel;
                grDb = 0.5f * (fast + slow);
            }
            const float g = dbToGain (grDb);
            const float mk = gainSm.next(), mx = mixSm.next();

            float yL = inL * g, yR = inR * g;
            // Tube make-up stage: a touch of second harmonic that grows with level.
            // (The squared term is held past +12 dBFS so the curve can never turn back on itself.)
            const float cL = clampv (yL, -4.0f, 4.0f), cR = clampv (yR, -4.0f, 4.0f);
            yL = dc[0].highpass (yL + 0.04f * cL * cL);
            yR = dc[1].highpass (yR + 0.04f * cR * cR);
            yL = inL + (yL * mk - inL) * mx;
            yR = inR + (yR * mk - inR) * mx;

            left[i] = yL;
            if (stereo)
                right[i] = yR;
            outMeter.push (std::max (std::abs (yL), std::abs (yR)));
            lastGr = grDb;
        }
        fast = undenorm (fast);
        slow = undenorm (slow);
        publish (lastGr);
    }

private:
    void update() noexcept
    {
        fastAttack = onePoleCoef (10.0, sr);
        fastRelease = onePoleCoef (60.0, sr);
        slowAttack = onePoleCoef (25.0, sr);
        slowReleaseShort = onePoleCoef (500.0, sr);
        slowReleaseLong = onePoleCoef (5000.0, sr);
        chargeCoef = onePoleCoef (2500.0, sr);
        manAttack = onePoleCoef (clampv (params.attackMs, 0.5f, 300.0f), sr);
        manRelease = onePoleCoef (clampv (params.releaseMs, 50.0f, 10000.0f), sr);
        setSidechainHpf (params.scHpfHz);
        gainSm.setTarget (dbToGain (params.gainDb));
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));
    }

    OptoParams params;
    float fast = 0.0f, slow = 0.0f, charge = 0.0f, rms = 0.0f, lastGr = 0.0f;
    float rmsCoef = 0.0f, fastAttack = 0.0f, fastRelease = 0.0f, slowAttack = 0.0f;
    float slowReleaseShort = 0.0f, slowReleaseLong = 0.0f, chargeCoef = 0.0f, manAttack = 0.0f, manRelease = 0.0f;
};

//==============================================================================
// FET: fixed threshold, driven by input gain, with a very fast feedback detector.
struct FetParams
{
    float inputDb = 0.0f;     // -12..+36
    float outputDb = 0.0f;    // -36..+12
    float attack = 0.5f;      // 0..1 -> 0.02..0.8 ms
    float releaseMs = 250.0f; // 50..1100
    int ratio = 0;            // 0=4, 1=8, 2=12, 3=20, 4=All
    float scHpfHz = 20.0f;
    float mix = 1.0f;
};

class FetComp : public CompShell
{
public:
    void prepare (double sampleRate)
    {
        CompShell::prepare (sampleRate);
        inSm.reset (sr, 30.0, 1.0f);
        reset();
        update();
        gainSm.snap();
        mixSm.snap();
        inSm.snap();
    }
    void reset() noexcept
    {
        gr = 0.0f;
        peak = 0.0f;
    }
    void setParams (const FetParams& p) noexcept
    {
        params = p;
        update();
    }
    float attackMs() const noexcept { return 0.02f * std::pow (40.0f, clampv (params.attack, 0.0f, 1.0f)); }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        for (int i = 0; i < numSamples; ++i)
        {
            const float dryL = left[i], dryR = stereo ? right[i] : dryL;
            const float ig = inSm.next();
            const float inL = dryL * ig, inR = dryR * ig;
            inMeter.push (std::max (std::abs (inL), std::abs (inR)));

            // Feedback detection: the detector listens to the compressor's own output, which is
            // the input level plus the gain reduction being decided right now. Solving for that
            // reduction within the sample (one linearised implicit step) keeps the loop stable at
            // any ratio and lets the fastest attack really be a sample or two, as on the hardware;
            // the usual one-sample-late loop would have to slow the attack down to stay stable.
            const float lvl = std::max (std::abs (detectorFilter (0, inL)), std::abs (detectorFilter (1, inR)));
            peak = std::max (lvl, peak * peakDecay);
            const float over = gainToDb (peak) + gr - threshold;
            const float target = curve (over, slope, knee);
            const float step = 1.0f - (target < gr ? attackCoef : releaseCoef);
            gr += step * (target - gr) / (1.0f + step * curveSlope (over, slope, knee));
            gr = undenorm (std::min (gr, 0.0f));
            const float g = dbToGain (gr);

            float yL = inL * g, yR = inR * g;

            // FET stage colour: mostly third harmonic, more of it the harder it works. For small
            // signals this is y - amt*y^3/3; unlike that polynomial it keeps rising for large ones,
            // so a transient that gets past a slow attack is squashed, never folded back.
            const float k = colour * (0.15f + clampv (-gr / 12.0f, 0.0f, 1.0f)) * 0.6667f;
            yL /= std::sqrt (1.0f + k * yL * yL);
            yR /= std::sqrt (1.0f + k * yR * yR);

            const float og = gainSm.next(), mx = mixSm.next();
            yL = dryL + (yL * og - dryL) * mx;
            yR = dryR + (yR * og - dryR) * mx;
            left[i] = yL;
            if (stereo)
                right[i] = yR;
            outMeter.push (std::max (std::abs (yL), std::abs (yR)));
        }
        publish (gr);
    }

private:
    void update() noexcept
    {
        static const float ratios[5] = { 4.0f, 8.0f, 12.0f, 20.0f, 20.0f };
        // Referred to digital full scale, so that Input at 0 dB starts to work on peaks around
        // -12 dBFS: ordinary track levels in a DAW, with room to back off on hot ones.
        static const float thresholds[5] = { -12.0f, -10.0f, -8.0f, -6.0f, -14.0f };
        const int r = clampv (params.ratio, 0, 4);
        const bool all = r == 4;
        threshold = thresholds[r];
        knee = all ? 2.0f : 5.0f;
        slope = std::min (ratios[r] - 1.0f, 19.0f); // feedback loop gain for this ratio
        colour = all ? 0.12f : 0.04f;

        const double release = clampv ((double) params.releaseMs, 50.0, 1100.0) * (all ? 0.6 : 1.0);
        // A feedback loop settles (1 + slope) times faster than its own smoothing, so slow the
        // smoothing by that much: the Attack time is then what you actually get at every ratio.
        attackCoef = onePoleCoef (attackMs() * (1.0f + slope), sr);
        releaseCoef = onePoleCoef (release, sr);
        peakDecay = onePoleCoef (std::min (12.0, release * 0.5), sr);

        setSidechainHpf (params.scHpfHz);
        inSm.setTarget (dbToGain (params.inputDb));
        gainSm.setTarget (dbToGain (params.outputDb));
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));
    }

    FetParams params;
    Smoothed inSm;
    float gr = 0.0f, peak = 0.0f, threshold = -12.0f, knee = 5.0f, slope = 3.0f, colour = 0.04f;
    float attackCoef = 0.0f, releaseCoef = 0.0f, peakDecay = 0.0f;
};

//==============================================================================
// Bus: a VCA-style feedback compressor with stepped settings, made for whole mixes.
struct BusParams
{
    float thresholdDb = -16.0f; // -40..0
    int ratio = 2;              // 0=1.5, 1=2, 2=4, 3=10
    int attack = 3;             // 0.1, 0.3, 1, 3, 10, 30 ms
    int release = 4;            // 0.1, 0.3, 0.6, 1.2 s, Auto
    float makeupDb = 0.0f;      // 0..24
    float scHpfHz = 20.0f;      // 20..300
    float mix = 1.0f;
};

class BusComp : public CompShell
{
public:
    static constexpr int kNumRatios = 4, kNumAttacks = 6, kNumReleases = 5;
    static float ratioValue (int i) noexcept { static const float v[] = { 1.5f, 2.0f, 4.0f, 10.0f }; return v[clampv (i, 0, 3)]; }
    static float attackValue (int i) noexcept { static const float v[] = { 0.1f, 0.3f, 1.0f, 3.0f, 10.0f, 30.0f }; return v[clampv (i, 0, 5)]; }
    static float releaseValue (int i) noexcept { static const float v[] = { 100.0f, 300.0f, 600.0f, 1200.0f, 0.0f }; return v[clampv (i, 0, 4)]; }

    void prepare (double sampleRate)
    {
        CompShell::prepare (sampleRate);
        rmsCoef = onePoleCoef (10.0, sr);
        reset();
        update();
        gainSm.snap();
        mixSm.snap();
    }
    void reset() noexcept
    {
        gr = slowGr = rms = 0.0f;
        last = { 0.0f, 0.0f };
    }
    void setParams (const BusParams& p) noexcept
    {
        params = p;
        update();
    }
    float staticCurve (float levelDb) const noexcept
    {
        const float r = ratioValue (params.ratio);
        return curve (levelDb - params.thresholdDb, 1.0f - 1.0f / r, knee);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = left[i], inR = stereo ? right[i] : inL;
            inMeter.push (std::max (std::abs (inL), std::abs (inR)));

            // Feedback RMS detector on the (linked) output.
            const float dL = detectorFilter (0, last[0]), dR = detectorFilter (1, last[1]);
            const float sq = std::max (dL * dL, dR * dR);
            rms = sq + (rms - sq) * rmsCoef;
            // The detector sees the output, so the slope is the feedback equivalent of the ratio.
            const float target = curve (gainToDb (std::sqrt (rms)) - params.thresholdDb, fbSlope, knee);

            if (target < gr)
            {
                gr = target + (gr - target) * attackCoef;
            }
            else if (autoRelease)
            {
                // Short dips recover quickly; sustained reduction lets go slowly.
                slowGr = gr < slowGr ? gr + (slowGr - gr) * slowAttack : gr + (slowGr - gr) * slowRelease;
                const float sustained = clampv (-slowGr / 8.0f, 0.0f, 1.0f);
                gr = target + (gr - target) * (autoFast + (autoSlow - autoFast) * sustained);
            }
            else
            {
                gr = target + (gr - target) * releaseCoef;
            }
            gr = undenorm (gr);
            const float g = dbToGain (gr);
            const float yL = inL * g, yR = inR * g;
            last[0] = yL;
            last[1] = yR;

            const float mk = gainSm.next(), mx = mixSm.next();
            const float oL = inL + (yL * mk - inL) * mx;
            const float oR = inR + (yR * mk - inR) * mx;
            left[i] = oL;
            if (stereo)
                right[i] = oR;
            outMeter.push (std::max (std::abs (oL), std::abs (oR)));
        }
        slowGr = undenorm (slowGr);
        publish (gr);
    }

private:
    void update() noexcept
    {
        const float r = ratioValue (params.ratio);
        fbSlope = r - 1.0f;
        knee = r < 2.5f ? 10.0f : (r < 6.0f ? 6.0f : 3.0f);
        attackCoef = onePoleCoef (attackValue (params.attack), sr);
        const float maxStep = 0.85f / std::max (fbSlope, 1.0e-3f);
        if (1.0f - attackCoef > maxStep)
            attackCoef = 1.0f - maxStep;
        const float rel = releaseValue (params.release);
        autoRelease = rel <= 0.0f;
        releaseCoef = onePoleCoef (autoRelease ? 300.0 : (double) rel, sr);
        autoFast = onePoleCoef (120.0, sr);
        autoSlow = onePoleCoef (1500.0, sr);
        slowAttack = onePoleCoef (300.0, sr);
        slowRelease = onePoleCoef (2500.0, sr);
        setSidechainHpf (params.scHpfHz);
        gainSm.setTarget (dbToGain (params.makeupDb));
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));
    }

    BusParams params;
    std::array<float, 2> last { 0.0f, 0.0f };
    float gr = 0.0f, slowGr = 0.0f, rms = 0.0f, rmsCoef = 0.0f;
    float fbSlope = 3.0f, knee = 6.0f, attackCoef = 0.0f, releaseCoef = 0.0f;
    float autoFast = 0.0f, autoSlow = 0.0f, slowAttack = 0.0f, slowRelease = 0.0f;
    bool autoRelease = false;
};
} // namespace gitto
