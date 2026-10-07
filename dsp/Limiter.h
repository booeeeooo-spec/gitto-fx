// Gitto FX Limiter - lookahead true-peak limiter with loudness metering.
#pragma once

#include "Common.h"
#include <atomic>

namespace gitto
{
//==============================================================================
// ITU-R BS.1770 loudness meter: momentary (400 ms), short-term (3 s) and gated
// integrated loudness.
class LoudnessMeter
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        // Stage 1: high shelf, stage 2: high-pass (K-weighting), designed for any sample rate.
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (kPi * f0 / sr), Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
            shelf.b1 = 2.0 * (K * K - Vh) / a0;
            shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
            shelf.a1 = 2.0 * (K * K - 1.0) / a0;
            shelf.a2 = (1.0 - K / Q + K * K) / a0;
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (kPi * f0 / sr);
            const double a0 = 1.0 + K / Q + K * K;
            hp.b0 = 1.0;
            hp.b1 = -2.0;
            hp.b2 = 1.0;
            hp.a1 = 2.0 * (K * K - 1.0) / a0;
            hp.a2 = (1.0 - K / Q + K * K) / a0;
        }
        hopSamples = std::max (1, (int) std::lround (sr * 0.1));
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : state)
            s = {};
        hopEnergy = 0.0;
        hopCount = 0;
        hops.fill (0.0);
        hopIndex = 0;
        hopsFilled = 0;
        histCount.fill (0);
        histEnergy.fill (0.0);
        momentary.store (-100.0f);
        shortTerm.store (-100.0f);
        integrated.store (-100.0f);
    }

    void requestReset() noexcept { resetFlag.store (true); }

    void process (const float* left, const float* right, int numSamples) noexcept
    {
        if (resetFlag.exchange (false))
            reset();

        for (int i = 0; i < numSamples; ++i)
        {
            const double l = weight (0, left[i]);
            const double r = right != nullptr ? weight (1, right[i]) : l;
            hopEnergy += right != nullptr ? l * l + r * r : l * l;
            if (++hopCount >= hopSamples)
                finishHop();
        }
    }

    float getMomentary() const noexcept { return momentary.load (std::memory_order_relaxed); }
    float getShortTerm() const noexcept { return shortTerm.load (std::memory_order_relaxed); }
    float getIntegrated() const noexcept { return integrated.load (std::memory_order_relaxed); }

private:
    struct State { double z1a = 0, z2a = 0, z1b = 0, z2b = 0; };

    double weight (int ch, double x) noexcept
    {
        auto& s = state[(size_t) ch];
        const double y1 = shelf.b0 * x + s.z1a;
        s.z1a = shelf.b1 * x - shelf.a1 * y1 + s.z2a;
        s.z2a = shelf.b2 * x - shelf.a2 * y1;
        const double y2 = hp.b0 * y1 + s.z1b;
        s.z1b = hp.b1 * y1 - hp.a1 * y2 + s.z2b;
        s.z2b = hp.b2 * y1 - hp.a2 * y2;
        return y2;
    }

    static double toLufs (double meanSquare) noexcept
    {
        return meanSquare > 1.0e-12 ? -0.691 + 10.0 * std::log10 (meanSquare) : -100.0;
    }

    void finishHop() noexcept
    {
        hops[(size_t) hopIndex] = hopEnergy / (double) hopCount;
        hopIndex = (hopIndex + 1) % (int) hops.size();
        hopsFilled = std::min (hopsFilled + 1, (int) hops.size());
        hopEnergy = 0.0;
        hopCount = 0;

        auto meanOfLast = [this] (int n)
        {
            n = std::min (n, hopsFilled);
            double sum = 0.0;
            for (int k = 1; k <= n; ++k)
                sum += hops[(size_t) ((hopIndex - k + (int) hops.size()) % (int) hops.size())];
            return n > 0 ? sum / n : 0.0;
        };

        const double m = meanOfLast (4);
        momentary.store ((float) toLufs (m), std::memory_order_relaxed);
        shortTerm.store ((float) toLufs (meanOfLast (30)), std::memory_order_relaxed);

        if (hopsFilled >= 4)
        {
            const double lufs = toLufs (m);
            if (lufs > -70.0)
            {
                const int bin = clampv ((int) ((lufs + 70.0) * 10.0), 0, kBins - 1);
                ++histCount[(size_t) bin];
                histEnergy[(size_t) bin] += m;
            }
            // Relative gate: 10 LU below the mean of everything above the absolute gate.
            double sum = 0.0;
            long count = 0;
            for (int b = 0; b < kBins; ++b)
            {
                sum += histEnergy[(size_t) b];
                count += histCount[(size_t) b];
            }
            if (count > 0)
            {
                const double gate = toLufs (sum / (double) count) - 10.0;
                const int gateBin = clampv ((int) std::ceil ((gate + 70.0) * 10.0), 0, kBins - 1);
                double gs = 0.0;
                long gc = 0;
                for (int b = gateBin; b < kBins; ++b)
                {
                    gs += histEnergy[(size_t) b];
                    gc += histCount[(size_t) b];
                }
                integrated.store (gc > 0 ? (float) toLufs (gs / (double) gc) : -100.0f, std::memory_order_relaxed);
            }
        }
    }

    static constexpr int kBins = 760; // -70 .. +6 LUFS in 0.1 LU steps
    double sr = 48000.0;
    BiquadCoefs shelf, hp;
    std::array<State, 2> state {};
    double hopEnergy = 0.0;
    int hopCount = 0, hopSamples = 4800;
    std::array<double, 30> hops {};
    int hopIndex = 0, hopsFilled = 0;
    std::array<long, kBins> histCount {};
    std::array<double, kBins> histEnergy {};
    std::atomic<float> momentary { -100.0f }, shortTerm { -100.0f }, integrated { -100.0f };
    std::atomic<bool> resetFlag { false };
};

//==============================================================================
enum class LimiterStyle { Transparent = 0, Punchy, Dynamic, Aggressive, Safe, Count };

struct LimiterParams
{
    LimiterStyle style = LimiterStyle::Transparent;
    float gainDb = 0.0f;        // 0..30
    float ceilingDb = -1.0f;    // -12..0
    float lookaheadMs = 3.0f;   // 0.1..10
    float releaseMs = 120.0f;   // 1..1000
    float stereoLink = 1.0f;    // 0..1
    bool truePeak = true;
    int ditherBits = 0;         // 0 = off, 16 or 24
};

class Limiter
{
public:
    static constexpr double kMaxLookaheadMs = 10.0;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        maxWindow = (int) std::ceil (kMaxLookaheadMs * 0.001 * sr) + 8;
        for (auto& c : main)
            c.prepare (maxWindow);
        gainSm.reset (sr, 20.0, 1.0f);
        loudness.prepare (sr);
        inMeter.prepare (sr);
        outMeter.prepare (sr);
        reset();
        updateDerived();
        gainSm.snap();
    }

    void reset() noexcept
    {
        for (auto& c : main)
            c.reset();
        loudness.reset();
        maxTruePeak = 0.0f;
    }

    void setParams (const LimiterParams& p) noexcept
    {
        params = p;
        updateDerived();
    }

    int getLatencySamples() const noexcept { return mainSet.delay(); }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        const int numCh = stereo ? 2 : 1;
        float* io[2] = { left, right };

        for (int i = 0; i < numSamples; ++i)
        {
            const float g = gainSm.next();
            float x[2] = { left[i] * g, stereo ? right[i] * g : 0.0f };
            inMeter.push (std::max (std::abs (x[0]), std::abs (x[1])));

            float y[2] = { 0.0f, 0.0f };
            const float worstGain = runStage (main, mainSet, x, y, numCh, params.stereoLink, params.truePeak);

            for (int c = 0; c < numCh; ++c)
            {
                // Guard against rounding: the sample value itself can never pass the ceiling.
                float out = clampv (y[c], -ceiling, ceiling);
                if (ditherLsb > 0.0f)
                    out += (rng.next01() - rng.next01()) * ditherLsb;
                io[c][i] = out;
                const float tp = meterTp[(size_t) c].process (out);
                maxTruePeak = std::max (maxTruePeak, std::max (tp, std::abs (out)));
                outMeter.push (std::max (tp, std::abs (out)));
            }
            minGainBlock = std::min (minGainBlock, worstGain);
        }

        loudness.process (left, right, numSamples);
        grMeter.store (gainToDb (minGainBlock), std::memory_order_relaxed);
        minGainBlock = 1.0f;
        inLevel.store (inMeter.value, std::memory_order_relaxed);
        outLevel.store (outMeter.value, std::memory_order_relaxed);
        truePeakMax.store (maxTruePeak, std::memory_order_relaxed);
        if (resetPeakFlag.exchange (false))
            maxTruePeak = 0.0f;
    }

    float getGainReductionDb() const noexcept { return grMeter.load (std::memory_order_relaxed); }
    float getInputLevel() const noexcept { return inLevel.load (std::memory_order_relaxed); }
    float getOutputLevel() const noexcept { return outLevel.load (std::memory_order_relaxed); }
    float getMaxTruePeak() const noexcept { return truePeakMax.load (std::memory_order_relaxed); }
    void resetMeters() noexcept
    {
        resetPeakFlag.store (true);
        loudness.requestReset();
    }
    LoudnessMeter loudness;

private:
    struct StageSettings
    {
        int window = 64, box1Len = 32, box2Len = 33;
        float fastRelCoef = 0.0f, slowAttCoef = 0.0f, slowRelCoef = 0.0f;
        bool adaptive = false;

        void setWindow (int w) noexcept
        {
            window = w;
            box1Len = (w + 1) / 2;
            box2Len = w - box1Len + 1;
        }
        int delay() const noexcept { return window - 1 + TruePeakDetector::kLatency; }
    };

    struct Channel
    {
        void prepare (int maxWin)
        {
            audio.resize (maxWin + TruePeakDetector::kLatency + 8);
            minValues.assign ((size_t) maxWin + 8, 1.0f);
            minIndex.assign ((size_t) maxWin + 8, 0);
            box1.assign ((size_t) maxWin + 8, 1.0f);
            box2.assign ((size_t) maxWin + 8, 1.0f);
            reset();
        }
        void reset() noexcept
        {
            audio.clear();
            inTp.reset();
            fastDb = slowDb = 0.0f;
            head = tail = 0;
            sampleIndex = 0;
            std::fill (box1.begin(), box1.end(), 1.0f);
            std::fill (box2.begin(), box2.end(), 1.0f);
            sum1 = sum2 = 0.0;
            pos1 = pos2 = 0;
            n1 = n2 = 0;
        }

        float detect (float x, bool truePeak) noexcept
        {
            audio.write (x);
            // The interpolator output describes the signal about 7.5 samples ago, so it is
            // paired with the two samples either side of that point.
            const float tp = inTp.process (x);
            const float sample = std::max (std::abs (audio.readInt (TruePeakDetector::kLatency)),
                                           std::abs (audio.readInt (TruePeakDetector::kLatency + 1)));
            return truePeak ? std::max (tp, sample) : sample;
        }

        float delayed (const StageSettings& st) const noexcept { return audio.readInt (st.delay() + 1); }

        float envelope (float required, const StageSettings& st) noexcept
        {
            // Release stage in dB: instant attack, timed release.
            const float reqDb = gainToDb (required);
            fastDb = reqDb < fastDb ? reqDb : reqDb + (fastDb - reqDb) * st.fastRelCoef;
            float eDb = fastDb;
            if (st.adaptive)
            {
                // A slower envelope builds up only under sustained limiting and holds the
                // gain steadier there, while isolated peaks recover at the fast rate.
                slowDb = fastDb < slowDb ? fastDb + (slowDb - fastDb) * st.slowAttCoef
                                         : fastDb + (slowDb - fastDb) * st.slowRelCoef;
                eDb = std::min (fastDb, slowDb);
            }
            fastDb = undenorm (fastDb);
            slowDb = undenorm (slowDb);
            const float e = dbToGain (eDb);

            // Sliding minimum over the lookahead window (+1 sample of cover either side).
            const int win = st.window + 1;
            const int cap = (int) minValues.size();
            while (head != tail && minValues[(size_t) ((tail - 1 + cap) % cap)] >= e)
                tail = (tail - 1 + cap) % cap;
            minValues[(size_t) tail] = e;
            minIndex[(size_t) tail] = sampleIndex;
            tail = (tail + 1) % cap;
            while (sampleIndex - minIndex[(size_t) head] >= win)
                head = (head + 1) % cap;
            const float m = minValues[(size_t) head];
            ++sampleIndex;

            // Two cascaded moving averages turn the stepped minimum into a smooth ramp that
            // arrives exactly when the delayed peak does.
            const float a = boxFilter (box1, pos1, n1, sum1, st.box1Len, m);
            const float b = boxFilter (box2, pos2, n2, sum2, st.box2Len, a);
            return std::min (b, 1.0f);
        }

        static float boxFilter (std::vector<float>& buf, int& pos, int& n, double& sum, int len, float x) noexcept
        {
            if (n != len)
            {
                // Window length changed: restart the average from the current value.
                std::fill (buf.begin(), buf.begin() + len, x);
                sum = (double) x * len;
                n = len;
                pos = 0;
            }
            sum += (double) x - (double) buf[(size_t) pos];
            buf[(size_t) pos] = x;
            pos = (pos + 1) % len;
            return (float) (sum / (double) len);
        }

        DelayLine audio;
        TruePeakDetector inTp;
        float fastDb = 0.0f, slowDb = 0.0f;
        std::vector<float> minValues, box1, box2;
        std::vector<long long> minIndex;
        int head = 0, tail = 0;
        long long sampleIndex = 0;
        double sum1 = 0.0, sum2 = 0.0;
        int pos1 = 0, pos2 = 0, n1 = 0, n2 = 0;
    };

    // Runs the limiter over one stereo frame. Returns the smallest gain applied.
    float runStage (std::array<Channel, 2>& chans, const StageSettings& st, const float* in, float* out,
                    int numCh, float link, bool truePeak) noexcept
    {
        float peak[2] = { 0.0f, 0.0f };
        for (int c = 0; c < numCh; ++c)
            peak[c] = chans[(size_t) c].detect (in[c], truePeak);

        const float linked = std::max (peak[0], peak[1]);
        float worst = 1.0f;
        for (int c = 0; c < numCh; ++c)
        {
            const float pk = peak[c] + (linked - peak[c]) * link;
            const float required = pk > ceiling ? ceiling / pk : 1.0f;
            const float gain = chans[(size_t) c].envelope (required, st);
            out[c] = chans[(size_t) c].delayed (st) * gain;
            worst = std::min (worst, gain);
        }
        return worst;
    }

    void updateDerived() noexcept
    {
        float attackFraction = 1.0f, releaseScale = 1.0f, margin = 0.0f;
        mainSet.adaptive = false;
        switch (params.style)
        {
            case LimiterStyle::Transparent: mainSet.adaptive = true; break;
            case LimiterStyle::Punchy:      attackFraction = 0.4f; releaseScale = 0.6f; break;
            case LimiterStyle::Dynamic:     attackFraction = 0.7f; releaseScale = 0.5f; mainSet.adaptive = true; break;
            case LimiterStyle::Aggressive:  attackFraction = 0.25f; releaseScale = 0.3f; break;
            case LimiterStyle::Safe:        releaseScale = 2.0f; mainSet.adaptive = true; margin = 0.1f; break;
            case LimiterStyle::Count: break;
        }

        const double lookMs = clampv ((double) params.lookaheadMs, 0.1, kMaxLookaheadMs) * attackFraction;
        mainSet.setWindow (clampv ((int) std::lround (lookMs * 0.001 * sr), 2, maxWindow - 8));

        const double rel = clampv ((double) params.releaseMs, 1.0, 1000.0) * releaseScale;
        mainSet.fastRelCoef = onePoleCoef (rel, sr);
        mainSet.slowAttCoef = onePoleCoef (80.0, sr);
        mainSet.slowRelCoef = onePoleCoef (rel * 6.0 + 150.0, sr);

        // A small safety margin absorbs the residual error of estimating inter-sample peaks.
        const float tpMargin = params.truePeak ? 0.05f : 0.0f;
        ceiling = dbToGain (params.ceilingDb - margin - tpMargin);
        gainSm.setTarget (dbToGain (params.gainDb));
        ditherLsb = params.ditherBits == 16 ? 1.0f / 32768.0f : (params.ditherBits == 24 ? 1.0f / 8388608.0f : 0.0f);
    }

    double sr = 44100.0;
    LimiterParams params;
    std::array<Channel, 2> main;
    StageSettings mainSet;
    std::array<TruePeakDetector, 2> meterTp;
    int maxWindow = 512;
    float ceiling = 1.0f, ditherLsb = 0.0f;
    Smoothed gainSm;
    Random rng { 0x1234567u };
    LevelFollower inMeter, outMeter;
    float minGainBlock = 1.0f, maxTruePeak = 0.0f;
    std::atomic<float> grMeter { 0.0f }, inLevel { 0.0f }, outLevel { 0.0f }, truePeakMax { 0.0f };
    std::atomic<bool> resetPeakFlag { false };
};
} // namespace gitto
