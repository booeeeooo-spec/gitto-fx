// Offline checks for the second set of Gitto FX plugins.
// Build: g++ -std=c++17 -O2 dsp_tests2.cpp -o dsp_tests2
#include "../dsp/ChannelStrip.h"
#include "../dsp/ClassicComps.h"
#include "../dsp/DeEsser.h"
#include "../dsp/Distortion.h"
#include "../dsp/Multiband.h"
#include "../dsp/Plate.h"
#include "../dsp/Saturator.h"
#include "../dsp/Shaper.h"
#include "../dsp/Smooth.h"
#include "../dsp/Space.h"
#include "../dsp/Tape.h"
#include "../dsp/Tune.h"

#include "test_util.h"

// Total harmonic distortion (%) of a tone, harmonics 2..9.
static double thdPercent (const Buf& b, double hz, size_t from, size_t to)
{
    const double fund = toneAmp (b, hz, from, to);
    double harm = 0;
    for (int h = 2; h <= 9; ++h)
        if (hz * h < SR * 0.48)
        {
            const double a = toneAmp (b, hz * h, from, to);
            harm += a * a;
        }
    return 100.0 * std::sqrt (harm) / std::max (fund, 1e-12);
}

// Energy that is not at a multiple of `hz`, relative to the total, in dB. For a
// distorted sine this is the aliasing (plus noise), since real harmonics are multiples.
static double inharmonicDb (const Buf& b, double hz, size_t from)
{
    const size_t n = 32768;
    std::vector<std::complex<float>> spec (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double w = 0.35875 - 0.48829 * std::cos (kTwoPi * i / (n - 1)) + 0.14128 * std::cos (2 * kTwoPi * i / (n - 1)) - 0.01168 * std::cos (3 * kTwoPi * i / (n - 1));
        spec[i] = (float) (b[from + i] * w);
    }
    fft (spec);
    double total = 0, other = 0;
    const double binHz = SR / (double) n;
    for (size_t k = 8; k < n / 2; ++k)
    {
        const double f = k * binHz;
        const double p = std::norm (spec[k]);
        total += p;
        const double nearest = std::round (f / hz) * hz;
        if (std::abs (f - nearest) > 6.0 * binHz)
            other += p;
    }
    return 10.0 * std::log10 (other / total + 1e-30);
}

// Fundamental frequency of a steady pitched signal, by normalised autocorrelation.
static double measureHz (const Buf& b, size_t from, size_t len, double lo, double hi)
{
    const int minLag = (int) (SR / hi), maxLag = (int) (SR / lo);
    std::vector<double> ns ((size_t) maxLag + 2, 0.0);
    for (int tau = minLag - 1; tau <= maxLag + 1; ++tau)
    {
        double r = 0, m = 0;
        for (size_t j = from; j + (size_t) tau < from + len; ++j)
        {
            r += (double) b[j] * b[j + (size_t) tau];
            m += (double) b[j] * b[j] + (double) b[j + (size_t) tau] * b[j + (size_t) tau];
        }
        ns[(size_t) tau] = m > 0 ? 2 * r / m : 0;
    }
    double best = 0;
    for (int tau = minLag; tau <= maxLag; ++tau)
        if (ns[(size_t) tau] > ns[(size_t) tau - 1] && ns[(size_t) tau] >= ns[(size_t) tau + 1])
            best = std::max (best, ns[(size_t) tau]);
    for (int tau = minLag; tau <= maxLag; ++tau)
        if (ns[(size_t) tau] > ns[(size_t) tau - 1] && ns[(size_t) tau] >= ns[(size_t) tau + 1] && ns[(size_t) tau] >= 0.95 * best)
        {
            const double a = ns[(size_t) tau - 1], c = ns[(size_t) tau + 1], m = ns[(size_t) tau];
            const double shift = 0.5 * (a - c) / (a - 2 * m + c);
            return SR / (tau + shift);
        }
    return 0;
}

// Band-limited sawtooth-like tone: the first 12 harmonics, falling off like a voice.
static Buf voiceTone (double hz, double amp, double seconds)
{
    Buf b ((size_t) (seconds * SR));
    for (size_t i = 0; i < b.size(); ++i)
    {
        double v = 0;
        for (int h = 1; h <= 12 && hz * h < 9000; ++h)
            v += std::sin (kTwoPi * hz * h * (double) i / SR + 0.3 * h) / std::pow ((double) h, 1.2);
        b[i] = (float) (amp * v * 0.6);
    }
    return b;
}

static double cents (double f, double ref) { return 1200.0 * std::log2 (f / ref); }

static double maxDiffDelayed (const Buf& out, const Buf& in, int delay, size_t from = 0)
{
    double d = 0;
    for (size_t i = std::max (from, (size_t) delay); i < out.size(); ++i)
        d = std::max (d, (double) std::abs (out[i] - in[i - (size_t) delay]));
    return d;
}

//==============================================================================
static void testClassicComps()
{
    std::printf ("\nOpto Comp\n");
    {
        OptoComp c;
        OptoParams p;
        p.reduction = 0.5f;
        p.ratio = 3;
        c.setParams (p);
        c.prepare (SR);
        Buf l = sine (1000, 0.5, 4), r = l;
        for (size_t i = (size_t) (2 * SR); i < l.size(); ++i)
            l[i] = r[i] = (float) (1e-4 * std::sin (kTwoPi * 1000 * (double) i / SR)); // near-silence to watch the release
        double steady = 0, t50 = -1, t90 = -1;
        size_t pos = 0;
        processBlocks (l, r, [&] (float* a, float* b, int n)
        {
            c.process (a, b, n);
            pos += (size_t) n;
            const double g = c.getGainReductionDb();
            if (pos <= (size_t) (2 * SR))
                steady = g;
            else
            {
                const double t = (double) (pos - (size_t) (2 * SR)) / SR * 1000.0;
                if (t50 < 0 && g > 0.5 * steady) t50 = t;
                if (t90 < 0 && g > 0.1 * steady) t90 = t;
            }
        }, 32);
        check (steady < -7 && steady > -13 && allFinite (l), "steady reduction on a -6 dBFS tone", fmt ("(%.1f dB)", steady));
        check (t50 > 20 && t50 < 200 && t90 > 350, "two-stage release", fmt ("(half back in %.0f ms, 90%% back in %.0f ms)", t50, t90));
    }
    {
        OptoComp c;
        OptoParams p;
        p.reduction = 1;
        p.ratio = 10;
        p.gainDb = 36;
        p.manual = true;
        p.attackMs = 0.5f;
        p.releaseMs = 50;
        c.setParams (p);
        c.prepare (SR);
        Buf l = drums (0.9, 5), r = drums (0.8, 5);
        processBlocks (l, r, [&] (float* a, float* b, int n) { c.process (a, b, n); });
        check (allFinite (l) && allFinite (r) && peakOf (l) < 40.0, "extreme settings stay finite", fmt ("(peak %.1f dB)", db (peakOf (l))));
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { c.process (a, b, n); }));
    }

    std::printf ("\nFET Comp\n");
    const char* ratios[] = { "4:1", "8:1", "12:1", "20:1", "All" };
    for (int rt = 0; rt < 5; ++rt)
    {
        FetComp c;
        FetParams p;
        p.ratio = rt;
        p.inputDb = 18;
        p.attack = 0.0f;
        p.releaseMs = 50;
        c.setParams (p);
        c.prepare (SR);
        Buf l = sine (220, 0.5, 3), r = l;
        double lo = 0, hi = -100;
        size_t pos = 0;
        processBlocks (l, r, [&] (float* a, float* b, int n)
        {
            c.process (a, b, n);
            pos += (size_t) n;
            if (pos > (size_t) (2 * SR))
            {
                lo = std::min (lo, (double) c.getGainReductionDb());
                hi = std::max (hi, (double) c.getGainReductionDb());
            }
        }, 16);
        check (allFinite (l) && lo < -8 && lo > -45 && hi - lo < 3.0, std::string ("steady and stable, ratio ") + ratios[rt],
               fmt ("(reduction %.1f to %.1f dB)", lo, hi));
    }
    {
        FetComp c;
        FetParams p;
        p.inputDb = 20;
        p.attack = 0.0f;
        c.setParams (p);
        c.prepare (SR);
        Buf l = sine (3000, 0.5, 0.2), r = l;
        double t63 = -1, finalGr = 0;
        {
            FetComp ref;
            ref.setParams (p);
            ref.prepare (SR);
            Buf a = sine (3000, 0.5, 1), b = a;
            processBlocks (a, b, [&] (float* x, float* y, int n) { ref.process (x, y, n); });
            finalGr = ref.getGainReductionDb();
        }
        for (size_t pos = 0; pos < l.size() && t63 < 0; pos += 2)
        {
            c.process (l.data() + pos, r.data() + pos, 2);
            if (c.getGainReductionDb() < 0.63 * finalGr)
                t63 = (double) pos / SR * 1000.0;
        }
        check (t63 >= 0 && t63 < 2.0, "fastest attack", fmt ("(63%% of %.1f dB reached in %.2f ms)", finalGr, t63));
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { c.process (a, b, n); }));
    }
    {
        // The attack time has to hold at every ratio, including All where the loop gain is highest.
        bool ok = true;
        std::string detail;
        for (int rt = 0; rt < 5; ++rt)
            for (float knob : { 0.0f, 1.0f })
            {
                FetComp c;
                FetParams p;
                p.ratio = rt;
                p.inputDb = 12;
                p.attack = knob;
                c.setParams (p);
                c.prepare (SR);
                Buf settle = sine (3000, 0.5, 1), s2 = settle;
                FetComp ref;
                ref.setParams (p);
                ref.prepare (SR);
                processBlocks (settle, s2, [&] (float* x, float* y, int n) { ref.process (x, y, n); });
                const double finalGr = ref.getGainReductionDb();
                Buf l = sine (3000, 0.5, 0.2), r = l;
                double t63 = -1;
                for (size_t pos = 0; pos < l.size() && t63 < 0; ++pos)
                {
                    c.process (l.data() + pos, r.data() + pos, 1);
                    if (c.getGainReductionDb() < 0.63 * finalGr)
                        t63 = (double) pos / SR * 1000.0;
                }
                const double want = knob == 0.0f ? 0.02 : 0.8;
                // One sample is 0.02 ms here, and the knee slows the first part a little.
                ok = ok && t63 >= 0 && t63 < want * 2.5 + 0.05 && (knob == 0.0f || t63 > want * 0.3);
                detail += std::string (" ") + ratios[rt] + fmt (" %.2f", t63);
            }
        check (ok, "attack 0.02 and 0.8 ms at every ratio", "(ms to 63%:" + detail + ")");
    }
    {
        // Driven far too hard with the slowest attack, transients get past the gain reduction.
        // They must come out squashed, never larger than what went in or flipped over.
        bool bounded = true;
        double worst = 0;
        for (int rt = 0; rt < 5; ++rt)
        {
            FetComp c;
            FetParams p;
            p.ratio = rt;
            p.inputDb = 36;
            p.attack = 1.0f;
            c.setParams (p);
            c.prepare (SR);
            Buf l = drums (0.5, 2.0), r = l;
            const double inPeak = peakOf (l) * dbToGain (36.0f);
            processBlocks (l, r, [&] (float* a, float* b, int n) { c.process (a, b, n); });
            worst = std::max (worst, peakOf (l) / inPeak);
            bounded = bounded && allFinite (l) && peakOf (l) <= inPeak;
        }
        check (bounded, "overdriven transients stay bounded", fmt ("(largest output peak is %.2f of the driven input peak)", worst));
    }

    std::printf ("\nBus Comp\n");
    {
        BusComp c;
        BusParams p;
        p.thresholdDb = -20;
        p.ratio = 2; // 4:1
        p.attack = 2;
        p.release = 1;
        c.setParams (p);
        c.prepare (SR);
        Buf l = sine (1000, 0.5, 3), r = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { c.process (a, b, n); });
        const double g = c.getGainReductionDb();
        check (g < -6.5 && g > -10.5, "steady reduction, 4:1 at -20 dB on a -6 dBFS tone", fmt ("(%.1f dB; feedback design predicts about -8.3)", g));
    }
    {
        bool ok = true;
        double worst = 0;
        for (int ra = 0; ra < BusComp::kNumRatios; ++ra)
            for (int at = 0; at < BusComp::kNumAttacks; ++at)
                for (int re = 0; re < BusComp::kNumReleases; ++re)
                {
                    BusComp c;
                    BusParams p;
                    p.thresholdDb = -40;
                    p.ratio = ra;
                    p.attack = at;
                    p.release = re;
                    p.makeupDb = 12;
                    c.setParams (p);
                    c.prepare (SR);
                    Buf l = drums (0.9, 1.5), r = drums (0.8, 1.5);
                    processBlocks (l, r, [&] (float* a, float* b, int n) { c.process (a, b, n); });
                    ok = ok && allFinite (l) && allFinite (r) && c.getGainReductionDb() <= 0.01f;
                    worst = std::max (worst, peakOf (l));
                }
        check (ok && worst < 8.0, "every ratio, attack and release combination", fmt ("(120 combinations, peak %.1f dB)", db (worst)));
        BusComp c;
        c.prepare (SR);
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { c.process (a, b, n); }));
    }
}

//==============================================================================
static void testMultiband()
{
    std::printf ("\nMultiband\n");
    auto neutral = []
    {
        MultibandParams p;
        for (auto& b : p.bands)
            b.rangeDb = 0;
        return p;
    };

    // 1. With nothing engaged, the four bands add back up to a flat response.
    {
        double worst = 0;
        for (double f : { 40., 120., 300., 800., 2000., 5000., 12000. })
        {
            Multiband m;
            m.setParams (neutral());
            m.prepare (SR);
            Buf l = sine (f, 0.25, 1.5), r = l;
            processBlocks (l, r, [&] (float* a, float* b, int n) { m.process (a, b, n); });
            worst = std::max (worst, std::abs (db (toneAmp (l, f, (size_t) SR, l.size()) / 0.25)));
        }
        check (worst < 0.05, "bands sum flat when idle", fmt ("(worst deviation %.3f dB across 40 Hz to 12 kHz)", worst));
    }

    // 2. Compressing the low band leaves the highs alone, and the other way round.
    {
        auto run = [] (double f, int band)
        {
            Multiband m;
            MultibandParams p;
            for (auto& b : p.bands)
                b.rangeDb = 0;
            p.bands[(size_t) band].rangeDb = -12;
            p.bands[(size_t) band].thresholdDb = -40;
            p.bands[(size_t) band].ratio = 20;
            m.setParams (p);
            m.prepare (SR);
            Buf l = sine (f, 0.5, 2), r = l;
            processBlocks (l, r, [&] (float* a, float* b, int n) { m.process (a, b, n); });
            return db (toneAmp (l, f, (size_t) SR, l.size()) / 0.5);
        };
        const double lowHit = run (50, 0), highSpared = run (14000, 0), highHit = run (14000, 3), lowSpared = run (50, 3);
        check (lowHit < -10 && highSpared > -0.3 && highHit < -10 && lowSpared > -0.3, "each band only touches its own range",
               fmt ("(band 1: 50 Hz %.1f dB, 14 kHz %.2f dB", lowHit, highSpared) + fmt ("; band 4: 14 kHz %.1f dB, 50 Hz %.2f dB)", highHit, lowSpared));
    }

    // 3. Positive range expands upward.
    {
        Multiband m;
        MultibandParams p = neutral();
        p.bands[1].rangeDb = 6;
        p.bands[1].thresholdDb = -40;
        p.bands[1].ratio = 20;
        m.setParams (p);
        m.prepare (SR);
        Buf l = sine (400, 0.25, 2), r = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { m.process (a, b, n); });
        const double g = db (toneAmp (l, 400, (size_t) SR, l.size()) / 0.25);
        check (g > 5.0 && g < 6.3, "upward expansion", fmt ("(+%.1f dB, range set to +6)", g));
    }

    // 4. Lookahead latency is what is reported; Mix at 0 is the same as idle.
    {
        Multiband m;
        MultibandParams p = neutral();
        p.lookaheadMs = 5;
        m.setParams (p);
        m.prepare (SR);
        Buf l (4800, 0.0f), r = l;
        l[10] = r[10] = 1.0f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { m.process (a, b, n); });
        double e0 = 0, e1 = 0;
        for (size_t i = 0; i < l.size(); ++i)
            (i < 10 + (size_t) m.getLatencySamples() ? e0 : e1) += (double) l[i] * l[i];
        check (m.getLatencySamples() == 240 && e0 < 1e-12 && e1 > 0.5, "lookahead latency", fmt ("(%g samples; nothing arrives early)", m.getLatencySamples()));
    }

    // 5. Hard use on every band with mid/side placements.
    {
        Multiband m;
        MultibandParams p;
        for (int b = 0; b < 4; ++b)
        {
            p.bands[(size_t) b].rangeDb = b % 2 ? 30.0f : -30.0f;
            p.bands[(size_t) b].thresholdDb = -50;
            p.bands[(size_t) b].ratio = 20;
            p.bands[(size_t) b].attackMs = 0.1f;
            p.bands[(size_t) b].releaseMs = 5;
            p.bands[(size_t) b].placement = (BandPlacement) (b % 3);
        }
        p.lookaheadMs = 20;
        m.setParams (p);
        m.prepare (SR);
        Buf l = drums (0.9, 5), r = noise (0.5, 5, 7);
        processBlocks (l, r, [&] (float* a, float* b, int n) { m.process (a, b, n); });
        check (allFinite (l) && allFinite (r) && peakOf (l) < 60.0, "extreme settings stay finite", fmt ("(peak %.1f dB)", db (peakOf (l))));
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { m.process (a, b, n); }));
    }
}

//==============================================================================
static void testSaturator()
{
    std::printf ("\nSaturator\n");
    const char* names[] = { "Warm", "Tape", "Console", "Crunch", "Fuzz" };

    for (int s = 0; s < (int) SatStyle::Count; ++s)
    {
        Saturator sat;
        SaturatorParams p;
        p.style = (SatStyle) s;
        p.drive = 0.6f;
        sat.setParams (p);
        sat.prepare (SR);
        Buf l = sine (1000, 0.25, 2), r = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { sat.process (a, b, n); });
        const double thd = thdPercent (l, 1000, (size_t) SR, l.size());

        // Aliasing check with a 5 kHz tone driven hard, against the same curve with no oversampling.
        Saturator hard;
        p.drive = 1.0f;
        hard.setParams (p);
        hard.prepare (SR);
        Buf a = sine (5000, 0.5, 2), b = a, naive = a;
        processBlocks (a, b, [&] (float* x, float* y, int n) { hard.process (x, y, n); });
        const float g = dbToGain (36.0f);
        for (auto& v : naive)
            v = shape (Saturator::curveFor ((SatStyle) s), v * g);
        const double alias = inharmonicDb (a, 5000, (size_t) SR), aliasNaive = inharmonicDb (naive, 5000, (size_t) SR);
        check (thd > 1.0 && allFinite (l) && alias < aliasNaive - 15.0, names[s],
               fmt ("(THD %.1f%% at 60%% drive; aliasing %.0f dB, against %.0f dB without oversampling)", thd, alias, aliasNaive));
    }

    // Auto gain keeps loudness roughly constant across the drive range, for material at
    // the level it is calibrated for (-18 dBFS RMS). Quieter or louder material drifts.
    {
        double worst = 0;
        for (int s = 0; s < (int) SatStyle::Count; ++s)
            for (float drive : { 0.0f, 0.3f, 0.6f, 1.0f })
            {
                Saturator sat;
                SaturatorParams p;
                p.style = (SatStyle) s;
                p.drive = drive;
                sat.setParams (p);
                sat.prepare (SR);
                Buf l = noise (0.2, 2, 5), r = l;
                // Limit the test noise to the audio mid-band so it resembles programme material.
                Biquad f;
                f.setCoefs (design::lowpass (4000, 0.7, SR));
                for (auto& v : l) v = f.process (v);
                const double scaleTo = 0.125 / rms (l);
                for (auto& v : l) v = (float) (v * scaleTo);
                r = l;
                const double in = rms (l);
                processBlocks (l, r, [&] (float* a, float* b, int n) { sat.process (a, b, n); });
                worst = std::max (worst, std::abs (db (rms (l, 4800) / in)));
            }
        check (worst < 2.5, "auto gain holds the level at -18 dBFS RMS", fmt ("(worst change %.1f dB over all styles and drive settings)", worst));
    }

    // Mix at 0 is the input, delayed by exactly the reported latency.
    {
        Saturator sat;
        SaturatorParams p;
        p.mix = 0;
        p.drive = 1;
        sat.setParams (p);
        sat.prepare (SR);
        Buf l = noise (0.5, 1, 9), r = l, l0 = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { sat.process (a, b, n); });
        const double d = maxDiffDelayed (l, l0, Saturator::kLatency);
        check (d < 1e-6, "dry path matches the reported latency", fmt ("(%g samples, max diff %.1e)", Saturator::kLatency, d));
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { sat.process (a, b, n); }));
    }
}

//==============================================================================
static void testDistortion()
{
    std::printf ("\nDistortion\n");

    // 1. All bands off: the plugin is a clean (all-pass) path at every frequency.
    {
        double worst = 0;
        for (double f : { 60., 150., 500., 900., 2500., 4500., 12000. })
        {
            Distortion d;
            DistortionParams p;
            for (auto& b : p.bands)
                b.enabled = false;
            d.setParams (p);
            d.prepare (SR);
            Buf l = sine (f, 0.25, 1.5), r = l;
            processBlocks (l, r, [&] (float* a, float* b, int n) { d.process (a, b, n); });
            worst = std::max (worst, std::abs (db (toneAmp (l, f, (size_t) SR, l.size()) / 0.25)));
            worst = std::max (worst, thdPercent (l, f, (size_t) SR, l.size()) * 0.01);
        }
        check (worst < 0.05, "clean and flat with every band switched off", fmt ("(worst deviation %.3f dB)", worst));
    }

    // 2. Distorting one band leaves a tone in another band clean.
    {
        Distortion d;
        DistortionParams p;
        for (auto& b : p.bands)
            b.enabled = false;
        p.bands[3].enabled = true;
        p.bands[3].drive = 1;
        p.bands[3].style = ShapeStyle::HardClip;
        d.setParams (p);
        d.prepare (SR);
        Buf l = sine (100, 0.5, 2), r = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { d.process (a, b, n); });
        const double lowThd = thdPercent (l, 100, (size_t) SR, l.size());

        Distortion d2;
        d2.setParams (p);
        d2.prepare (SR);
        Buf h = sine (6000, 0.5, 2), hr = h;
        processBlocks (h, hr, [&] (float* a, float* b, int n) { d2.process (a, b, n); });
        const double highThd = thdPercent (h, 6000, (size_t) SR, h.size());
        check (lowThd < 0.2 && highThd > 5.0, "bands are independent", fmt ("(band 4 driven hard: 100 Hz tone THD %.3f%%, 6 kHz tone THD %.0f%%)", lowThd, highThd));
    }

    // 3. Every curve, with everything turned up, stays bounded.
    {
        bool ok = true;
        double worst = 0;
        for (int s = 0; s < (int) ShapeStyle::Count; ++s)
        {
            Distortion d;
            DistortionParams p;
            for (auto& b : p.bands)
            {
                b.style = (ShapeStyle) s;
                b.drive = 1;
                b.feedback = 0.9f;
                b.dynamics = s % 2 ? 1.0f : -1.0f;
                b.tone = 1;
                b.levelDb = 12;
            }
            d.setParams (p);
            d.prepare (SR);
            Buf l = drums (0.9, 3), r = noise (0.9, 3, 4);
            processBlocks (l, r, [&] (float* a, float* b, int n) { d.process (a, b, n); });
            ok = ok && allFinite (l) && allFinite (r);
            worst = std::max (worst, std::max (peakOf (l), peakOf (r)));
        }
        check (ok && worst < 120.0, "all 12 curves at full drive, feedback and level", fmt ("(peak %.1f dB)", db (worst)));
    }

    // 4. Overall Mix at 0 gives the same clean path as switching the bands off.
    {
        Distortion d;
        DistortionParams p;
        for (auto& b : p.bands)
            b.drive = 1;
        p.mix = 0;
        d.setParams (p);
        d.prepare (SR);
        Buf l = sine (1000, 0.25, 1.5), r = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { d.process (a, b, n); });
        const double thd = thdPercent (l, 1000, (size_t) SR, l.size());
        check (thd < 0.05, "Mix at 0% is clean", fmt ("(THD %.4f%%)", thd));
        Distortion bench;
        bench.prepare (SR);
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { bench.process (a, b, n); }));
    }
}

//==============================================================================
static void testTape()
{
    std::printf ("\nTape\n");
    auto clean = []
    {
        TapeParams p;
        p.inputDb = -6;
        p.wow = p.flutter = 0;
        p.hiss = 0;
        return p;
    };

    // 1. Dry path lines up with the reported latency.
    {
        Tape t;
        auto p = clean();
        p.mix = 0;
        t.setParams (p);
        t.prepare (SR);
        Buf l = noise (0.5, 1, 9), r = l, l0 = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { t.process (a, b, n); });
        const double d = maxDiffDelayed (l, l0, t.getLatencySamples());
        check (d < 1e-6, "dry path matches the reported latency", fmt ("(%g samples, max diff %.1e)", t.getLatencySamples(), d));
    }

    // 2. Tone by speed: head bump at the bottom, earlier roll-off at slower speeds.
    {
        auto level = [&] (TapeSpeed sp, double f, float bumpAmount)
        {
            Tape t;
            auto p = clean();
            p.speed = sp;
            p.headBump = bumpAmount;
            p.inputDb = -12;
            t.setParams (p);
            t.prepare (SR);
            Buf l = sine (f, 0.1, 1.5), r = l;
            processBlocks (l, r, [&] (float* a, float* b, int n) { t.process (a, b, n); });
            return db (toneAmp (l, f, (size_t) SR, l.size()) / 0.1);
        };
        const double mid = level (TapeSpeed::Ips15, 1000, 1), bump = level (TapeSpeed::Ips15, 80, 1) - mid;
        const double top7 = level (TapeSpeed::Ips7, 14000, 0) - level (TapeSpeed::Ips7, 1000, 0);
        const double top30 = level (TapeSpeed::Ips30, 14000, 0) - level (TapeSpeed::Ips30, 1000, 0);
        check (std::abs (mid) < 1.5 && bump > 2.0 && bump < 4.5 && top7 < -3.0 && top30 > -1.5, "speed-dependent tone",
               fmt ("(15 ips: 1 kHz %.1f dB, head bump +%.1f dB at 80 Hz", mid, bump) + fmt ("; 14 kHz: %.1f dB at 7.5 ips, %.1f dB at 30 ips)", top7, top30));
    }

    // 3. Saturation grows with input level.
    {
        auto thd = [&] (float inputDb)
        {
            Tape t;
            auto p = clean();
            p.inputDb = inputDb;
            t.setParams (p);
            t.prepare (SR);
            Buf l = sine (400, 0.25, 2), r = l;
            processBlocks (l, r, [&] (float* a, float* b, int n) { t.process (a, b, n); });
            return thdPercent (l, 400, (size_t) SR, l.size());
        };
        const double quiet = thd (-12), hot = thd (18);
        check (quiet < 1.0 && hot > 5.0, "saturation follows input level", fmt ("(THD %.2f%% at -12 dB input, %.1f%% at +18 dB)", quiet, hot));
    }

    // 4. Wow and flutter move the pitch a little, not a lot.
    {
        Tape t;
        auto p = clean();
        p.wow = 1;
        p.flutter = 1;
        t.setParams (p);
        t.prepare (SR);
        Buf l = sine (1000, 0.2, 6), r = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { t.process (a, b, n); });
        // Track pitch from zero crossings in 50 ms windows.
        double lo = 1e9, hi = 0;
        for (size_t start = (size_t) SR; start + 2400 < l.size(); start += 2400)
        {
            int crossings = 0;
            double first = -1, last = -1;
            for (size_t i = start + 1; i < start + 2400; ++i)
                if (l[i - 1] < 0 && l[i] >= 0)
                {
                    const double tt = (double) i - l[i] / (l[i] - l[i - 1]);
                    if (first < 0) first = tt;
                    last = tt;
                    ++crossings;
                }
            if (crossings > 2)
            {
                const double f = (crossings - 1) * SR / (last - first);
                lo = std::min (lo, f);
                hi = std::max (hi, f);
            }
        }
        const double swing = cents (hi, lo);
        check (swing > 3.0 && swing < 60.0, "wow and flutter at maximum", fmt ("(pitch swings %.0f cents peak to peak)", swing));
    }

    // 5. Hiss: none at zero, a low bed at full.
    {
        auto hissLevel = [&] (float amount)
        {
            Tape t;
            auto p = clean();
            p.hiss = amount;
            t.setParams (p);
            t.prepare (SR);
            Buf l ((size_t) SR, 0.0f), r = l;
            processBlocks (l, r, [&] (float* a, float* b, int n) { t.process (a, b, n); });
            return rms (l, 4800);
        };
        const double off = hissLevel (0), full = db (hissLevel (1));
        check (off == 0.0 && full > -62 && full < -40, "hiss", fmt ("(silent at 0; %.0f dBFS at full)", full));
    }

    // 6. Everything at the extremes.
    {
        bool ok = true;
        for (int sp = 0; sp < 3; ++sp)
            for (int fo = 0; fo < 3; ++fo)
            {
                Tape t;
                TapeParams p;
                p.speed = (TapeSpeed) sp;
                p.formula = (TapeFormula) fo;
                p.inputDb = 24;
                p.bias = sp == 1 ? 1.0f : -1.0f;
                p.headBump = p.wow = p.flutter = p.hiss = 1;
                t.setParams (p);
                t.prepare (SR);
                Buf l = drums (0.9, 2), r = noise (0.9, 2, 3);
                processBlocks (l, r, [&] (float* a, float* b, int n) { t.process (a, b, n); });
                ok = ok && allFinite (l) && allFinite (r) && peakOf (l) < 30.0;
            }
        check (ok, "every speed and formula at extreme settings");
        Tape t;
        t.prepare (SR);
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { t.process (a, b, n); }));
    }
}

//==============================================================================
static void testDeEsser()
{
    std::printf ("\nDe-Esser\n");
    // A "voice": a 200 Hz tone with a burst of 5-10 kHz noise (the "s") in the middle second.
    auto makeVoice = [] (double scale)
    {
        Buf b = sine (200, 0.25 * scale, 3);
        Random rnd (17);
        Biquad hp1, hp2, lp;
        hp1.setCoefs (design::highpass (5000, 0.54, SR));
        hp2.setCoefs (design::highpass (5000, 1.3, SR));
        lp.setCoefs (design::lowpass (10000, 0.7, SR));
        for (size_t i = (size_t) SR; i < (size_t) (2 * SR); ++i)
            b[i] += (float) (0.35 * scale * lp.process (hp2.process (hp1.process (rnd.nextBipolar()))));
        return b;
    };
    auto bandRms = [] (const Buf& b, double f, size_t from, size_t to)
    {
        Biquad hp1, hp2;
        hp1.setCoefs (design::highpass (f, 0.54, SR));
        hp2.setCoefs (design::highpass (f, 1.3, SR));
        double s = 0;
        for (size_t i = 0; i < to; ++i)
        {
            const float v = hp2.process (hp1.process (b[i]));
            if (i >= from)
                s += (double) v * v;
        }
        return std::sqrt (s / (double) (to - from));
    };
    const size_t s0 = (size_t) (1.2 * SR), s1 = (size_t) (1.9 * SR);

    for (int band = 0; band < 2; ++band)
    {
        DeEsser de;
        DeEsserParams p;
        p.mode = DeEssMode::Wide;
        p.band = (DeEssBand) band;
        p.thresholdDb = -30;
        p.rangeDb = 12;
        de.setParams (p);
        de.prepare (SR);
        Buf l = makeVoice (1), r = l, dry = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { de.process (a, b, n); });
        const double essCut = db (bandRms (l, 4500, s0, s1) / bandRms (dry, 4500, s0, s1));
        const double toneDuringEss = db (toneAmp (l, 200, s0, s1) / toneAmp (dry, 200, s0, s1));
        const double toneAlone = db (toneAmp (l, 200, (size_t) (2.3 * SR), l.size()) / toneAmp (dry, 200, (size_t) (2.3 * SR), dry.size()));
        const bool ok = band == 0 ? (essCut < -6 && toneDuringEss < -6 && std::abs (toneAlone) < 0.2)
                                  : (essCut < -6 && std::abs (toneDuringEss) < 0.4 && std::abs (toneAlone) < 0.2);
        check (ok, band == 0 ? "wide-band: the whole signal dips on the \"s\"" : "split-band: only the top dips on the \"s\"",
               fmt ("(\"s\" %.1f dB; 200 Hz tone %.1f dB during it, %.2f dB elsewhere)", essCut, toneDuringEss, toneAlone));
    }

    // Vocal mode behaves the same on a quiet take as on a loud one.
    {
        double cut[2];
        int k = 0;
        for (double scale : { 1.0, 0.1 })
        {
            DeEsser de;
            DeEsserParams p;
            p.mode = DeEssMode::Vocal;
            p.thresholdDb = -30;
            p.rangeDb = 12;
            de.setParams (p);
            de.prepare (SR);
            Buf l = makeVoice (scale), r = l, dry = l;
            processBlocks (l, r, [&] (float* a, float* b, int n) { de.process (a, b, n); });
            cut[k++] = db (bandRms (l, 4500, s0, s1) / bandRms (dry, 4500, s0, s1));
        }
        check (cut[0] < -4 && std::abs (cut[0] - cut[1]) < 2.0, "vocal mode ignores overall level", fmt ("(\"s\" cut %.1f dB at full level, %.1f dB 20 dB quieter)", cut[0], cut[1]));
    }

    {
        DeEsser de;
        DeEsserParams p;
        p.lookaheadMs = 10;
        p.thresholdDb = 0;
        de.setParams (p);
        de.prepare (SR);
        Buf l (4800, 0.0f), r = l;
        l[10] = r[10] = 1.0f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { de.process (a, b, n); });
        double early = 0;
        for (size_t i = 0; i < 10 + (size_t) de.getLatencySamples(); ++i)
            early += (double) l[i] * l[i];
        check (de.getLatencySamples() == 480 && early < 1e-12, "lookahead latency", fmt ("(%g samples)", de.getLatencySamples()));

        DeEsser hard;
        DeEsserParams q;
        q.thresholdDb = -60;
        q.rangeDb = 24;
        q.lookaheadMs = 15;
        q.stereoLink = 0;
        hard.setParams (q);
        hard.prepare (SR);
        Buf a = noise (0.9, 3, 5), b = drums (0.9, 3);
        processBlocks (a, b, [&] (float* x, float* y, int n) { hard.process (x, y, n); });
        check (allFinite (a) && allFinite (b) && peakOf (a) < 2.0, "extreme settings stay finite");
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* x, float* y, int n) { hard.process (x, y, n); }));
    }
}

//==============================================================================
static void testChannelStrip()
{
    std::printf ("\nChannel Strip\n");
    {
        ChannelStrip cs;
        cs.setParams ({});
        cs.prepare (SR);
        Buf l = noise (0.5, 1, 3), r = noise (0.5, 1, 4), l0 = l, r0 = r;
        processBlocks (l, r, [&] (float* a, float* b, int n) { cs.process (a, b, n); });
        check (l == l0 && r == r0, "bit-exact at default settings");
    }
    {
        ChannelStripParams p;
        p.hmfGainDb = 6;
        p.hmfHz = 3000;
        p.hpfHz = 100;
        const double peak = ChannelStrip::responseDb (p, 3000, SR), corner = ChannelStrip::responseDb (p, 100, SR);
        ChannelStrip cs;
        cs.setParams (p);
        cs.prepare (SR);
        Buf l = sine (3000, 0.1, 1.5), r = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { cs.process (a, b, n); });
        const double measured = db (toneAmp (l, 3000, (size_t) SR, l.size()) / 0.1);
        check (std::abs (measured - 6.0) < 0.1 && std::abs (peak - 6.0) < 0.1 && std::abs (corner + 3.0) < 0.3, "EQ and filter accuracy",
               fmt ("(HMF +6 dB measures %.2f dB; high-pass is %.1f dB at its corner)", measured, corner));
    }
    {
        // "Tight" narrows a bell as it is pushed; "Smooth" keeps its width.
        ChannelStripParams p;
        p.lmfGainDb = 12;
        p.lmfHz = 1000;
        p.lmfQ = 1;
        const double smooth = ChannelStrip::responseDb (p, 1600, SR);
        p.eqType = StripEqType::Tight;
        const double tight = ChannelStrip::responseDb (p, 1600, SR);
        check (tight < smooth - 1.5, "two EQ characters", fmt ("(+12 dB bell at 1 kHz, level at 1.6 kHz: Smooth %.1f dB, Tight %.1f dB)", smooth, tight));
    }
    {
        ChannelStrip cs;
        ChannelStripParams p;
        p.compThresholdDb = -20;
        p.compRatio = 4;
        cs.setParams (p);
        cs.prepare (SR);
        Buf l = sine (1000, 0.5, 2), r = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { cs.process (a, b, n); });
        const double g = cs.getCompReductionDb();
        check (g < -9.0 && g > -11.5, "compressor ratio", fmt ("(%.1f dB on a -6 dBFS tone at 4:1, -20 dB threshold; ideal -10.5)", g));
    }
    {
        auto gated = [] (double amp, bool expander)
        {
            ChannelStrip cs;
            ChannelStripParams p;
            p.gateThresholdDb = -30;
            p.gateRangeDb = 30;
            p.gateExpander = expander;
            cs.setParams (p);
            cs.prepare (SR);
            Buf l = sine (500, amp, 2), r = l;
            processBlocks (l, r, [&] (float* a, float* b, int n) { cs.process (a, b, n); });
            return db (toneAmp (l, 500, (size_t) SR, l.size()) / amp);
        };
        const double loud = gated (0.25, false), quiet = gated (0.003, false), expanded = gated (0.01, true);
        check (std::abs (loud) < 0.1 && quiet < -29 && expanded < -8 && expanded > -12, "gate and expander",
               fmt ("(gate: loud %.1f dB, quiet %.0f dB", loud, quiet) + fmt ("; expander 10 dB under threshold: %.1f dB)", expanded));
    }
    {
        // Drive: audible harmonics, far less aliasing than a plain tanh.
        ChannelStrip cs;
        ChannelStripParams p;
        p.drive = 1;
        cs.setParams (p);
        cs.prepare (SR);
        Buf l = sine (5000, 0.7, 2), r = l, naive = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { cs.process (a, b, n); });
        for (auto& v : naive)
            v = std::tanh (v * 4.0f) / 4.0f;
        const double alias = inharmonicDb (l, 5000, (size_t) SR), aliasNaive = inharmonicDb (naive, 5000, (size_t) SR);
        check (alias < aliasNaive - 5.0, "input drive is anti-aliased", fmt ("(aliasing %.0f dB, against %.0f dB for a plain curve)", alias, aliasNaive));
    }
    {
        ChannelStrip cs;
        ChannelStripParams p;
        p.inputDb = 20;
        p.drive = 1;
        p.hpfHz = 350;
        p.lpfHz = 3000;
        p.lfGainDb = p.lmfGainDb = p.hmfGainDb = p.hfGainDb = 15;
        p.eqType = StripEqType::Tight;
        p.compThresholdDb = -50;
        p.compRatio = 20;
        p.compFastAttack = true;
        p.compMakeupDb = 20;
        p.gateThresholdDb = -10;
        p.gateRangeDb = 40;
        p.order = StripOrder::DynamicsThenEq;
        cs.setParams (p);
        cs.prepare (SR);
        Buf l = drums (0.9, 4), r = noise (0.9, 4, 6);
        processBlocks (l, r, [&] (float* a, float* b, int n) { cs.process (a, b, n); });
        check (allFinite (l) && allFinite (r), "extreme settings stay finite", fmt ("(peak %.1f dB)", db (peakOf (l))));
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { cs.process (a, b, n); }));
    }
}

//==============================================================================
static void testPlate()
{
    std::printf ("\nPlate\n");
    const char* names[] = { "Classic", "Bright", "Dark", "Dense", "Thin", "Vintage" };

    for (double target : { 1.0, 2.5, 6.0 })
    {
        Plate pl;
        PlateParams p;
        p.mix = 1;
        p.decaySec = (float) target;
        p.dampHz = 16000;
        p.predelayMs = 0;
        p.modDepth = 0;
        p.lowCutHz = 20;
        pl.setParams (p);
        pl.prepare (SR);
        Buf l ((size_t) (SR * (target * 1.6 + 1)), 0.0f), r = l;
        l[0] = r[0] = 1.0f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { pl.process (a, b, n); });
        // Judge the decay in the 1 kHz region, away from the damping filter.
        Biquad b1, b2;
        b1.setCoefs (design::bandpass (1000, 1.4, SR));
        b2.setCoefs (design::bandpass (1000, 1.4, SR));
        for (auto& v : l)
            v = b2.process (b1.process (v));
        const double rt = measureRt60 (l, SR);
        check (std::abs (rt / target - 1.0) < 0.2, fmt ("decay time, %.1f s", target), fmt ("(measured %.2f s)", rt));
    }

    for (int t = 0; t < (int) PlateType::Count; ++t)
    {
        Plate pl;
        PlateParams p;
        p.type = (PlateType) t;
        p.mix = 1;
        p.decaySec = 2;
        pl.setParams (p);
        pl.prepare (SR);
        Buf l = drums (0.5, 6), r = l;
        const double dryRms = rms (l, 0, (size_t) (3 * SR));
        for (size_t i = (size_t) (3 * SR); i < l.size(); ++i)
            l[i] = r[i] = 0.0f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { pl.process (a, b, n); });
        const double wet = rms (l, (size_t) SR, (size_t) (3 * SR)), tail = rms (l, (size_t) (5.5 * SR));
        double lr = 0, ll = 0, rr = 0;
        for (size_t i = (size_t) SR; i < (size_t) (3 * SR); ++i)
        {
            lr += (double) l[i] * r[i];
            ll += (double) l[i] * l[i];
            rr += (double) r[i] * r[i];
        }
        const double corr = lr / std::sqrt (ll * rr + 1e-30), rel = db (wet / dryRms);
        check (allFinite (l) && allFinite (r) && rel > -6 && rel < 1 && tail < wet * 0.05 && std::abs (corr) < 0.6, names[t],
               fmt ("(wet %.1f dB vs dry, L/R correlation %.2f, tail after 2.5 s %.0f dB)", rel, corr, db (tail / (wet + 1e-12))));
    }

    {
        bool ok = true;
        for (int t = 0; t < (int) PlateType::Count; ++t)
        {
            Plate pl;
            PlateParams p;
            p.type = (PlateType) t;
            p.mix = 1;
            p.decaySec = 20;
            p.size = 1;
            p.modDepth = 1;
            p.dampHz = 16000;
            pl.setParams (p);
            pl.prepare (SR);
            Buf l = noise (1.0, 15, 21), r = noise (1.0, 15, 22);
            processBlocks (l, r, [&] (float* a, float* b, int n) { pl.process (a, b, n); });
            ok = ok && allFinite (l) && peakOf (l) < 60.0;
        }
        check (ok, "longest decay with loud noise stays bounded");

        Plate pl;
        PlateParams p;
        p.mix = 0;
        pl.setParams (p);
        pl.prepare (SR);
        Buf l = noise (0.5, 1, 41), r = noise (0.5, 1, 42), l0 = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { pl.process (a, b, n); });
        check (maxDiffDelayed (l, l0, 0) < 1e-6, "dry at 0% mix");
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { pl.process (a, b, n); }));
    }
}

//==============================================================================
static void testSpace()
{
    std::printf ("\nSpace\n");

    // 1. Simplest setting is an exact echo.
    {
        Space sp;
        SpaceParams p;
        p.mode = SpaceMode::Canyon;
        p.mix = 1;
        p.delayMs = 250;
        p.warp = 0;
        p.density = 0;
        p.modDepth = 0;
        p.feedback = 0.5f;
        p.lowCutHz = 10;
        p.highCutHz = 20000;
        sp.setParams (p);
        sp.prepare (SR);
        Buf l ((size_t) (2 * SR), 0.0f), r = l;
        l[0] = 0.5f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { sp.process (a, b, n); });
        const auto el = findEchoes (l, 0.02, 2000), er = findEchoes (r, 0.02, 2000);
        const bool ok = ! el.empty() && ! er.empty() && el[0].first == 12000 && er[0].first == 24000;
        check (ok, "echo timing with warp, density and modulation at zero",
               fmt ("(left at %g, right at %g samples; expected 12000 and 24000)", el.empty() ? -1.0 : (double) el[0].first, er.empty() ? -1.0 : (double) er[0].first));
    }

    // 2. Every mode: sensible level, decays, bounded at full feedback, holds when frozen.
    for (int m = 0; m < (int) SpaceMode::Count; ++m)
    {
        const auto& md = Space::modeData ((SpaceMode) m);
        Space sp;
        SpaceParams p;
        p.mode = (SpaceMode) m;
        p.mix = 1;
        sp.setParams (p);
        sp.prepare (SR);
        Buf l = drums (0.5, 14), r = l;
        const double dryRms = rms (l, 0, (size_t) (3 * SR));
        for (size_t i = (size_t) (3 * SR); i < l.size(); ++i)
            l[i] = r[i] = 0.0f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { sp.process (a, b, n); });
        const double wet = rms (l, (size_t) SR, (size_t) (3 * SR)), tail = rms (l, (size_t) (13 * SR));
        const double rel = db (wet / dryRms);

        Space loud;
        p.feedback = 1;
        p.modDepth = 1;
        p.density = 1;
        loud.setParams (p);
        loud.prepare (SR);
        Buf a = noise (1.0, 20, 31), b = noise (1.0, 20, 32);
        processBlocks (a, b, [&] (float* x, float* y, int n) { loud.process (x, y, n); });

        Space frozen;
        SpaceParams q;
        q.mode = (SpaceMode) m;
        q.mix = 1;
        frozen.setParams (q);
        frozen.prepare (SR);
        Buf f1 = noise (0.3, 2, 33), f2 = noise (0.3, 2, 34);
        processBlocks (f1, f2, [&] (float* x, float* y, int n) { frozen.process (x, y, n); });
        q.freeze = true;
        q.modDepth = 0;
        frozen.setParams (q);
        Buf g1 ((size_t) (10 * SR), 0.0f), g2 = g1;
        processBlocks (g1, g2, [&] (float* x, float* y, int n) { frozen.process (x, y, n); });
        const double hold = db (rms (g1, (size_t) (8 * SR), (size_t) (9 * SR)) / (rms (g1, (size_t) SR, (size_t) (2 * SR)) + 1e-12));

        const bool ok = allFinite (l) && rel > -8 && rel < 3 && tail < wet * 0.1 && allFinite (a) && peakOf (a) < 30.0 && hold > -3.0 && hold < 1.0;
        check (ok, md.name, fmt ("(wet %.1f dB vs dry; tail 10 s later %.0f dB", rel, db (tail / (wet + 1e-12)))
                                + fmt ("; full feedback peak %.1f dB; frozen level drifts %.1f dB in 7 s)", db (peakOf (a)), hold));
    }

    {
        Space sp;
        SpaceParams p;
        p.mix = 0;
        sp.setParams (p);
        sp.prepare (SR);
        Buf l = noise (0.5, 1, 41), r = noise (0.5, 1, 42), l0 = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { sp.process (a, b, n); });
        check (maxDiffDelayed (l, l0, 0) < 1e-6, "dry at 0% mix");
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { sp.process (a, b, n); }));
    }
}

//==============================================================================
static void testSmooth()
{
    std::printf ("\nSmooth\n");

    // 1. With depth at zero the spectral engine hands back the input, just delayed.
    {
        Smooth sm;
        SmoothParams p;
        p.depth = 0;
        p.selectivity = 1;
        sm.setParams (p);
        sm.prepare (SR);
        Buf l = noise (0.4, 2, 3), r = noise (0.4, 2, 4), l0 = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { sm.process (a, b, n); });
        // Depth 0 still allows a little reduction, so use the delta/identity relation below
        // for exactness and just require the levels to agree here.
        const double change = db (rms (l, (size_t) SR) / rms (l0, (size_t) SR));
        check (std::abs (change) < 0.5, "near-transparent at minimum depth", fmt ("(level change %.2f dB, latency %g samples)", change, sm.getLatencySamples()));
    }

    // 2. Removed + kept = original: the delta output is exactly what was taken out.
    {
        Smooth wet, delta;
        SmoothParams p;
        p.depth = 1;
        p.selectivity = 0;
        wet.setParams (p);
        wet.prepare (SR);
        p.delta = true;
        delta.setParams (p);
        delta.prepare (SR);
        Buf in = drums (0.6, 2), a = in, ar = in, b = in, br = in;
        processBlocks (a, ar, [&] (float* x, float* y, int n) { wet.process (x, y, n); });
        processBlocks (b, br, [&] (float* x, float* y, int n) { delta.process (x, y, n); });
        Buf sum (in.size());
        for (size_t i = 0; i < in.size(); ++i)
            sum[i] = a[i] + b[i];
        const double d = maxDiffDelayed (sum, in, wet.getLatencySamples());
        check (d < 1e-4, "delta plus output equals the delayed input", fmt ("(max error %.1e)", d));
    }

    // 3. A resonance poking out of a noise bed is pulled down; the bed is left alone.
    {
        Smooth sm;
        SmoothParams p;
        p.depth = 1;
        p.selectivity = 0.3f;
        p.sharpness = 0.6f;
        sm.setParams (p);
        sm.prepare (SR);
        Buf l = noise (0.05, 4, 8);
        Biquad tilt;
        tilt.setCoefs (design::lowpass (2500, 0.6, SR));
        for (auto& v : l)
            v = tilt.process (v);
        Buf bed = l;
        for (size_t i = 0; i < l.size(); ++i)
            l[i] += (float) (0.15 * std::sin (kTwoPi * 3100.0 * (double) i / SR));
        Buf r = l, dry = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { sm.process (a, b, n); });
        const size_t from = (size_t) (2 * SR);
        const double toneCut = db (toneAmp (l, 3100, from, l.size()) / toneAmp (dry, 3100, from, dry.size()));
        // Noise bed level, measured below the resonance.
        auto lowBand = [] (const Buf& b, size_t f0)
        {
            Biquad lp1, lp2;
            lp1.setCoefs (design::lowpass (1200, 0.54, SR));
            lp2.setCoefs (design::lowpass (1200, 1.3, SR));
            double s = 0;
            for (size_t i = 0; i < b.size(); ++i)
            {
                const float v = lp2.process (lp1.process (b[i]));
                if (i >= f0)
                    s += (double) v * v;
            }
            return std::sqrt (s / (double) (b.size() - f0));
        };
        const double bedChange = db (lowBand (l, from) / lowBand (dry, from));
        check (toneCut < -6.0 && std::abs (bedChange) < 1.5, "tames a resonance, leaves the rest",
               fmt ("(3.1 kHz resonance %.1f dB; material below 1.2 kHz %.2f dB)", toneCut, bedChange));
    }

    {
        Smooth sm;
        SmoothParams p;
        p.depth = 1;
        p.sharpness = 1;
        p.selectivity = 0;
        p.attackMs = 1;
        p.releaseMs = 10;
        p.lowHz = 20;
        p.highHz = 20000;
        sm.setParams (p);
        sm.prepare (SR);
        Buf l = drums (0.9, 4), r = noise (0.9, 4, 12);
        processBlocks (l, r, [&] (float* a, float* b, int n) { sm.process (a, b, n); });
        check (allFinite (l) && allFinite (r) && peakOf (l) < 3.0, "extreme settings stay finite", fmt ("(peak %.1f dB)", db (peakOf (l))));
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { sm.process (a, b, n); }));
    }
}

//==============================================================================
static void testTune()
{
    std::printf ("\nTune\n");
    auto run = [] (const TuneParams& p, Buf in)
    {
        Tune t;
        t.setParams (p);
        t.prepare (SR);
        Buf r = in;
        processBlocks (in, r, [&] (float* a, float* b, int n) { t.process (a, b, n); });
        return in;
    };
    const size_t from = (size_t) (1.5 * SR), len = (size_t) (0.5 * SR);

    // 1. A note 35 cents sharp is pulled to pitch.
    {
        TuneParams p;
        p.retuneMs = 0;
        const double inHz = 440.0 * std::pow (2.0, 35.0 / 1200.0);
        Buf in = voiceTone (inHz, 0.3, 2.5);
        Buf out = run (p, in);
        const double f = measureHz (out, from, len, 150, 900);
        const double level = db (rms (out, from, from + len) / rms (in, from, from + len));
        check (std::abs (cents (f, 440.0)) < 4.0 && std::abs (level) < 1.5, "chromatic correction", fmt ("(A4 +35 cents in; out %.1f cents from A4, level %.2f dB)", cents (f, 440.0), level));
    }

    // 2. Notes outside the scale move to the nearest note in it.
    {
        TuneParams p;
        p.retuneMs = 0;
        p.key = 0;
        p.scale = TuneScale::Major;
        const double cSharp = 277.183 * std::pow (2.0, 10.0 / 1200.0); // C#4 +10 cents: nearest note in C major is D4
        Buf out = run (p, voiceTone (cSharp, 0.3, 2.5));
        const double f = measureHz (out, from, len, 150, 900);
        check (std::abs (cents (f, 293.665)) < 6.0, "scale: C#4 in C major goes to D4", fmt ("(out %.1f cents from D4)", cents (f, 293.665)));
    }

    // 3. Across the range, low and high notes.
    {
        double worst = 0;
        struct Case { double hz; TuneRange range; };
        for (auto c : { Case { 98.0, TuneRange::Low }, Case { 146.83, TuneRange::Mid }, Case { 329.63, TuneRange::Mid }, Case { 659.26, TuneRange::High }, Case { 880.0, TuneRange::High } })
        {
            TuneParams p;
            p.retuneMs = 0;
            p.range = c.range;
            Buf out = run (p, voiceTone (c.hz * std::pow (2.0, -30.0 / 1200.0), 0.3, 2.5));
            const double f = measureHz (out, from, len, c.hz * 0.6, c.hz * 1.6);
            worst = std::max (worst, std::abs (cents (f, c.hz)));
        }
        check (worst < 6.0, "notes from G2 to A5, each 30 cents flat", fmt ("(worst result %.1f cents off)", worst));
    }

    // 4. Transpose and formant.
    {
        TuneParams p;
        p.retuneMs = 0;
        p.transpose = 3;
        Buf out = run (p, voiceTone (220.0, 0.3, 2.5));
        const double f = measureHz (out, from, len, 150, 900);
        TuneParams q;
        q.retuneMs = 0;
        q.formant = 4;
        Buf out2 = run (q, voiceTone (220.0, 0.3, 2.5));
        const double f2 = measureHz (out2, from, len, 150, 900);
        check (std::abs (cents (f, 261.626)) < 6.0 && std::abs (cents (f2, 220.0)) < 6.0 && allFinite (out2), "transpose and formant",
               fmt ("(+3 semitones: %.1f cents from C4; formant +4: pitch stays within %.1f cents)", cents (f, 261.626), cents (f2, 220.0)));
    }

    // 5. Slow retune lets vibrato through; fast retune flattens it.
    {
        auto vibrato = [] ()
        {
            Buf b ((size_t) (3 * SR));
            double ph = 0;
            for (size_t i = 0; i < b.size(); ++i)
            {
                const double f = 440.0 * std::pow (2.0, 40.0 * std::sin (kTwoPi * 5.5 * (double) i / SR) / 1200.0);
                ph += kTwoPi * f / SR;
                double v = 0;
                for (int h = 1; h <= 8; ++h)
                    v += std::sin (ph * h) / h;
                b[i] = (float) (0.2 * v);
            }
            return b;
        };
        auto swing = [] (const Buf& b)
        {
            double lo = 1e9, hi = 0;
            for (size_t start = (size_t) (1.5 * SR); start + 1500 < (size_t) (2.8 * SR); start += 480)
            {
                const double f = measureHz (b, start, 1500, 300, 600);
                if (f > 0)
                {
                    lo = std::min (lo, f);
                    hi = std::max (hi, f);
                }
            }
            return cents (hi, lo);
        };
        TuneParams fast, slow;
        fast.retuneMs = 0;
        slow.retuneMs = 400;
        const double inSwing = swing (vibrato()), fastSwing = swing (run (fast, vibrato())), slowSwing = swing (run (slow, vibrato()));
        check (fastSwing < inSwing * 0.45 && slowSwing > inSwing * 0.6, "retune speed", fmt ("(vibrato %.0f cents in; %.0f at 0 ms, ", inSwing, fastSwing) + fmt ("%.0f at 400 ms)", slowSwing));
    }

    // 6. Unpitched material passes at the same level; silence stays silent.
    {
        TuneParams p;
        Buf in = noise (0.3, 3, 77);
        Buf out = run (p, in);
        const double level = db (rms (out, (size_t) SR) / rms (in, (size_t) SR));
        Buf quiet = run (p, Buf ((size_t) SR, 0.0f));
        check (allFinite (out) && std::abs (level) < 1.5 && peakOf (quiet) == 0.0, "noise and silence", fmt ("(noise level %.2f dB)", level));
    }

    // 7. Messy input, all scales and settings: nothing breaks.
    {
        bool ok = true;
        for (int s = 0; s < (int) TuneScale::Count; ++s)
        {
            Tune t;
            TuneParams p;
            p.scale = (TuneScale) s;
            p.key = s;
            p.range = (TuneRange) (s % 3);
            p.transpose = s % 2 ? 12.0f : -12.0f;
            p.formant = s % 2 ? -6.0f : 6.0f;
            p.humanize = 1;
            t.setParams (p);
            t.prepare (SR);
            Buf l = drums (0.8, 3), r = voiceTone (130 + 40 * s, 0.4, 3);
            for (size_t i = 0; i < l.size(); ++i)
                l[i] += r[i] * (float) (0.5 + 0.5 * std::sin ((double) i * 0.0003));
            processBlocks (l, r, [&] (float* a, float* b, int n) { t.process (a, b, n); });
            ok = ok && allFinite (l) && allFinite (r) && peakOf (l) < 8.0;
        }
        check (ok, "every scale with extreme transpose and formant");
        Tune t;
        t.prepare (SR);
        Buf v = voiceTone (220, 0.3, 10.2);
        size_t pos = 0;
        std::printf ("  CPU on a sung note: %.2f%% of one core, latency %d samples in the mid range\n",
                     benchmark ([&] (float* a, float* b, int n) { std::copy (v.begin() + (long) pos, v.begin() + (long) pos + n, a); std::copy (a, a + n, b); pos += (size_t) n; t.process (a, b, n); }),
                     t.getLatencySamples());
    }
}

//==============================================================================
static void testShaper()
{
    std::printf ("\nShaper\n");

    // 1. Volume follows the curve in time with the transport.
    {
        Shaper sh;
        ShaperParams p;
        p.shape = 6;   // Saw Down
        p.length = 2;  // one beat
        p.smooth = 0;
        sh.setParams (p);
        sh.prepare (SR);
        sh.setTransport (0.0, 120.0, true); // one beat = 0.5 s
        Buf l ((size_t) (0.5 * SR), 1.0f), r = l;
        sh.process (l.data(), r.data(), (int) l.size());
        const double q = l[(size_t) (0.125 * SR)], h = l[(size_t) (0.25 * SR)], e = l[(size_t) (0.45 * SR)];
        check (std::abs (q - 0.75) < 0.01 && std::abs (h - 0.5) < 0.01 && std::abs (e - 0.1) < 0.01, "volume follows the curve",
               fmt ("(saw down over one beat: %.3f, %.3f, %.3f at 25%%, 50%%, 90%%)", q, h, e));
    }

    // 2. A hand-drawn curve replaces the built-in one.
    {
        Shaper sh;
        float table[Shaper::kTable + 1];
        const float xs[3] = { 0.0f, 0.5f, 1.0f }, ys[3] = { 0.2f, 0.8f, 0.2f };
        Shaper::buildTable (xs, ys, 3, table);
        sh.setCustomTable (table);
        ShaperParams p;
        p.shape = 0;
        p.smooth = 0;
        sh.setParams (p);
        sh.prepare (SR);
        sh.setTransport (0.0, 120.0, true);
        Buf l ((size_t) (0.5 * SR), 1.0f), r = l;
        sh.process (l.data(), r.data(), (int) l.size());
        check (std::abs (l[(size_t) (0.25 * SR)] - 0.8) < 0.01 && std::abs (l[300] - 0.2) < 0.02, "own curve", fmt ("(%.3f at the midpoint, %.3f at the start)", l[(size_t) (0.25 * SR)], l[300]));
    }

    // 3. Audio trigger: each hit restarts the curve, which then plays once and holds.
    {
        Shaper sh;
        ShaperParams p;
        p.shape = 1; // Duck
        p.trigger = ShaperTrigger::Audio;
        p.length = 1; // half a beat = 0.25 s at 120
        p.smooth = 0;
        sh.setParams (p);
        sh.prepare (SR);
        sh.setTransport (0.0, 120.0, false);
        Buf l ((size_t) (2 * SR), 0.0f);
        for (size_t i = 0; i < l.size(); ++i)
        {
            const double t = std::fmod ((double) i / SR, 1.0);
            l[i] = (float) (0.02 * std::sin (kTwoPi * 300 * (double) i / SR) + (t < 0.01 ? 0.8 * std::sin (kTwoPi * 80 * t) : 0.0));
        }
        Buf r = l, dry = l;
        sh.process (l.data(), r.data(), (int) l.size());
        const size_t afterHit = (size_t) (1.03 * SR), recovered = (size_t) (1.6 * SR);
        const double ducked = db (rms (l, afterHit, afterHit + 480) / rms (dry, afterHit, afterHit + 480));
        const double open = db (rms (l, recovered, recovered + 4800) / rms (dry, recovered, recovered + 4800));
        check (ducked < -12 && std::abs (open) < 0.2, "audio trigger", fmt ("(%.0f dB just after a hit, %.2f dB once the curve has finished)", ducked, open));
    }

    // 4. Filter, pan and width: finite everywhere, and Mix at 0 is the input.
    {
        bool ok = true;
        for (int shape = 0; shape <= Shaper::kNumShapes; ++shape)
            for (int ft = 0; ft < 3; ++ft)
            {
                Shaper sh;
                ShaperParams p;
                p.shape = shape;
                p.length = shape % Shaper::kNumLengths;
                p.filter = 1;
                p.filterType = (ShaperFilter) ft;
                p.resonance = 1;
                p.filterLowHz = 20;
                p.filterHighHz = 20000;
                p.pan = p.width = p.volume = 1;
                p.smooth = 0;
                sh.setParams (p);
                sh.prepare (SR);
                sh.setTransport (3.7, 174.0, true);
                Buf l = drums (0.9, 2), r = noise (0.9, 2, 2);
                processBlocks (l, r, [&] (float* a, float* b, int n) { sh.process (a, b, n); });
                ok = ok && allFinite (l) && allFinite (r) && peakOf (l) < 40.0;
            }
        Shaper sh;
        ShaperParams p;
        p.mix = 0;
        p.filter = 1;
        sh.setParams (p);
        sh.prepare (SR);
        sh.setTransport (0, 120, true);
        Buf l = noise (0.5, 1, 5), r = noise (0.5, 1, 6), l0 = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { sh.process (a, b, n); });
        check (ok && maxDiffDelayed (l, l0, 0) < 1e-6, "all shapes and filter types stay finite; dry at 0% mix");
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { sh.process (a, b, n); }));
    }
}

int main()
{
    testClassicComps();
    testMultiband();
    testSaturator();
    testDistortion();
    testTape();
    testDeEsser();
    testChannelStrip();
    testPlate();
    testSpace();
    testSmooth();
    testTune();
    testShaper();
    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
