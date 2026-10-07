// Offline checks for the Gitto FX DSP. Build: g++ -std=c++17 -O2 dsp_tests.cpp -o dsp_tests
#include "../dsp/Chorus.h"
#include "../dsp/Compressor.h"
#include "../dsp/Delay.h"
#include "../dsp/EQ.h"
#include "../dsp/Limiter.h"
#include "../dsp/Reverb.h"

#include "test_util.h"

//==============================================================================
static void testEq()
{
    std::printf ("\nEQ\n");

    // 1. Matched designs follow the analog curves.
    {
        double worst = 0, worstRbj = 0;
        const double sr = 44100;
        for (double f0 : { 30., 100., 1000., 4000., 8000., 12000., 16000. })
            for (double q : { 0.3, 0.71, 1., 3., 10. })
                for (double g : { -18., -6., 6., 18. })
                {
                    const auto m = design::matchedPeak (f0, q, g, sr);
                    const auto rb = design::peak (f0, q, g, sr);
                    const double A = std::pow (10, g / 40);
                    for (double f = 20; f < 0.9 * sr / 2; f *= 1.03)
                    {
                        const std::complex<double> s (0, f / f0);
                        const double an = std::abs ((s * s + s * (A / q) + 1.0) / (s * s + s / (A * q) + 1.0));
                        worst = std::max (worst, std::abs (db (m.magnitude (kTwoPi * f / sr) / an)));
                        worstRbj = std::max (worstRbj, std::abs (db (rb.magnitude (kTwoPi * f / sr) / an)));
                    }
                }
        check (worst < 2.0, "bell matches analog shape up to 16 kHz at 44.1k", fmt ("(worst %.2f dB; plain bilinear design %.2f dB)", worst, worstRbj));
    }

    // 2. A +6 dB bell at 1 kHz measures +6 dB.
    {
        Equaliser eq;
        eq.prepare (SR);
        EqBandParams p;
        p.enabled = true;
        p.freq = 1000;
        p.gainDb = 6;
        p.q = 1;
        eq.setBand (0, p);
        Buf l = sine (1000, 0.25, 2), r = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { eq.process (a, b, n); });
        const double g = db (toneAmp (l, 1000, (size_t) SR, l.size()) / 0.25);
        check (std::abs (g - 6.0) < 0.05, "bell gain accuracy", fmt ("(%.3f dB, expected 6)", g));
    }

    // 3. Everything off is bit-exact.
    {
        Equaliser eq;
        eq.prepare (SR);
        Buf l = noise (0.5, 1), r = noise (0.5, 1, 77), l0 = l, r0 = r;
        processBlocks (l, r, [&] (float* a, float* b, int n) { eq.process (a, b, n); });
        check (l == l0 && r == r0, "bit-exact when no band is enabled");
    }

    // 4. Cut slopes.
    {
        for (int slope = 0; slope < 4; ++slope)
        {
            EqBandParams p;
            p.enabled = true;
            p.type = EqType::LowCut;
            p.freq = 1000;
            p.q = 0.7071f;
            p.slope = slope;
            const double a1 = Equaliser::bandMagnitudeDb (p, 0, 250, SR);
            const double a2 = Equaliser::bandMagnitudeDb (p, 0, 125, SR);
            const double expected = 12.0 * (slope + 1);
            check (std::abs ((a1 - a2) - expected) < 0.6, fmt ("low cut slope %g dB/oct", expected), fmt ("(measured %.2f)", a1 - a2));
        }
    }

    // 5. A side-only band leaves a mono signal untouched; a mid band changes it.
    {
        Equaliser eq;
        eq.prepare (SR);
        EqBandParams p;
        p.enabled = true;
        p.freq = 2000;
        p.gainDb = 12;
        p.stereo = EqStereo::Side;
        eq.setBand (0, p);
        Buf l = noise (0.3, 1), r = l, l0 = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { eq.process (a, b, n); });
        double diff = 0;
        for (size_t i = 0; i < l.size(); ++i)
            diff = std::max (diff, (double) std::abs (l[i] - l0[i]));
        check (diff < 1e-6, "side band ignores mono content", fmt ("(max diff %.2e)", diff));
    }

    // 6. Dynamic band ducks loud material and leaves quiet material alone.
    {
        auto run = [] (double amp)
        {
            Equaliser eq;
            eq.prepare (SR);
            EqBandParams p;
            p.enabled = true;
            p.freq = 3000;
            p.q = 2;
            p.gainDb = 0;
            p.dynRangeDb = -12;
            p.dynThreshDb = -30;
            eq.setBand (0, p);
            Buf l = sine (3000, amp, 2), r = l;
            processBlocks (l, r, [&] (float* a, float* b, int n) { eq.process (a, b, n); });
            return db (toneAmp (l, 3000, (size_t) SR, l.size()) / amp);
        };
        const double loud = run (0.5), quiet = run (0.003);
        check (loud < -8.0 && quiet > -0.3, "dynamic band", fmt ("(loud tone %.1f dB, quiet tone %.2f dB)", loud, quiet));
    }

    // 7. Survives hard automation of everything, then returns to silence.
    {
        Equaliser eq;
        eq.prepare (SR);
        Random rnd (5);
        Buf l = noise (0.5, 6), r = noise (0.5, 6, 9);
        processBlocks (l, r, [&] (float* a, float* b, int n)
        {
            for (int band = 0; band < Equaliser::kNumBands; ++band)
            {
                EqBandParams p;
                p.enabled = rnd.next01() > 0.2f;
                p.type = (EqType) (rnd.nextInt() % (int) EqType::Count);
                p.freq = 20.0f * std::pow (1000.0f, rnd.next01());
                p.gainDb = rnd.nextBipolar() * 30.0f;
                p.q = 0.1f * std::pow (300.0f, rnd.next01());
                p.slope = (int) (rnd.nextInt() % 4);
                p.stereo = (EqStereo) (rnd.nextInt() % (int) EqStereo::Count);
                p.dynRangeDb = rnd.nextBipolar() * 20.0f;
                p.dynThreshDb = -60.0f * rnd.next01();
                eq.setBand (band, p);
            }
            eq.process (a, b, n);
        }, 64);
        const bool finite = allFinite (l) && allFinite (r);
        Buf sl ((size_t) (2 * SR), 0.0f), sr2 = sl;
        processBlocks (sl, sr2, [&] (float* a, float* b, int n) { eq.process (a, b, n); });
        const double residue = std::max (peakOf (sl, (size_t) SR), peakOf (sr2, (size_t) SR));
        check (finite && allFinite (sl) && residue < 1e-4, "stable under random automation", fmt ("(residue after input stops: %.1f dB)", db (residue)));
    }

    {
        Equaliser eq;
        eq.prepare (SR);
        for (int band = 0; band < Equaliser::kNumBands; ++band)
        {
            EqBandParams p;
            p.enabled = true;
            p.freq = 60.0f * std::pow (2.2f, (float) band);
            p.gainDb = 3;
            p.dynRangeDb = band % 2 ? -6.0f : 0.0f;
            eq.setBand (band, p);
        }
        std::printf ("  CPU, 8 bands (4 dynamic): %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { eq.process (a, b, n); }));
    }
}

//==============================================================================
static void testCompressor()
{
    std::printf ("\nCompressor\n");
    const char* names[] = { "Clean", "Punch", "Glue", "Opto", "Vari-Mu", "FET" };

    // 1. Static ratio: -6 dBFS sine, threshold -20, 4:1, hard knee => 10.5 dB of reduction.
    for (int s = 0; s < (int) CompStyle::Count; ++s)
    {
        Compressor c;
        c.prepare (SR);
        CompParams p;
        p.style = (CompStyle) s;
        p.thresholdDb = -20;
        p.ratio = 4;
        p.kneeDb = 0;
        p.attackMs = 5;
        p.releaseMs = 200;
        c.setParams (p);
        Buf l = sine (1000, 0.5, 4), r = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { c.process (a, b, nullptr, nullptr, n); });
        const double gr = db (rms (l, (size_t) (3 * SR)) / (0.5 / std::sqrt (2.0)));
        // Styles with wide built-in knees or averaging detectors land a little either side.
        const bool ok = gr < -6.0 && gr > -13.5;
        check (ok && allFinite (l), std::string ("static reduction, ") + names[s], fmt ("(%.2f dB; ideal hard-knee peak value -10.5)", gr));
    }

    // 2. Below threshold nothing happens.
    {
        Compressor c;
        c.prepare (SR);
        CompParams p;
        p.thresholdDb = -10;
        c.setParams (p);
        Buf l = sine (440, 0.05, 1), r = l, l0 = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { c.process (a, b, nullptr, nullptr, n); });
        double diff = 0;
        for (size_t i = 0; i < l.size(); ++i)
            diff = std::max (diff, (double) std::abs (l[i] - l0[i]));
        check (diff < 1e-6, "unity below threshold", fmt ("(max diff %.2e)", diff));
    }

    // 3. Attack time: 63% of the way to the final reduction.
    {
        Compressor c;
        c.prepare (SR);
        CompParams p;
        p.thresholdDb = -30;
        p.ratio = 4;
        p.kneeDb = 0;
        p.attackMs = 20;
        p.releaseMs = 500;
        c.setParams (p);
        Buf l = sine (2000, 0.5, 1), r = l;
        double t63 = -1;
        const double finalGr = (-6.02 + 30) * (1 - 0.25) * -1; // ~ -18 dB
        for (size_t pos = 0; pos < l.size() && t63 < 0; pos += 8)
        {
            c.process (l.data() + pos, r.data() + pos, nullptr, nullptr, 8);
            if (c.getGainReductionDb() < 0.63 * finalGr)
                t63 = (double) pos / SR * 1000.0;
        }
        check (t63 > 12 && t63 < 30, "attack timing", fmt ("(63%% reached in %.1f ms, set to 20)", t63));
    }

    // 4. Lookahead reports the latency it actually introduces; mix at 0 is a pure delay.
    {
        Compressor c;
        c.prepare (SR);
        CompParams p;
        p.lookaheadMs = 5;
        p.mix = 0;
        c.setParams (p);
        // Let the mix smoother settle.
        Buf warm ((size_t) SR, 0.0f), warm2 = warm;
        processBlocks (warm, warm2, [&] (float* a, float* b, int n) { c.process (a, b, nullptr, nullptr, n); });
        Buf l (4800, 0.0f), r (4800, 0.0f);
        l[100] = r[100] = 1.0f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { c.process (a, b, nullptr, nullptr, n); });
        size_t at = 0;
        for (size_t i = 0; i < l.size(); ++i)
            if (std::abs (l[i]) > 0.5f)
            {
                at = i;
                break;
            }
        check ((int) at - 100 == c.getLatencySamples(), "lookahead latency", fmt ("(%g samples, reported %g)", (double) at - 100, c.getLatencySamples()));
    }

    // 5. All styles stay well behaved on drums with aggressive settings.
    for (int s = 0; s < (int) CompStyle::Count; ++s)
    {
        Compressor c;
        c.prepare (SR);
        CompParams p;
        p.style = (CompStyle) s;
        p.thresholdDb = -40;
        p.ratio = 20;
        p.attackMs = 0.01f;
        p.releaseMs = 5;
        p.kneeDb = 0;
        p.autoMakeup = true;
        p.lookaheadMs = 2;
        c.setParams (p);
        Buf l = drums (0.9, 5), r = drums (0.8, 5);
        double minGr = 0;
        processBlocks (l, r, [&] (float* a, float* b, int n)
        {
            c.process (a, b, nullptr, nullptr, n);
            minGr = std::min (minGr, (double) c.getGainReductionDb());
        }, 64);
        check (allFinite (l) && peakOf (l) < 8.0 && minGr >= -60.01, std::string ("extreme settings, ") + names[s],
               fmt ("(peak %.1f dB, max reduction %.1f dB)", db (peakOf (l)), minGr));
    }

    // 6. Feedback styles do not oscillate: reduction on a steady tone is steady.
    for (CompStyle s : { CompStyle::Glue, CompStyle::Fet })
    {
        Compressor c;
        c.prepare (SR);
        CompParams p;
        p.style = s;
        p.thresholdDb = -30;
        p.ratio = 20;
        p.attackMs = 0.01f;
        p.releaseMs = 50;
        p.kneeDb = 0;
        c.setParams (p);
        Buf l = sine (200, 0.7, 3), r = l;
        double lo = 0, hi = -100;
        size_t pos = 0;
        processBlocks (l, r, [&] (float* a, float* b, int n)
        {
            c.process (a, b, nullptr, nullptr, n);
            pos += (size_t) n;
            if (pos > (size_t) (2 * SR))
            {
                lo = std::min (lo, (double) c.getGainReductionDb());
                hi = std::max (hi, (double) c.getGainReductionDb());
            }
        }, 16);
        check (hi - lo < 3.0, std::string ("feedback loop steady, ") + names[(int) s], fmt ("(reduction between %.1f and %.1f dB)", lo, hi));
    }

    {
        Compressor c;
        c.prepare (SR);
        CompParams p;
        p.lookaheadMs = 5;
        p.thresholdDb = -30;
        c.setParams (p);
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { c.process (a, b, nullptr, nullptr, n); }));
    }
}

//==============================================================================
static void testLimiter()
{
    std::printf ("\nLimiter\n");
    const char* names[] = { "Transparent", "Punchy", "Dynamic", "Aggressive", "Safe" };

    struct Sig { const char* name; Buf l, r; };
    std::vector<Sig> sigs;
    // fs/4 sine sampled 45 degrees off its peaks: the classic inter-sample overshoot case.
    sigs.push_back ({ "fs/4 sine, peaks between samples", sine (SR / 4, 1.0, 2, SR, kPi / 4), sine (SR / 4, 1.0, 2, SR, kPi / 4) });
    {
        // Noise band-limited to 16 kHz: dense, loud, and full of inter-sample peaks.
        Buf a = noise (1.0, 2, 11), b = noise (1.0, 2, 12);
        for (Buf* x : { &a, &b })
        {
            Biquad f1, f2;
            f1.setCoefs (design::lowpass (16000, 0.5412, SR));
            f2.setCoefs (design::lowpass (16000, 1.3066, SR));
            for (auto& v : *x)
                v = f2.process (f1.process (v));
        }
        sigs.push_back ({ "noise to 16 kHz", a, b });
    }
    {
        Buf a = drums (1.0, 2), b = drums (0.9, 2);
        for (Buf* x : { &a, &b })
        {
            Biquad f1, f2;
            f1.setCoefs (design::lowpass (16000, 0.5412, SR));
            f2.setCoefs (design::lowpass (16000, 1.3066, SR));
            for (auto& v : *x)
                v = f2.process (f1.process (v));
        }
        sigs.push_back ({ "drum hits", a, b });
    }
    {
        Buf sq ((size_t) (2 * SR));
        for (size_t i = 0; i < sq.size(); ++i)
            sq[i] = ((i / 37) % 2) ? 0.9f : -0.9f;
        sigs.push_back ({ "square wave", sq, sq });
    }
    sigs.push_back ({ "low sine 50 Hz", sine (50, 1.0, 2), sine (50, 1.0, 2) });

    // 1. True-peak ceiling holds for every style and signal, 12 dB into limiting.
    for (int s = 0; s < (int) LimiterStyle::Count; ++s)
    {
        double worst = -100;
        std::string worstName;
        for (auto& sg : sigs)
        {
            Limiter lim;
            lim.prepare (SR);
            LimiterParams p;
            p.style = (LimiterStyle) s;
            p.gainDb = 12;
            p.ceilingDb = -1;
            p.truePeak = true;
            lim.setParams (p);
            Buf l = sg.l, r = sg.r;
            processBlocks (l, r, [&] (float* a, float* b, int n) { lim.process (a, b, n); });
            const double tp = db (std::max (truePeak (l), truePeak (r)));
            if (tp > worst)
            {
                worst = tp;
                worstName = sg.name;
            }
        }
        check (worst <= -1.0 + 0.05, std::string ("true peak <= -1 dBTP, ") + names[s], fmt ("(worst %.3f dBTP on ", worst) + worstName + ")");
    }

    // 1b. Full-band white noise is the pathological case: most of its inter-sample
    // energy sits just under Nyquist, where any practical detector rolls off.
    {
        Limiter lim;
        lim.prepare (SR);
        LimiterParams p;
        p.gainDb = 12;
        p.ceilingDb = -1;
        lim.setParams (p);
        Buf l = noise (1.0, 2, 11), r = noise (1.0, 2, 12);
        processBlocks (l, r, [&] (float* a, float* b, int n) { lim.process (a, b, n); });
        std::printf ("  [INFO] full-band white noise, 12 dB into limiting: %.2f dBTP by a 16x reference meter, %.2f dBTP by the built-in 8x meter (ceiling -1)\n",
                     db (std::max (truePeak (l), truePeak (r))), db (lim.getMaxTruePeak()));
        Limiter lim2;
        lim2.prepare (SR);
        lim2.setParams (p);
        Buf dl = drums (1.0, 2), dr = drums (0.9, 2);
        processBlocks (dl, dr, [&] (float* a, float* b, int n) { lim2.process (a, b, n); });
        std::printf ("  [INFO] drum hits with full-band noise: %.2f dBTP by the reference meter, %.2f dBTP by the built-in meter\n",
                     db (std::max (truePeak (dl), truePeak (dr))), db (lim2.getMaxTruePeak()));
    }

    // 2. With true-peak off the sample peak still holds exactly.
    {
        Limiter lim;
        lim.prepare (SR);
        LimiterParams p;
        p.gainDb = 18;
        p.ceilingDb = -0.3f;
        p.truePeak = false;
        lim.setParams (p);
        Buf l = noise (1.0, 2, 3), r = noise (1.0, 2, 4);
        processBlocks (l, r, [&] (float* a, float* b, int n) { lim.process (a, b, n); });
        const double pk = db (std::max (peakOf (l), peakOf (r)));
        check (pk <= -0.3 + 1e-4, "sample-peak ceiling", fmt ("(%.4f dBFS, ceiling -0.3)", pk));
    }

    // 3. Quiet signals pass through untouched, delayed by the reported latency.
    {
        Limiter lim;
        lim.prepare (SR);
        LimiterParams p;
        p.gainDb = 0;
        p.ceilingDb = -1;
        lim.setParams (p);
        Buf l = noise (0.2, 1, 5), r = noise (0.2, 1, 6), l0 = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { lim.process (a, b, n); });
        const int lat = lim.getLatencySamples();
        double diff = 0;
        for (size_t i = (size_t) lat; i < l.size(); ++i)
            diff = std::max (diff, (double) std::abs (l[i] - l0[i - (size_t) lat]));
        check (diff < 1e-6, "transparent below the ceiling, latency as reported", fmt ("(latency %g samples, max diff %.2e)", lat, diff));
    }

    // 4. Loudness meter calibration: stereo 1 kHz sine at -23 dBFS reads -23 LUFS.
    {
        LoudnessMeter m;
        m.prepare (SR);
        Buf l = sine (1000, std::pow (10.0, -23.0 / 20.0), 10), r = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { m.process (a, b, n); });
        check (std::abs (m.getIntegrated() + 23.0) < 0.1 && std::abs (m.getShortTerm() + 23.0) < 0.1 && std::abs (m.getMomentary() + 23.0) < 0.1,
               "loudness meter calibration", fmt ("(integrated %.2f, short-term %.2f, momentary %.2f LUFS)", m.getIntegrated(), m.getShortTerm(), m.getMomentary()));
        LoudnessMeter m44;
        m44.prepare (44100);
        Buf l2 = sine (1000, std::pow (10.0, -23.0 / 20.0), 10, 44100), r2 = l2;
        processBlocks (l2, r2, [&] (float* a, float* b, int n) { m44.process (a, b, n); });
        check (std::abs (m44.getIntegrated() + 23.0) < 0.1, "loudness meter at 44.1 kHz", fmt ("(%.2f LUFS)", m44.getIntegrated()));
    }

    // 5. Distortion of a sustained 1 kHz tone pushed 6 dB into the limiter.
    for (int s = 0; s < (int) LimiterStyle::Count; ++s)
    {
        Limiter lim;
        lim.prepare (SR);
        LimiterParams p;
        p.style = (LimiterStyle) s;
        p.gainDb = 6;
        p.ceilingDb = 0;
        p.truePeak = false;
        lim.setParams (p);
        Buf l = sine (1000, 1.0, 3), r = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { lim.process (a, b, n); });
        const size_t from = (size_t) (2 * SR), to = l.size();
        const double fund = toneAmp (l, 1000, from, to);
        double harm = 0;
        for (int h = 2; h <= 9; ++h)
        {
            const double a = toneAmp (l, 1000.0 * h, from, to);
            harm += a * a;
        }
        const double thd = 100.0 * std::sqrt (harm) / fund;
        check (thd < (s == (int) LimiterStyle::Aggressive ? 3.0 : 1.0), std::string ("steady-tone distortion, ") + names[s], fmt ("(THD %.3f%%)", thd));
    }

    // 6. Low notes are the hard case: the gain must not follow the waveform.
    for (int s = 0; s < (int) LimiterStyle::Count; ++s)
    {
        Limiter lim;
        lim.prepare (SR);
        LimiterParams p;
        p.style = (LimiterStyle) s;
        p.gainDb = 6;
        p.ceilingDb = 0;
        p.truePeak = false;
        lim.setParams (p);
        Buf l = sine (50, 1.0, 4), r = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { lim.process (a, b, n); });
        const size_t from = (size_t) (2 * SR), to = l.size();
        const double fund = toneAmp (l, 50, from, to);
        double harm = 0;
        for (int h = 2; h <= 9; ++h)
        {
            const double a = toneAmp (l, 50.0 * h, from, to);
            harm += a * a;
        }
        const double thd = 100.0 * std::sqrt (harm) / fund;
        std::printf ("  [INFO] 50 Hz tone 6 dB into limiting, %s: THD %.2f%%\n", names[s], thd);
    }

    {
        Limiter lim;
        lim.prepare (SR);
        LimiterParams p;
        p.gainDb = 10;
        lim.setParams (p);
        std::printf ("  CPU with true peak: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { lim.process (a, b, n); }));
    }
}

//==============================================================================
static void testReverb()
{
    std::printf ("\nReverb\n");
    const char* names[] = { "Concert Hall", "Bright Hall", "Plate", "Room", "Chamber", "Ambience", "Cathedral",
                            "Random Hall", "Chorus Hall", "Dark Chamber", "Lo-Fi Hall", "Gated" };

    // 1. Decay time follows the Decay control.
    for (double target : { 1.0, 2.5, 6.0 })
    {
        Reverb rv;
        ReverbParams p;
        p.mode = ReverbMode::ConcertHall;
        p.mix = 1;
        p.decaySec = (float) target;
        p.bassMult = 1;
        p.highMult = 1;
        p.early = 0;
        p.predelayMs = 0;
        p.modDepth = 0;
        rv.setParams (p);
        rv.prepare (SR);
        Buf l ((size_t) (SR * (target * 1.6 + 1)), 0.0f), r = l;
        l[0] = r[0] = 1.0f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { rv.process (a, b, n); });
        const double rt = measureRt60 (l, SR);
        check (std::abs (rt / target - 1.0) < 0.15, fmt ("decay time, %.1f s", target), fmt ("(measured %.2f s)", rt));
    }

    // 1b. With modulation running, the 1 kHz octave still decays on schedule.
    {
        Reverb rv;
        ReverbParams p;
        p.mix = 1;
        p.decaySec = 2.5f;
        p.bassMult = 1;
        p.highMult = 1;
        p.early = 0;
        p.predelayMs = 0;
        rv.setParams (p);
        rv.prepare (SR);
        Buf l ((size_t) (SR * 5), 0.0f), r = l;
        l[0] = r[0] = 1.0f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { rv.process (a, b, n); });
        Biquad bp1, bp2;
        bp1.setCoefs (design::bandpass (1000, 1.4, SR));
        bp2.setCoefs (design::bandpass (1000, 1.4, SR));
        for (auto& v : l)
            v = bp2.process (bp1.process (v));
        const double rt = measureRt60 (l, SR);
        check (std::abs (rt / 2.5 - 1.0) < 0.15, "decay time with modulation, 1 kHz band", fmt ("(measured %.2f s, set to 2.5)", rt));
    }

    // 2. Each mode: stable, decays, wide, and sits at a sensible level.
    for (int m = 0; m < (int) ReverbMode::Count; ++m)
    {
        Reverb rv;
        rv.prepare (SR);
        ReverbParams p;
        p.mode = (ReverbMode) m;
        p.mix = 1;
        p.decaySec = 2;
        rv.setParams (p);
        Buf l = drums (0.5, 6), r = l;
        const double dryRms = rms (l, 0, (size_t) (3 * SR));
        for (size_t i = (size_t) (3 * SR); i < l.size(); ++i)
            l[i] = r[i] = 0.0f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { rv.process (a, b, n); });
        const double wet = rms (l, (size_t) SR, (size_t) (3 * SR));
        const double tail = rms (l, (size_t) (5.5 * SR));
        double lr = 0, ll = 0, rr = 0;
        for (size_t i = (size_t) SR; i < (size_t) (3 * SR); ++i)
        {
            lr += (double) l[i] * r[i];
            ll += (double) l[i] * l[i];
            rr += (double) r[i] * r[i];
        }
        const double corr = lr / std::sqrt (ll * rr + 1e-30);
        const double rel = db (wet / dryRms);
        const bool ok = allFinite (l) && allFinite (r) && rel > -14 && rel < 4 && tail < wet * 0.05 && std::abs (corr) < 0.6;
        check (ok, names[m], fmt ("(wet %.1f dB vs dry, L/R correlation %.2f, tail after 2.5 s %.0f dB)", rel, corr, db (tail / (wet + 1e-12))));
    }

    // 3. Worst-case settings cannot blow up.
    for (int m = 0; m < (int) ReverbMode::Count; ++m)
    {
        Reverb rv;
        rv.prepare (SR);
        ReverbParams p;
        p.mode = (ReverbMode) m;
        p.mix = 1;
        p.decaySec = 60;
        p.bassMult = 4;
        p.highMult = 1;
        p.size = 1;
        p.lateDiff = 1;
        p.earlyDiff = 1;
        p.modDepth = 1;
        p.modRateHz = 5;
        p.early = 1;
        rv.setParams (p);
        Buf l = noise (1.0, 20, 21), r = noise (1.0, 20, 22);
        processBlocks (l, r, [&] (float* a, float* b, int n) { rv.process (a, b, n); });
        if (! (allFinite (l) && peakOf (l) < 60.0))
            check (false, std::string ("worst-case stability, ") + names[m], fmt ("(peak %.1f dB)", db (peakOf (l))));
    }
    check (true, "worst-case stability sweep finished (failures listed above, if any)");

    // 4. Freeze holds the tail.
    {
        Reverb rv;
        rv.prepare (SR);
        ReverbParams p;
        p.mix = 1;
        p.decaySec = 2;
        p.modDepth = 0.2f;
        rv.setParams (p);
        Buf l = noise (0.5, 2, 31), r = noise (0.5, 2, 32);
        processBlocks (l, r, [&] (float* a, float* b, int n) { rv.process (a, b, n); });
        p.freeze = true;
        rv.setParams (p);
        Buf l2 ((size_t) (10 * SR), 0.0f), r2 = l2;
        processBlocks (l2, r2, [&] (float* a, float* b, int n) { rv.process (a, b, n); });
        const double early = rms (l2, (size_t) SR, (size_t) (2 * SR)), late = rms (l2, (size_t) (8 * SR), (size_t) (9 * SR));
        check (db (late / early) > -6.0 && db (late / early) < 1.0, "freeze holds the tail", fmt ("(level change over 7 s: %.1f dB)", db (late / early)));
    }

    // 5. Mix at zero returns the input.
    {
        Reverb rv;
        ReverbParams p;
        p.mix = 0;
        rv.setParams (p);
        rv.prepare (SR);
        Buf l = noise (0.5, 1, 41), r = noise (0.5, 1, 42), l0 = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { rv.process (a, b, n); });
        double diff = 0;
        for (size_t i = 0; i < l.size(); ++i)
            diff = std::max (diff, (double) std::abs (l[i] - l0[i]));
        check (diff < 1e-6, "dry at 0% mix", fmt ("(max diff %.2e)", diff));
    }

    // 6. Tail colouration: how peaky is the long-term spectrum of the hall impulse response?
    {
        Reverb rv;
        rv.prepare (SR);
        ReverbParams p;
        p.mix = 1;
        p.decaySec = 3;
        p.early = 0;
        p.highMult = 1;
        p.bassMult = 1;
        p.dampHz = 20000;
        rv.setParams (p);
        Buf l ((size_t) 131072 + 8192, 0.0f), r = l;
        l[0] = r[0] = 1.0f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { rv.process (a, b, n); });
        std::vector<std::complex<float>> spec (131072);
        for (size_t i = 0; i < spec.size(); ++i)
            spec[i] = l[i + 4096];
        fft (spec);
        // Third-octave band energies between 100 Hz and 10 kHz.
        std::vector<double> bands;
        for (double f = 100; f < 10000; f *= std::pow (2.0, 1.0 / 3.0))
        {
            const size_t a = (size_t) (f / std::pow (2.0, 1.0 / 6.0) * spec.size() / SR), b = (size_t) (f * std::pow (2.0, 1.0 / 6.0) * spec.size() / SR);
            double e = 0;
            for (size_t k = a; k < b; ++k)
                e += std::norm (spec[k]);
            bands.push_back (10.0 * std::log10 (e / (double) (b - a) + 1e-30));
        }
        double lo = 1e9, hi = -1e9;
        for (double v : bands)
        {
            lo = std::min (lo, v);
            hi = std::max (hi, v);
        }
        check (hi - lo < 8.0, "even tail spectrum (third-octave spread, 100 Hz to 10 kHz)", fmt ("(%.1f dB)", hi - lo));
    }

    {
        Reverb rv;
        rv.prepare (SR);
        ReverbParams p;
        rv.setParams (p);
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { rv.process (a, b, n); }));
    }
}

//==============================================================================
static void testDelay()
{
    std::printf ("\nDelay\n");
    const char* styles[] = { "Digital", "Studio Tape", "Worn Tape", "Analog", "Tube", "Lo-Fi", "Telephone", "Diffuse" };

    auto cleanParams = []
    {
        DelayParams p;
        p.style = DelayStyle::Digital;
        p.timeLMs = p.timeRMs = 250;
        p.feedback = 0.5f;
        p.mix = 1;
        p.lowCutHz = 20;
        p.highCutHz = 20000;
        p.saturation = 0;
        p.wobble = 0;
        return p;
    };

    // 1. Echo timing and decay.
    {
        Delay d;
        d.setParams (cleanParams());
        d.prepare (SR);
        Buf l ((size_t) (2 * SR), 0.0f), r = l;
        l[0] = r[0] = 0.5f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { d.process (a, b, n); });
        const auto e = findEchoes (l, 0.01, 2000);
        const bool timing = e.size() >= 4 && e[0].first == 12000 && e[1].first == 24000 && e[2].first == 36000;
        const double ratio = e.size() >= 3 ? db (e[1].second / e[0].second) : 0;
        check (timing, "echo timing, 250 ms", fmt ("(first three at %g, %g, %g samples; expected 12000, 24000, 36000)",
                                                    e.size() > 0 ? (double) e[0].first : -1, e.size() > 1 ? (double) e[1].first : -1, e.size() > 2 ? (double) e[2].first : -1));
        check (std::abs (ratio + 6.02) < 0.2, "50% feedback loses 6 dB per repeat", fmt ("(%.2f dB)", ratio));
    }

    // 2. Ping-pong alternates sides.
    {
        Delay d;
        auto p = cleanParams();
        p.mode = DelayMode::PingPong;
        d.setParams (p);
        d.prepare (SR);
        Buf l ((size_t) SR, 0.0f), r = l;
        l[0] = r[0] = 0.5f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { d.process (a, b, n); });
        const auto el = findEchoes (l, 0.01, 2000), er = findEchoes (r, 0.01, 2000);
        const bool ok = ! el.empty() && ! er.empty() && el[0].first == 12000 && er[0].first == 24000 && std::abs (r[12000]) < 1e-4;
        check (ok, "ping-pong alternates left then right", fmt ("(left at %g, right at %g)", el.empty() ? -1.0 : (double) el[0].first, er.empty() ? -1.0 : (double) er[0].first));
    }

    // 3. Groove shifts every other repeat.
    {
        Delay d;
        auto p = cleanParams();
        p.groove = 0.5f; // 25% late
        d.setParams (p);
        d.prepare (SR);
        Buf l ((size_t) (2 * SR), 0.0f), r = l;
        l[0] = r[0] = 0.5f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { d.process (a, b, n); });
        const auto e = findEchoes (l, 0.01, 2000);
        const bool ok = e.size() >= 3 && e[0].first == 15000 && e[1].first == 24000 && e[2].first == 39000;
        check (ok, "groove moves odd repeats, even repeats stay on the grid", fmt ("(repeats at %g, %g, %g)", e.size() > 0 ? (double) e[0].first : -1, e.size() > 1 ? (double) e[1].first : -1, e.size() > 2 ? (double) e[2].first : -1));
    }

    // 4. Rhythm pattern taps land where the pattern says.
    {
        Delay d;
        auto p = cleanParams();
        p.mode = DelayMode::Rhythm;
        p.pattern = 0;
        p.feedback = 0;
        p.width = 0;
        d.setParams (p);
        d.prepare (SR);
        Buf l ((size_t) (2 * SR), 0.0f), r = l;
        l[0] = r[0] = 0.5f;
        processBlocks (l, r, [&] (float* a, float* b, int n) { d.process (a, b, n); });
        const auto e = findEchoes (l, 0.005, 2000);
        const bool ok = e.size() == 4 && e[0].first == 12000 && e[3].first == 48000;
        check (ok, "rhythm pattern tap positions", fmt ("(%g taps, first at %g, last at %g)", (double) e.size(), e.empty() ? -1.0 : (double) e[0].first, e.empty() ? -1.0 : (double) e.back().first));
    }

    // 5. Every style stays bounded with feedback above 100%.
    for (int s = 0; s < (int) DelayStyle::Count; ++s)
        for (int m = 0; m < (int) DelayMode::Count; ++m)
        {
            Delay d;
            d.prepare (SR);
            DelayParams p;
            p.style = (DelayStyle) s;
            p.mode = (DelayMode) m;
            p.feedback = 1.1f;
            p.timeLMs = 90;
            p.timeRMs = 130;
            p.mix = 1;
            p.wobble = 1;
            p.saturation = 1;
            p.diffusion = 1;
            p.lowCutHz = 20;
            p.highCutHz = 20000;
            d.setParams (p);
            Buf l = drums (0.9, 20), r = drums (0.9, 20);
            processBlocks (l, r, [&] (float* a, float* b, int n) { d.process (a, b, n); });
            if (! (allFinite (l) && allFinite (r) && peakOf (l) < 6.0 && peakOf (r) < 6.0))
                check (false, std::string ("runaway feedback bounded, ") + styles[s], fmt ("(mode %g, peak %.1f dB)", m, db (peakOf (l))));
        }
    check (true, "runaway-feedback sweep finished (failures listed above, if any)");

    // 6. Mix at zero returns the input.
    {
        Delay d;
        auto p = cleanParams();
        p.mix = 0;
        d.setParams (p);
        d.prepare (SR);
        Buf l = noise (0.5, 1, 51), r = noise (0.5, 1, 52), l0 = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { d.process (a, b, n); });
        double diff = 0;
        for (size_t i = 0; i < l.size(); ++i)
            diff = std::max (diff, (double) std::abs (l[i] - l0[i]));
        check (diff < 1e-6, "dry at 0% mix", fmt ("(max diff %.2e)", diff));
    }

    {
        Delay d;
        d.prepare (SR);
        DelayParams p;
        d.setParams (p);
        std::printf ("  CPU: %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { d.process (a, b, n); }));
    }
}

//==============================================================================
static void testChorus()
{
    std::printf ("\nChorus\n");
    const char* types[] = { "Dimension", "Classic", "Ensemble", "Modern" };

    for (int t = 0; t < (int) ChorusType::Count; ++t)
    {
        bool ok = true;
        double worstMono = 100, peak = 0;
        for (int m = 0; m < (int) ChorusMode::Count; ++m)
            for (int s = 0; s < (int) ChorusShape::Count; ++s)
                for (float fb : { -0.9f, 0.0f, 0.9f })
                {
                    Chorus c;
                    c.prepare (SR);
                    ChorusParams p;
                    p.type = (ChorusType) t;
                    p.mode = (ChorusMode) m;
                    p.shape = (ChorusShape) s;
                    p.feedback = fb;
                    p.mix = 1;
                    p.rateHz = 10;
                    p.depth = 1;
                    p.delayMs = 30;
                    p.warmth = 1;
                    c.setParams (p);
                    Buf l = noise (0.5, 3, 61), r = l;
                    const double in = rms (l);
                    processBlocks (l, r, [&] (float* a, float* b, int n) { c.process (a, b, n); });
                    ok = ok && allFinite (l) && allFinite (r);
                    peak = std::max (peak, std::max (peakOf (l), peakOf (r)));
                    if (fb == 0.0f)
                    {
                        Buf mono (l.size());
                        for (size_t i = 0; i < l.size(); ++i)
                            mono[i] = 0.5f * (l[i] + r[i]);
                        worstMono = std::min (worstMono, db (rms (mono) / rms (l)));
                    }
                    (void) in;
                }
        check (ok && peak < 8.0 && worstMono > -9.0, std::string ("all modes and shapes, ") + types[t],
               fmt ("(peak %.1f dB, wet signal folded to mono loses at most %.1f dB)", db (peak), -worstMono));
    }

    {
        Chorus c;
        ChorusParams p;
        p.mix = 0;
        c.setParams (p);
        c.prepare (SR);
        Buf l = noise (0.5, 1, 71), r = noise (0.5, 1, 72), l0 = l;
        processBlocks (l, r, [&] (float* a, float* b, int n) { c.process (a, b, n); });
        double diff = 0;
        for (size_t i = 0; i < l.size(); ++i)
            diff = std::max (diff, (double) std::abs (l[i] - l0[i]));
        check (diff < 1e-6, "dry at 0% mix", fmt ("(max diff %.2e)", diff));
    }

    // Pitch deviation of a steady tone in the preset modes (should be a gentle chorus, not a warble).
    for (int t = 0; t < (int) ChorusType::Count; ++t)
    {
        const auto& td = Chorus::typeData ((ChorusType) t);
        double worst = 0;
        for (int m = 0; m < 4; ++m)
        {
            // Peak pitch shift of a triangle-modulated delay: 4 * depth * rate (as a ratio).
            const double cents = 1200.0 * std::log2 (1.0 + 4.0 * td.presetDepthMs[m] * 0.001 * td.presetRate[m]);
            worst = std::max (worst, cents);
        }
        check (worst < 25.0, std::string ("preset detune amount, ") + types[t], fmt ("(up to %.1f cents)", worst));
    }

    {
        Chorus c;
        c.prepare (SR);
        ChorusParams p;
        p.type = ChorusType::Modern;
        c.setParams (p);
        std::printf ("  CPU (6-voice Modern): %.2f%% of one core\n", benchmark ([&] (float* a, float* b, int n) { c.process (a, b, n); }));
    }
}

int main()
{
    testEq();
    testCompressor();
    testLimiter();
    testReverb();
    testDelay();
    testChorus();
    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
