// Gitto FX Shaper - a drawn curve, looped in time with the song, that moves volume,
// a filter, pan and stereo width.
#pragma once

#include "Common.h"
#include <atomic>

namespace gitto
{
enum class ShaperTrigger { Sync = 0, Free, Audio, Count };
enum class ShaperFilter { LowPass = 0, HighPass, BandPass, Count };

struct ShaperParams
{
    int shape = 1;              // 0 = the user's own curve, 1.. = built-in shapes
    int length = 2;             // index into kLengths (beats per loop)
    ShaperTrigger trigger = ShaperTrigger::Sync;
    float volume = 1.0f;        // 0..1 depth
    float filter = 0.0f;        // 0..1 depth
    ShaperFilter filterType = ShaperFilter::LowPass;
    float filterLowHz = 200.0f;   // 20..5000
    float filterHighHz = 16000.0f; // 200..20000
    float resonance = 0.2f;     // 0..1
    float pan = 0.0f;           // 0..1 depth
    float width = 0.0f;         // 0..1 depth
    float smooth = 0.15f;       // 0..1 -> 0..50 ms
    float thresholdDb = -24.0f; // audio trigger sensitivity
    float mix = 1.0f;
};

struct ShaperShape
{
    const char* name;
    int count;
    float x[34], y[34];
};

class Shaper
{
public:
    static constexpr int kTable = 512;
    static constexpr int kNumLengths = 7;
    static constexpr int kNumShapes = 10; // built-in shapes; shape parameter is 1..kNumShapes

    static float lengthBeats (int i) noexcept
    {
        static const float beats[kNumLengths] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };
        return beats[clampv (i, 0, kNumLengths - 1)];
    }

    static const ShaperShape& builtIn (int index) noexcept
    {
        static const ShaperShape shapes[kNumShapes] = {
            { "Duck", 5, { 0.0f, 0.08f, 0.35f, 0.6f, 1.0f }, { 0.0f, 0.05f, 0.75f, 0.97f, 1.0f } },
            { "Pump", 4, { 0.0f, 0.22f, 0.4f, 1.0f }, { 0.0f, 0.88f, 1.0f, 1.0f } },
            { "Half Gate", 4, { 0.0f, 0.5f, 0.5f, 1.0f }, { 1.0f, 1.0f, 0.0f, 0.0f } },
            { "Gate x4", 16, { 0.0f, 0.125f, 0.125f, 0.25f, 0.25f, 0.375f, 0.375f, 0.5f, 0.5f, 0.625f, 0.625f, 0.75f, 0.75f, 0.875f, 0.875f, 1.0f },
                             { 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0 } },
            { "Trance Gate", 32, { 0.0f, 0.0625f, 0.0625f, 0.125f, 0.125f, 0.1875f, 0.1875f, 0.25f, 0.25f, 0.3125f, 0.3125f, 0.375f, 0.375f, 0.4375f, 0.4375f, 0.5f,
                                   0.5f, 0.5625f, 0.5625f, 0.625f, 0.625f, 0.6875f, 0.6875f, 0.75f, 0.75f, 0.8125f, 0.8125f, 0.875f, 0.875f, 0.9375f, 0.9375f, 1.0f },
                                 { 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1 } },
            { "Saw Down", 2, { 0.0f, 1.0f }, { 1.0f, 0.0f } },
            { "Saw Up", 2, { 0.0f, 1.0f }, { 0.0f, 1.0f } },
            { "Triangle", 3, { 0.0f, 0.5f, 1.0f }, { 0.0f, 1.0f, 0.0f } },
            { "Stairs", 8, { 0.0f, 0.25f, 0.25f, 0.5f, 0.5f, 0.75f, 0.75f, 1.0f }, { 1.0f, 1.0f, 0.66f, 0.66f, 0.33f, 0.33f, 0.0f, 0.0f } },
            { "Swell", 4, { 0.0f, 0.75f, 0.85f, 1.0f }, { 0.0f, 0.9f, 1.0f, 0.0f } },
        };
        return shapes[clampv (index, 0, kNumShapes - 1)];
    }

    // Turns a list of points (sorted by x, 0..1) into an evenly spaced table.
    static void buildTable (const float* xs, const float* ys, int count, float* table) noexcept
    {
        if (count <= 0)
        {
            for (int i = 0; i <= kTable; ++i)
                table[i] = 1.0f;
            return;
        }
        int seg = 0;
        for (int i = 0; i <= kTable; ++i)
        {
            const float x = (float) i / (float) kTable;
            while (seg < count - 1 && xs[seg + 1] <= x)
                ++seg;
            if (x <= xs[0])
                table[i] = ys[0];
            else if (seg >= count - 1)
                table[i] = ys[count - 1];
            else
            {
                const float span = xs[seg + 1] - xs[seg];
                const float t = span > 1.0e-6f ? (x - xs[seg]) / span : 1.0f;
                table[i] = ys[seg] + (ys[seg + 1] - ys[seg]) * t;
            }
            table[i] = clampv (table[i], 0.0f, 1.0f);
        }
    }

    Shaper()
    {
        // Until a curve is supplied, "own curve" means fully open.
        for (auto& t : custom)
            t.fill (1.0f);
        builtInTable.fill (1.0f);
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        mixSm.reset (sr, 30.0, 1.0f);
        envFast = onePoleCoef (1.0, sr);
        envSlow = onePoleCoef (60.0, sr);
        retriggerGap = (int) (0.03 * sr);
        reset();
        update();
        mixSm.snap();
    }

    void reset() noexcept
    {
        for (auto& s : svf) s = {};
        value = 1.0f;
        fast = slow = 0.0f;
        sinceTrigger = retriggerGap + 1;
        oneShotDone = true;
        phase = 0.0;
        filterCount = 0;
    }

    void setParams (const ShaperParams& p) noexcept
    {
        const bool shapeChanged = p.shape != params.shape;
        params = p;
        if (shapeChanged || ! builtInReady)
        {
            const auto& s = builtIn (std::max (0, params.shape - 1));
            buildTable (s.x, s.y, s.count, builtInTable.data());
            builtInReady = true;
        }
        update();
    }

    // Called from the editor thread when the user's own curve changes.
    void setCustomTable (const float* table) noexcept
    {
        const int next = 1 - customIndex.load (std::memory_order_relaxed);
        std::copy (table, table + kTable + 1, custom[(size_t) next].begin());
        customIndex.store (next, std::memory_order_release);
    }

    // Song position at the start of the next block. beatsPerSecond drives the loop even
    // when the host is stopped, so the effect can still be auditioned.
    void setTransport (double ppqPosition, double bpm, bool playing) noexcept
    {
        const double beats = lengthBeats (params.length);
        phaseInc = (bpm / 60.0) / (beats * sr);
        if (playing && params.trigger == ShaperTrigger::Sync)
        {
            double p = ppqPosition / beats;
            phase = p - std::floor (p);
        }
    }

    float getPhase() const noexcept { return phaseOut.load (std::memory_order_relaxed); }
    float getValue() const noexcept { return valueOut.load (std::memory_order_relaxed); }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        const float* table = params.shape == 0 ? custom[(size_t) customIndex.load (std::memory_order_acquire)].data() : builtInTable.data();
        const bool audioTrig = params.trigger == ShaperTrigger::Audio;
        const float threshold = dbToGain (params.thresholdDb);

        for (int i = 0; i < numSamples; ++i)
        {
            const float dryL = left[i], dryR = stereo ? right[i] : dryL;

            if (audioTrig)
            {
                // A hit is a fast rise well above the recent average, louder than the threshold.
                const float lvl = std::max (std::abs (dryL), std::abs (dryR));
                fast = lvl > fast ? lvl : lvl + (fast - lvl) * envFast;
                slow = lvl + (slow - lvl) * envSlow;
                if (sinceTrigger <= retriggerGap)
                    ++sinceTrigger;
                if (fast > threshold && fast > 1.8f * slow && sinceTrigger > retriggerGap)
                {
                    phase = 0.0;
                    oneShotDone = false;
                    sinceTrigger = 0;
                }
                if (! oneShotDone)
                {
                    phase += phaseInc;
                    if (phase >= 1.0)
                    {
                        phase = 1.0;
                        oneShotDone = true;
                    }
                }
            }
            else
            {
                phase += phaseInc;
                if (phase >= 1.0)
                    phase -= std::floor (phase);
            }

            const float pos = (float) phase * (float) kTable;
            const int idx = std::min ((int) pos, kTable - 1);
            const float target = table[idx] + (table[idx + 1] - table[idx]) * (pos - (float) idx);
            value = target + (value - target) * smoothCoef;
            const float y = value;

            float l = dryL, r = dryR;

            if (params.filter > 0.005f)
            {
                if (filterCount == 0)
                    updateFilter (y);
                filterCount = (filterCount + 1) & 7;
                l = runFilter (0, l);
                r = runFilter (1, r);
            }

            if (params.volume > 0.0f)
            {
                const float g = 1.0f - params.volume * (1.0f - y);
                l *= g;
                r *= g;
            }

            if (stereo)
            {
                if (params.width > 0.0f)
                {
                    const float w = 1.0f + params.width * (2.0f * y - 1.0f);
                    const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * w;
                    l = mid + side;
                    r = mid - side;
                }
                if (params.pan > 0.0f)
                {
                    const float p = params.pan * (2.0f * y - 1.0f);
                    l *= std::min (1.0f, 1.0f - p);
                    r *= std::min (1.0f, 1.0f + p);
                }
            }

            const float mx = mixSm.next();
            left[i] = dryL + (l - dryL) * mx;
            if (stereo)
                right[i] = dryR + (r - dryR) * mx;
        }

        phaseOut.store ((float) phase, std::memory_order_relaxed);
        valueOut.store (value, std::memory_order_relaxed);
        for (auto& s : svf)
        {
            s.ic1 = undenorm (s.ic1);
            s.ic2 = undenorm (s.ic2);
        }
    }

private:
    struct Svf { float ic1 = 0.0f, ic2 = 0.0f; };

    void updateFilter (float y) noexcept
    {
        const float lo = clampv (params.filterLowHz, 20.0f, 8000.0f);
        const float hi = clampv (params.filterHighHz, lo * 1.5f, (float) (sr * 0.45));
        const float depth = clampv (params.filter, 0.0f, 1.0f);
        float cutoff;
        switch (params.filterType)
        {
            case ShaperFilter::LowPass:  cutoff = hi * std::pow (lo / hi, depth * (1.0f - y)); break;  // opens as the curve rises
            case ShaperFilter::HighPass: cutoff = lo * std::pow (hi / lo, depth * (1.0f - y)); break;  // thins as the curve falls
            default:                     cutoff = lo * std::pow (hi / lo, y); break;
        }
        // Trapezoidal state-variable filter: stays stable however fast the cutoff moves.
        const float g = std::tan ((float) kPi * clampv (cutoff, 20.0f, (float) (sr * 0.45)) / (float) sr);
        const float k = 1.0f / (0.5f + 7.5f * clampv (params.resonance, 0.0f, 1.0f));
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
        svfK = k;
        bandBlend = params.filterType == ShaperFilter::BandPass ? depth : 1.0f;
    }

    float runFilter (int ch, float x) noexcept
    {
        auto& s = svf[(size_t) ch];
        const float v3 = x - s.ic2;
        const float v1 = a1 * s.ic1 + a2 * v3;
        const float v2 = s.ic2 + a2 * s.ic1 + a3 * v3;
        s.ic1 = 2.0f * v1 - s.ic1;
        s.ic2 = 2.0f * v2 - s.ic2;
        switch (params.filterType)
        {
            case ShaperFilter::LowPass:  return v2;
            case ShaperFilter::HighPass: return x - svfK * v1 - v2;
            default:                     return x + (svfK * v1 - x) * bandBlend;
        }
    }

    void update() noexcept
    {
        smoothCoef = onePoleCoef (0.2 + 50.0 * clampv ((double) params.smooth, 0.0, 1.0), sr);
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));
    }

    double sr = 44100.0;
    ShaperParams params;
    std::array<float, kTable + 1> builtInTable {};
    std::array<std::array<float, kTable + 1>, 2> custom {};
    std::atomic<int> customIndex { 0 };
    bool builtInReady = false;
    double phase = 0.0, phaseInc = 0.0;
    float value = 1.0f, smoothCoef = 0.0f;
    float fast = 0.0f, slow = 0.0f, envFast = 0.0f, envSlow = 0.0f;
    int sinceTrigger = 1001, retriggerGap = 1000, filterCount = 0;
    bool oneShotDone = true;
    std::array<Svf, 2> svf;
    float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, svfK = 1.0f, bandBlend = 1.0f;
    Smoothed mixSm;
    std::atomic<float> phaseOut { 0.0f }, valueOut { 1.0f };
};
} // namespace gitto
