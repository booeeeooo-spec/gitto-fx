// Gitto FX Tune - real-time automatic pitch correction.
//
// A pitch detector (normalised square difference, after McLeod and Wyvill) follows
// the incoming note. The audio is then rebuilt from short overlapping grains, one per
// pitch period, placed closer together or further apart to raise or lower the pitch
// (pitch-synchronous overlap-add). Because the grains themselves are not stretched,
// the vocal character stays put unless the Formant control moves it on purpose.
#pragma once

#include "Common.h"
#include <atomic>

namespace gitto
{
enum class TuneScale { Chromatic = 0, Major, Minor, HarmonicMinor, MajorPentatonic, MinorPentatonic, Blues, Dorian, Mixolydian, Count };
enum class TuneRange { Low = 0, Mid, High, Count };

struct TuneParams
{
    int key = 0;                         // 0 = C .. 11 = B
    TuneScale scale = TuneScale::Chromatic;
    TuneRange range = TuneRange::Mid;
    float retuneMs = 20.0f;              // 0..400: 0 snaps hard, higher is more natural
    float humanize = 0.0f;               // 0..1: eases off on held notes
    float amount = 1.0f;                 // 0..1 of the correction applied
    float transpose = 0.0f;              // -12..+12 semitones
    float formant = 0.0f;                // -6..+6 semitones
    float tuningHz = 440.0f;             // 415..466 reference for A4
    float mix = 1.0f;
    float outputDb = 0.0f;
};

class Tune
{
public:
    static constexpr int kAnalysisWindow = 512; // samples at the decimated rate
    static constexpr int kRingBits = 16;
    static constexpr int kRing = 1 << kRingBits;
    static constexpr int kMask = kRing - 1;

    static int scaleMask (TuneScale s) noexcept
    {
        // Bit n set = n semitones above the key note is allowed.
        static const int masks[(int) TuneScale::Count] = {
            0xFFF,                                                   // chromatic
            (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),  // major
            (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),  // natural minor
            (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),  // harmonic minor
            (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),                         // major pentatonic
            (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),                        // minor pentatonic
            (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),             // blues
            (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),  // dorian
            (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),  // mixolydian
        };
        return masks[clampv ((int) s, 0, (int) TuneScale::Count - 1)];
    }

    static void rangeHz (TuneRange r, float& lo, float& hi) noexcept
    {
        switch (r)
        {
            case TuneRange::Low:  lo = 65.0f; hi = 500.0f; break;
            case TuneRange::High: lo = 160.0f; hi = 1200.0f; break;
            default:              lo = 90.0f; hi = 800.0f; break;
        }
    }

    // Nearest allowed note (as a MIDI note number) to a fractional MIDI pitch.
    static int nearestNote (float midi, int key, TuneScale scale) noexcept
    {
        const int mask = scaleMask (scale);
        const int centre = (int) std::lround (midi);
        int best = centre;
        float bestDist = 1.0e9f;
        for (int n = centre - 6; n <= centre + 6; ++n)
        {
            const int degree = ((n - key) % 12 + 12) % 12;
            if ((mask & (1 << degree)) == 0)
                continue;
            const float d = std::abs ((float) n - midi);
            if (d < bestDist)
            {
                bestDist = d;
                best = n;
            }
        }
        return best;
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        decimation = std::max (1, (int) std::lround (sr / 11025.0));
        decRate = sr / decimation;
        const auto lp1 = design::lowpass (decRate * 0.4, 0.5412, sr);
        const auto lp2 = design::lowpass (decRate * 0.4, 1.3066, sr);
        decLp[0].setCoefs (lp1);
        decLp[1].setCoefs (lp2);
        analysisHop = std::max (64, (int) (sr * 0.0058));
        for (auto& r : inRing) r.assign ((size_t) kRing, 0.0f);
        for (auto& r : accRing) r.assign ((size_t) kRing, 0.0f);
        hann.resize (1025);
        for (int i = 0; i <= 1024; ++i)
            hann[(size_t) i] = (float) (0.5 + 0.5 * std::cos (kPi * i / 1024.0)); // half window, 1 at centre
        mixSm.reset (sr, 30.0, 1.0f);
        outSm.reset (sr, 30.0, 1.0f);
        periodCoef = onePoleCoef (4.0, sr);
        unvoicedCoef = onePoleCoef (25.0, sr);
        reset();
        update();
        mixSm.snap();
        outSm.snap();
    }

    void reset() noexcept
    {
        for (auto& r : inRing) std::fill (r.begin(), r.end(), 0.0f);
        for (auto& r : accRing) std::fill (r.begin(), r.end(), 0.0f);
        decBuf.fill (0.0f);
        decLp[0].reset();
        decLp[1].reset();
        decPos = decCount = hopCount = 0;
        now = 0;
        period = (float) (sr / 200.0);
        periodTarget = period;
        synthMark = analysisMark = 0.0;
        correction = 0.0f;
        voiced = false;
        voicedRun = unvoicedRun = 0;
        heldNote = -1;
        heldSamples = 0;
        detectedMidi.store (-1.0f);
        targetMidi.store (-1.0f);
        correctionCents.store (0.0f);
    }

    void setParams (const TuneParams& p) noexcept
    {
        params = p;
        update();
    }

    int getLatencySamples() const noexcept { return latency; }
    // For the display: detected pitch and target note as MIDI numbers (-1 when no pitch),
    // and the correction currently applied, in cents.
    float getDetectedMidi() const noexcept { return detectedMidi.load (std::memory_order_relaxed); }
    float getTargetMidi() const noexcept { return targetMidi.load (std::memory_order_relaxed); }
    float getCorrectionCents() const noexcept { return correctionCents.load (std::memory_order_relaxed); }

    void process (float* left, float* right, int numSamples) noexcept
    {
        const bool stereo = right != nullptr;
        const int numCh = stereo ? 2 : 1;
        float* io[2] = { left, right };

        for (int i = 0; i < numSamples; ++i)
        {
            const int w = (int) (now & kMask);
            float mono = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                inRing[(size_t) c][(size_t) w] = io[c][i];
                mono += io[c][i];
            }
            mono /= (float) numCh;

            // --- pitch detector feed ---
            const float filtered = decLp[1].process (decLp[0].process (mono));
            if (++decCount >= decimation)
            {
                decCount = 0;
                decBuf[(size_t) decPos] = filtered;
                decPos = (decPos + 1) % kAnalysisWindow;
            }
            if (++hopCount >= analysisHop)
            {
                hopCount = 0;
                analyse();
            }

            // --- pitch and correction, smoothed per sample ---
            period += (periodTarget - period) * (1.0f - periodCoef);
            float desired = 0.0f;
            if (voiced && currentMidi > 0.0f)
                desired = ((float) currentTarget - currentMidi) * params.amount;
            float coef = voiced ? retuneCoef : unvoicedCoef;
            if (voiced && params.humanize > 0.0f && heldSamples > holdThreshold)
                coef = humanCoef; // held note: back off so natural drift and vibrato survive
            correction = desired + (correction - desired) * coef;
            if (voiced)
                ++heldSamples;
            const float semis = correction + (voiced ? params.transpose : 0.0f);
            const float ratio = clampv (std::pow (2.0f, semis / 12.0f), 0.4f, 2.5f);

            // --- lay down grains that are about to be needed ---
            const double readTime = (double) (now - latency);
            int guard = 0;
            while (synthMark - (double) period <= readTime + 1.0 && guard++ < 8)
            {
                addGrain (numCh, ratio);
                synthMark += (double) period / (double) ratio;
                while (analysisMark + 0.5 * (double) period < synthMark)
                    analysisMark += (double) period;
                if (! voiced)
                    analysisMark += (synthMark - analysisMark) * 0.02; // drift back into step
            }

            // --- output ---
            const int r = (int) ((now - latency) & kMask);
            const float mx = mixSm.next(), og = outSm.next();
            for (int c = 0; c < numCh; ++c)
            {
                const float wet = accRing[(size_t) c][(size_t) r];
                accRing[(size_t) c][(size_t) r] = 0.0f;
                const float dry = inRing[(size_t) c][(size_t) r];
                io[c][i] = (dry + (wet - dry) * mx) * og;
            }
            ++now;
        }
        decLp[0].sanitise();
        decLp[1].sanitise();
        correctionCents.store (voiced ? correction * 100.0f : 0.0f, std::memory_order_relaxed);
    }

private:
    void addGrain (int numCh, float ratio) noexcept
    {
        const double p = (double) period;
        const double centre = synthMark;
        const long long first = (long long) std::ceil (centre - p), last = (long long) std::floor (centre + p);
        const long long readIndex = now - latency;
        const float gain = 1.0f / ratio; // overlapping Hann grains sum to `ratio`
        const double invP = 1.0 / p;

        for (long long n = std::max (first, readIndex); n <= last; ++n)
        {
            const double offset = (double) n - centre;
            const double u = std::abs (offset * invP) * 1024.0;
            const int ui = (int) u;
            if (ui >= 1024)
                continue;
            const float win = hann[(size_t) ui] + (hann[(size_t) ui + 1] - hann[(size_t) ui]) * (float) (u - ui);

            const double src = analysisMark + offset * (double) formantRatio;
            const long long s0 = (long long) std::floor (src);
            if (s0 + 1 > now || now - s0 >= kRing - 2)
                continue; // not recorded yet, or already overwritten
            const float frac = (float) (src - (double) s0);
            for (int c = 0; c < numCh; ++c)
            {
                const float a = inRing[(size_t) c][(size_t) (s0 & kMask)];
                const float b = inRing[(size_t) c][(size_t) ((s0 + 1) & kMask)];
                accRing[(size_t) c][(size_t) (n & kMask)] += (a + (b - a) * frac) * win * gain;
            }
        }
    }

    void analyse() noexcept
    {
        // Unroll the ring, oldest first, and remove any offset.
        float x[kAnalysisWindow];
        double mean = 0.0;
        for (int i = 0; i < kAnalysisWindow; ++i)
        {
            x[i] = decBuf[(size_t) ((decPos + i) % kAnalysisWindow)];
            mean += x[i];
        }
        mean /= kAnalysisWindow;
        double energy = 0.0;
        for (float& v : x)
        {
            v -= (float) mean;
            energy += (double) v * v;
        }

        bool found = false;
        float freq = 0.0f;
        if (energy / kAnalysisWindow > 1.0e-6) // above roughly -60 dBFS
        {
            const int maxLag = std::min (kAnalysisWindow / 2, (int) (decRate / minHz) + 2);
            const int minLag = std::max (2, (int) (decRate / maxHz));
            float nsdf[kAnalysisWindow / 2 + 2];
            double m = 2.0 * energy;
            nsdf[0] = 1.0f;
            for (int tau = 1; tau <= maxLag; ++tau)
            {
                m -= (double) x[tau - 1] * x[tau - 1] + (double) x[kAnalysisWindow - tau] * x[kAnalysisWindow - tau];
                double r = 0.0;
                for (int j = 0; j < kAnalysisWindow - tau; ++j)
                    r += (double) x[j] * x[j + tau];
                nsdf[tau] = m > 1.0e-12 ? (float) (2.0 * r / m) : 0.0f;
            }

            // Highest peak after the first dip below zero, then the first peak close to it.
            int start = 1;
            while (start < maxLag && nsdf[start] > 0.0f)
                ++start;
            float best = 0.0f;
            for (int tau = std::max (start, minLag); tau < maxLag; ++tau)
                if (nsdf[tau] > nsdf[tau - 1] && nsdf[tau] >= nsdf[tau + 1])
                    best = std::max (best, nsdf[tau]);
            if (best > 0.55f)
            {
                for (int tau = std::max (start, minLag); tau < maxLag; ++tau)
                {
                    if (nsdf[tau] > nsdf[tau - 1] && nsdf[tau] >= nsdf[tau + 1] && nsdf[tau] >= 0.9f * best)
                    {
                        // Parabolic refinement between samples.
                        const float a = nsdf[tau - 1], b = nsdf[tau], c = nsdf[tau + 1];
                        const float denom = a - 2.0f * b + c;
                        const float shift = std::abs (denom) > 1.0e-9f ? 0.5f * (a - c) / denom : 0.0f;
                        freq = (float) (decRate / ((double) tau + (double) shift));
                        found = freq >= minHz * 0.97f && freq <= maxHz * 1.03f;
                        break;
                    }
                }
            }
        }

        // Need two agreeing frames to start, and three misses to stop: avoids chatter.
        if (found)
        {
            unvoicedRun = 0;
            if (++voicedRun >= 2 || voiced)
            {
                voiced = true;
                currentMidi = 69.0f + 12.0f * std::log2 (freq / params.tuningHz);
                periodTarget = clampv ((float) (sr / freq), (float) (sr / maxHz), (float) (sr / minHz));

                // Stick with the current note until another is clearly closer.
                int note = nearestNote (currentMidi, params.key, params.scale);
                if (heldNote >= 0 && note != heldNote
                    && std::abs ((float) heldNote - currentMidi) < std::abs ((float) note - currentMidi) + 0.12f
                    && nearestNote ((float) heldNote, params.key, params.scale) == heldNote)
                    note = heldNote;
                if (note != heldNote)
                {
                    heldNote = note;
                    heldSamples = 0;
                }
                currentTarget = note;
            }
        }
        else
        {
            voicedRun = 0;
            if (++unvoicedRun >= 3)
            {
                voiced = false;
                heldNote = -1;
                heldSamples = 0;
            }
        }

        detectedMidi.store (voiced ? currentMidi : -1.0f, std::memory_order_relaxed);
        targetMidi.store (voiced ? (float) currentTarget + params.transpose : -1.0f, std::memory_order_relaxed);
    }

    void update() noexcept
    {
        rangeHz (params.range, minHz, maxHz);
        // Room for one grain: half a period of slack, one period either side, plus the
        // extra reach a raised formant needs.
        latency = std::min (kRing / 4, (int) std::ceil (3.0 * sr / minHz));
        formantRatio = std::pow (2.0f, clampv (params.formant, -6.0f, 6.0f) / 12.0f);
        const double retune = clampv ((double) params.retuneMs, 0.0, 1000.0);
        retuneCoef = onePoleCoef (retune, sr);
        humanCoef = onePoleCoef (retune + 350.0 * clampv ((double) params.humanize, 0.0, 1.0), sr);
        holdThreshold = (int) (0.18 * sr);
        mixSm.setTarget (clampv (params.mix, 0.0f, 1.0f));
        outSm.setTarget (dbToGain (params.outputDb));
    }

    double sr = 44100.0, decRate = 11025.0;
    TuneParams params;
    int decimation = 4, analysisHop = 256, latency = 1470;
    std::array<Biquad, 2> decLp;
    std::array<float, kAnalysisWindow> decBuf {};
    int decPos = 0, decCount = 0, hopCount = 0;
    std::array<std::vector<float>, 2> inRing, accRing;
    std::vector<float> hann;
    long long now = 0;
    double synthMark = 0.0, analysisMark = 0.0;
    float period = 220.0f, periodTarget = 220.0f, periodCoef = 0.0f;
    float minHz = 90.0f, maxHz = 800.0f, formantRatio = 1.0f;
    float correction = 0.0f, retuneCoef = 0.0f, humanCoef = 0.0f, unvoicedCoef = 0.0f;
    float currentMidi = -1.0f;
    int currentTarget = 60, heldNote = -1, heldSamples = 0, holdThreshold = 8000;
    bool voiced = false;
    int voicedRun = 0, unvoicedRun = 0;
    Smoothed mixSm, outSm;
    std::atomic<float> detectedMidi { -1.0f }, targetMidi { -1.0f }, correctionCents { 0.0f };
};
} // namespace gitto
