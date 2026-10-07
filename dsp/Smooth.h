// Gitto FX Smooth - spectral resonance suppressor. Many times a second it compares a
// detailed view of the spectrum with a heavily smoothed one and turns down whatever
// sticks out of the smooth shape: a ringing room mode, a harsh formant, a whistling synth.
#pragma once

#include "Blocks.h"
#include <atomic>

namespace gitto
{
struct SmoothParams
{
    float depth = 0.5f;        // 0..1
    float sharpness = 0.5f;    // 0..1: broad cuts .. narrow cuts
    float selectivity = 0.4f;  // 0..1: everything that pokes out .. only the worst offenders
    float attackMs = 8.0f;     // 1..100
    float releaseMs = 60.0f;   // 10..500
    float lowHz = 150.0f;      // 20..5000: bottom of the range it works on
    float highHz = 16000.0f;   // 1000..20000: top of the range
    float mix = 1.0f;
    float outputDb = 0.0f;
    bool delta = false;        // hear only what is being removed
};

class Smooth
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        // About 23 Hz per bin at every sample rate: fine enough to tell a resonance from its
        // neighbours in the low mids, at the cost of about 43 ms of latency.
        size = sr > 100000.0 ? 8192 : (sr > 50000.0 ? 4096 : 2048);
        hop = size / 4;
        bins = size / 2 + 1;
        fft.prepare (size);
        window.resize ((size_t) size);
        for (int i = 0; i < size; ++i)
            window[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (kTwoPi * i / size)); // root-Hann, used twice
        for (auto& ch : chans)
        {
            ch.in.assign ((size_t) size, 0.0f);
            ch.out.assign ((size_t) size, 0.0f);
            ch.spec.assign ((size_t) size, {});
            ch.dry.resize (size + 8);
        }
        magDb.assign ((size_t) bins, -120.0f);
        prefix.assign ((size_t) bins + 1, 0.0);
        raw.assign ((size_t) bins, 0.0f);
        power.assign ((size_t) bins, 0.0f);
        fine.assign ((size_t) bins, 0.0f);
        reduction.assign ((size_t) bins, 0.0f);
        displayMag.assign ((size_t) bins, -120.0f);
        displayRed.assign ((size_t) bins, 0.0f);
        mixSm.reset (sr, 30.0, 1.0f);
        outSm.reset (sr, 30.0, 1.0f);
        reset();
        update();
        mixSm.snap();
        outSm.snap();
    }

    void reset() noexcept
    {
        for (auto& ch : chans)
        {
            std::fill (ch.in.begin(), ch.in.end(), 0.0f);
            std::fill (ch.out.begin(), ch.out.end(), 0.0f);
            ch.dry.clear();
        }
        std::fill (reduction.begin(), reduction.end(), 0.0f);
        std::fill (power.begin(), power.end(), 0.0f);
        pos = 0;
        count = 0;
    }

    void setParams (const SmoothParams& p) noexcept
    {
        params = p;
        update();
    }

    int getLatencySamples() const noexcept { return size; }
    int getNumBins() const noexcept { return bins; }
    double binHz() const noexcept { return sr / (double) size; }
    // Latest analysis frame for the display (read from the UI thread; a torn read is harmless).
    const std::vector<float>& getSpectrumDb() const noexcept { return displayMag; }
    const std::vector<float>& getReductionDb() const noexcept { return displayRed; }
    float getMaxReductionDb() const noexcept { return maxRed.load (std::memory_order_relaxed); }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        const int numCh = stereo ? 2 : 1;
        float* io[2] = { left, right };

        for (int i = 0; i < numSamples; ++i)
        {
            const float mx = mixSm.next(), og = outSm.next();
            for (int c = 0; c < numCh; ++c)
            {
                auto& ch = chans[(size_t) c];
                const float x = io[c][i];
                ch.in[(size_t) pos] = x;
                const float wet = ch.out[(size_t) pos];
                ch.out[(size_t) pos] = 0.0f;
                ch.dry.write (x);
                const float dry = ch.dry.readInt (size + 1);
                const float y = params.delta ? dry - wet : dry + (wet - dry) * mx;
                io[c][i] = y * og;
            }
            pos = (pos + 1) % size;
            if (++count >= hop)
            {
                count = 0;
                processFrame (numCh);
            }
        }
    }

private:
    struct Channel
    {
        std::vector<float> in, out;
        std::vector<std::complex<float>> spec;
        DelayLine dry;
    };

    // Mean of v over [lo, hi] using the running-sum table.
    double meanOver (int lo, int hi) const noexcept
    {
        lo = clampv (lo, 0, bins - 1);
        hi = clampv (hi, lo, bins - 1);
        return (prefix[(size_t) hi + 1] - prefix[(size_t) lo]) / (double) (hi - lo + 1);
    }

    void buildPrefix (const std::vector<float>& v) noexcept
    {
        double acc = 0.0;
        prefix[0] = 0.0;
        for (int k = 0; k < bins; ++k)
        {
            acc += v[(size_t) k];
            prefix[(size_t) k + 1] = acc;
        }
    }

    void processFrame (int numCh) noexcept
    {
        // Analysis: the most recent `size` samples, oldest first.
        for (int c = 0; c < numCh; ++c)
        {
            auto& ch = chans[(size_t) c];
            for (int i = 0; i < size; ++i)
                ch.spec[(size_t) i] = ch.in[(size_t) ((pos + i) % size)] * window[(size_t) i];
            fft.forward (ch.spec.data());
        }

        // Linked power per bin, averaged over about 20 ms. A single frame of any noisy sound
        // has bins jumping around by several dB; a real resonance stays put, so a short
        // average separates the two.
        const float scale = 2.0f / (float) size;
        for (int k = 0; k < bins; ++k)
        {
            float p = std::norm (chans[0].spec[(size_t) k]);
            if (numCh > 1)
                p = 0.5f * (p + std::norm (chans[1].spec[(size_t) k]));
            p *= scale * scale;
            float& avg = power[(size_t) k];
            avg = p + (avg - p) * averageCoef;
            avg = undenorm (avg);
        }

        // Two views of that spectrum. The detailed one averages over a fraction of an octave
        // set by Sharpness; the smooth one averages over a whole octave and stands for the
        // overall tonal shape, which is left alone.
        buildPrefix (power);
        for (int k = 0; k < bins; ++k)
        {
            const int half = std::max (1, (int) std::lround ((float) k * detail));
            const float p = (float) meanOver (k - half, k + half);
            fine[(size_t) k] = p;
            magDb[(size_t) k] = 10.0f * std::log10 (p + 1.0e-14f);
        }
        for (int k = 0; k < bins; ++k)
        {
            const int below = std::max (4, (int) ((float) k * 0.29f)), above = std::max (4, (int) ((float) k * 0.41f));
            const float smooth = 10.0f * (float) std::log10 (meanOver (k - below, k + above) + 1.0e-14);
            // How far this part of the spectrum sticks out of the smooth shape, past the tolerance.
            const float excess = magDb[(size_t) k] - smooth - tolerance;
            // Ignore the noise floor: nothing to gain from "fixing" silence.
            const float gate = clampv ((magDb[(size_t) k] + 96.0f) / 12.0f, 0.0f, 1.0f);
            raw[(size_t) k] = clampv (excess * slope, 0.0f, ceiling) * gate * rangeWeight (k);
        }

        // Blend each cut with its two neighbours so the gains change smoothly across the
        // spectrum, then let it come and go at the Attack and Release times.
        float worst = 0.0f;
        for (int k = 0; k < bins; ++k)
        {
            const float target = 0.25f * raw[(size_t) std::max (0, k - 1)] + 0.5f * raw[(size_t) k] + 0.25f * raw[(size_t) std::min (bins - 1, k + 1)];
            float& r = reduction[(size_t) k];
            r = target > r ? target + (r - target) * attackCoef : target + (r - target) * releaseCoef;
            r = undenorm (r);
            worst = std::max (worst, r);
        }
        maxRed.store (-worst, std::memory_order_relaxed);
        std::copy (magDb.begin(), magDb.end(), displayMag.begin());
        std::copy (reduction.begin(), reduction.end(), displayRed.begin());

        // Resynthesis with the gains applied (mirrored on to the negative frequencies).
        const float norm = 0.5f; // root-Hann twice at 75% overlap sums to 2
        for (int c = 0; c < numCh; ++c)
        {
            auto& ch = chans[(size_t) c];
            for (int k = 0; k < bins; ++k)
            {
                const float g = dbToGain (-reduction[(size_t) k]);
                ch.spec[(size_t) k] *= g;
                if (k > 0 && k < size / 2)
                    ch.spec[(size_t) (size - k)] *= g;
            }
            fft.inverse (ch.spec.data());
            for (int i = 0; i < size; ++i)
                ch.out[(size_t) ((pos + i) % size)] += ch.spec[(size_t) i].real() * window[(size_t) i] * norm;
        }
    }

    // 1 inside the working range, fading to 0 over a third of an octave outside it.
    float rangeWeight (int k) const noexcept
    {
        const float hz = (float) k * (float) binHz();
        if (hz <= 0.0f)
            return 0.0f;
        const float lo = clampv (std::log2 (hz / lowEdge) * 3.0f + 1.0f, 0.0f, 1.0f);
        const float hi = clampv (std::log2 (highEdge / hz) * 3.0f + 1.0f, 0.0f, 1.0f);
        return lo * hi;
    }

    void update() noexcept
    {
        const float depth = clampv (params.depth, 0.0f, 1.0f);
        slope = 0.25f + 1.5f * depth;                    // dB of cut per dB of excess
        ceiling = 4.0f + 20.0f * depth;                  // most it will ever take out
        tolerance = 1.0f + 7.0f * clampv (params.selectivity, 0.0f, 1.0f);
        // Width of the detailed view, and so of each cut: a third of an octave at 0 (broad,
        // gentle), a twelfth at 1 (narrow, surgical).
        const float octaves = (1.0f / 3.0f) * std::pow (0.25f, clampv (params.sharpness, 0.0f, 1.0f));
        detail = std::pow (2.0f, octaves * 0.5f) - 1.0f;
        const double hopSeconds = (double) hop / sr;
        averageCoef = (float) std::exp (-hopSeconds / 0.02);
        attackCoef = (float) std::exp (-hopSeconds / (0.001 * clampv ((double) params.attackMs, 0.5, 200.0)));
        releaseCoef = (float) std::exp (-hopSeconds / (0.001 * clampv ((double) params.releaseMs, 5.0, 1000.0)));
        lowEdge = clampv (params.lowHz, 20.0f, 10000.0f);
        highEdge = clampv (params.highHz, lowEdge * 1.5f, 22000.0f);
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));
        outSm.setTarget (dbToGain (params.outputDb));
    }

    double sr = 44100.0;
    SmoothParams params;
    int size = 2048, hop = 512, bins = 1025, pos = 0, count = 0;
    Fft fft;
    std::vector<float> window, magDb, raw, power, fine, reduction, displayMag, displayRed;
    std::vector<double> prefix;
    std::array<Channel, 2> chans;
    float slope = 1.0f, ceiling = 12.0f, tolerance = 4.0f, detail = 0.06f, averageCoef = 0.6f;
    float attackCoef = 0.0f, releaseCoef = 0.0f, lowEdge = 150.0f, highEdge = 16000.0f;
    Smoothed mixSm, outSm;
    std::atomic<float> maxRed { 0.0f };
};
} // namespace gitto
