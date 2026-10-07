// Gitto FX Channel Strip - plugin wrapper and interface.
#include "../../dsp/ChannelStrip.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class StripProcessor : public GittoProcessor
{
public:
    StripProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        layout.add (floatParam ("in", "Input", range (-20.0f, 20.0f), 0.0f, Unit::Db));
        layout.add (floatParam ("drive", "Drive", range (0.0f, 1.0f), 0.0f, Unit::Percent));
        layout.add (boolParam ("phase", "Polarity Invert", false));
        layout.add (floatParam ("hpf", "High-Pass", range (16.0f, 350.0f, 80.0f), 16.0f, Unit::Hz));
        layout.add (floatParam ("lpf", "Low-Pass", range (3000.0f, 22000.0f, 10000.0f), 22000.0f, Unit::Hz));
        layout.add (choiceParam ("eqtype", "EQ Character", { "Smooth", "Tight" }, 0));
        layout.add (floatParam ("lf_freq", "LF Frequency", range (30.0f, 450.0f, 120.0f), 80.0f, Unit::Hz));
        layout.add (floatParam ("lf_gain", "LF Gain", range (-15.0f, 15.0f), 0.0f, Unit::Db));
        layout.add (boolParam ("lf_bell", "LF Bell", false));
        layout.add (floatParam ("lmf_freq", "LMF Frequency", range (200.0f, 2500.0f, 700.0f), 500.0f, Unit::Hz));
        layout.add (floatParam ("lmf_gain", "LMF Gain", range (-15.0f, 15.0f), 0.0f, Unit::Db));
        layout.add (floatParam ("lmf_q", "LMF Q", range (0.5f, 3.0f, 1.2f), 1.0f, Unit::None));
        layout.add (floatParam ("hmf_freq", "HMF Frequency", range (600.0f, 7000.0f, 2500.0f), 3000.0f, Unit::Hz));
        layout.add (floatParam ("hmf_gain", "HMF Gain", range (-15.0f, 15.0f), 0.0f, Unit::Db));
        layout.add (floatParam ("hmf_q", "HMF Q", range (0.5f, 3.0f, 1.2f), 1.0f, Unit::None));
        layout.add (floatParam ("hf_freq", "HF Frequency", range (1500.0f, 16000.0f, 6000.0f), 8000.0f, Unit::Hz));
        layout.add (floatParam ("hf_gain", "HF Gain", range (-15.0f, 15.0f), 0.0f, Unit::Db));
        layout.add (boolParam ("hf_bell", "HF Bell", false));
        layout.add (floatParam ("c_thr", "Compressor Threshold", range (-50.0f, 0.0f), 0.0f, Unit::Db));
        layout.add (floatParam ("c_ratio", "Compressor Ratio", range (1.0f, 20.0f, 4.0f), 3.0f, Unit::Ratio));
        layout.add (floatParam ("c_rel", "Compressor Release", range (100.0f, 4000.0f, 600.0f), 300.0f, Unit::Ms));
        layout.add (boolParam ("c_fast", "Compressor Fast Attack", false));
        layout.add (floatParam ("c_makeup", "Compressor Makeup", range (0.0f, 20.0f), 0.0f, Unit::Db));
        layout.add (floatParam ("g_thr", "Gate Threshold", range (-70.0f, 0.0f), -70.0f, Unit::Db));
        layout.add (floatParam ("g_range", "Gate Range", range (0.0f, 40.0f), 20.0f, Unit::DbAmount));
        layout.add (floatParam ("g_rel", "Gate Release", range (100.0f, 4000.0f, 600.0f), 300.0f, Unit::Ms));
        layout.add (choiceParam ("g_mode", "Gate Mode", { "Gate", "Expander" }, 0));
        layout.add (choiceParam ("order", "Order", { "EQ > Dynamics", "Dynamics > EQ" }, 0));
        layout.add (floatParam ("out", "Output", range (-20.0f, 20.0f), 0.0f, Unit::Db));
        return layout;
    }

    juce::String getProductName() const override { return "Channel Strip"; }
    juce::Colour getAccent() const override { return juce::Colour (0xff7fb2ff); }

    gitto::ChannelStripParams readParams() const
    {
        gitto::ChannelStripParams p;
        p.inputDb = value ("in");
        p.drive = value ("drive");
        p.phaseInvert = flag ("phase");
        p.hpfHz = value ("hpf");
        p.lpfHz = value ("lpf");
        p.eqType = (gitto::StripEqType) choice ("eqtype");
        p.lfHz = value ("lf_freq");
        p.lfGainDb = value ("lf_gain");
        p.lfBell = flag ("lf_bell");
        p.lmfHz = value ("lmf_freq");
        p.lmfGainDb = value ("lmf_gain");
        p.lmfQ = value ("lmf_q");
        p.hmfHz = value ("hmf_freq");
        p.hmfGainDb = value ("hmf_gain");
        p.hmfQ = value ("hmf_q");
        p.hfHz = value ("hf_freq");
        p.hfGainDb = value ("hf_gain");
        p.hfBell = flag ("hf_bell");
        p.compThresholdDb = value ("c_thr");
        p.compRatio = value ("c_ratio");
        p.compReleaseMs = value ("c_rel");
        p.compFastAttack = flag ("c_fast");
        p.compMakeupDb = value ("c_makeup");
        p.gateThresholdDb = value ("g_thr");
        p.gateRangeDb = value ("g_range");
        p.gateReleaseMs = value ("g_rel");
        p.gateExpander = choice ("g_mode") == 1;
        p.order = (gitto::StripOrder) choice ("order");
        p.outputDb = value ("out");
        return p;
    }

    void prepareToPlay (double sr, int) override
    {
        sampleRate = sr;
        dsp.setParams (readParams());
        dsp.prepare (sr);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        if (consumeDirty())
            dsp.setParams (readParams());
        dsp.process (buffer.getWritePointer (0), buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr, buffer.getNumSamples());
    }

    std::vector<Preset> getPresets() const override
    {
        return {
            { "Default", {} },
            { "Lead Vocal", { { "hpf", 90 }, { "lmf_freq", 300 }, { "lmf_gain", -3 }, { "hmf_freq", 3500 }, { "hmf_gain", 2.5f }, { "hf_freq", 10000 }, { "hf_gain", 3 },
                              { "c_thr", -22 }, { "c_ratio", 3 }, { "c_rel", 250 }, { "c_makeup", 6 }, { "drive", 0.15f } } },
            { "Rap Vocal", { { "hpf", 100 }, { "lmf_freq", 350 }, { "lmf_gain", -4 }, { "hmf_freq", 4000 }, { "hmf_gain", 3 }, { "hf_freq", 12000 }, { "hf_gain", 2 },
                             { "c_thr", -20 }, { "c_ratio", 4 }, { "c_fast", 1 }, { "c_rel", 150 }, { "c_makeup", 7.5f }, { "eqtype", 1 } } },
            { "Kick", { { "hpf", 30 }, { "lf_freq", 65 }, { "lf_gain", 4 }, { "lf_bell", 1 }, { "lmf_freq", 380 }, { "lmf_gain", -5 }, { "lmf_q", 1.6f }, { "hmf_freq", 4000 }, { "hmf_gain", 4 },
                        { "c_thr", -16 }, { "c_ratio", 4 }, { "c_rel", 150 }, { "c_makeup", 2 }, { "g_thr", -36 }, { "g_range", 20 }, { "g_rel", 200 }, { "eqtype", 1 } } },
            { "Snare", { { "hpf", 90 }, { "lf_freq", 200 }, { "lf_gain", 3 }, { "lf_bell", 1 }, { "lmf_freq", 600 }, { "lmf_gain", -3 }, { "hmf_freq", 5000 }, { "hmf_gain", 4 }, { "hf_gain", 2 },
                         { "c_thr", -18 }, { "c_ratio", 4 }, { "c_makeup", 3 }, { "g_thr", -34 }, { "g_range", 15 }, { "g_rel", 250 } } },
            { "808 Bass", { { "lf_freq", 55 }, { "lf_gain", 3 }, { "lmf_freq", 250 }, { "lmf_gain", -3 }, { "hmf_freq", 1200 }, { "hmf_gain", 3 }, { "lpf", 9000 },
                            { "drive", 0.45f }, { "c_thr", -16 }, { "c_ratio", 5 }, { "c_rel", 300 }, { "c_makeup", 2 } } },
            { "Bright Keys", { { "hpf", 60 }, { "lmf_freq", 400 }, { "lmf_gain", -2 }, { "hf_freq", 9000 }, { "hf_gain", 4 }, { "c_thr", -20 }, { "c_ratio", 2 }, { "c_makeup", 4 } } },
            { "Drum Bus", { { "lf_freq", 70 }, { "lf_gain", 2 }, { "lmf_freq", 400 }, { "lmf_gain", -2 }, { "hf_freq", 10000 }, { "hf_gain", 2.5f }, { "drive", 0.3f },
                            { "c_thr", -14 }, { "c_ratio", 3 }, { "c_rel", 200 }, { "c_makeup", 2 } } },
            { "Telephone", { { "hpf", 350 }, { "lpf", 3000 }, { "hmf_freq", 1800 }, { "hmf_gain", 6 }, { "drive", 0.7f }, { "eqtype", 1 }, { "out", 1.5f } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::ChannelStrip dsp;
    double sampleRate = 48000.0;
};

class StripDisplay : public juce::Component
{
public:
    explicit StripDisplay (StripProcessor& p) : proc (p) {}

    void paint (juce::Graphics& g) override
    {
        const juce::Colour accent = proc.getAccent();
        auto area = getLocalBounds().toFloat().reduced (8.0f);
        auto meters = area.removeFromRight (150.0f);
        area.removeFromRight (10.0f);

        // ---- filter and EQ response ----
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        const auto r = area.reduced (4.0f);
        drawFrequencyGrid (g, r);
        auto toY = [&] (float db) { return r.getCentreY() - juce::jlimit (-19.0f, 19.0f, db) / 18.0f * (r.getHeight() * 0.5f - 6.0f); };
        g.setFont (font (9.5f));
        for (int db = -12; db <= 12; db += 6)
        {
            g.setColour (db == 0 ? colours::panelEdge.brighter (0.25f) : colours::grid);
            g.fillRect (r.getX(), toY ((float) db), r.getWidth(), 1.0f);
            g.setColour (colours::dim.withAlpha (0.7f));
            if (db != 0)
                g.drawText ((db > 0 ? "+" : "") + juce::String (db), juce::Rectangle<float> (r.getX() + 3.0f, toY ((float) db) - 12.0f, 30.0f, 11.0f), juce::Justification::centredLeft);
        }
        const auto p = proc.readParams();
        gitto::BiquadCoefs c[gitto::ChannelStrip::kEqSections];
        bool on[gitto::ChannelStrip::kEqSections];
        gitto::ChannelStrip::design (p, proc.sampleRate, c, on);
        juce::Path curve;
        const int points = juce::jmax (64, (int) r.getWidth() / 2);
        for (int i = 0; i < points; ++i)
        {
            const float x = (float) i / (float) (points - 1) * r.getWidth();
            const double w = gitto::kTwoPi * juce::jmin ((double) xToFreq (x, r.getWidth()), proc.sampleRate * 0.499) / proc.sampleRate;
            double mag = 1.0;
            for (int s = 0; s < gitto::ChannelStrip::kEqSections; ++s)
                if (on[s])
                    mag *= c[s].magnitude (w);
            const float y = toY ((float) (20.0 * std::log10 (juce::jmax (mag, 1.0e-6))));
            if (i == 0) curve.startNewSubPath (r.getX() + x, y);
            else curve.lineTo (r.getX() + x, y);
        }
        juce::Path fill (curve);
        fill.lineTo (r.getRight(), toY (0.0f));
        fill.lineTo (r.getX(), toY (0.0f));
        fill.closeSubPath();
        g.setColour (accent.withAlpha (0.14f));
        g.fillPath (fill);
        g.setColour (accent);
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // ---- dynamics meters ----
        g.setColour (colours::background);
        g.fillRoundedRectangle (meters, 4.0f);
        const float comp = -proc.dsp.getCompReductionDb(), gate = -proc.dsp.getGateReductionDb();
        struct Item { const char* label; float value, range; juce::Colour colour; };
        const Item items[2] = { { "COMP", comp, 20.0f, juce::Colour (0xffff8a4c) }, { "GATE", gate, 40.0f, juce::Colour (0xff6ec6ff) } };
        const float colW = meters.getWidth() / 2.0f;
        for (int i = 0; i < 2; ++i)
        {
            auto col = juce::Rectangle<float> (meters.getX() + colW * (float) i, meters.getY(), colW, meters.getHeight()).reduced (10.0f, 8.0f);
            g.setColour (colours::dim);
            g.setFont (font (9.5f, true));
            g.drawText (items[i].label, col.removeFromTop (12.0f), juce::Justification::centred);
            g.setColour (colours::text);
            g.setFont (font (12.0f, true));
            g.drawText (dbText (-std::abs (items[i].value)).upToFirstOccurrenceOf (" ", false, false), col.removeFromBottom (16.0f), juce::Justification::centred);
            const auto bar = col.reduced (colW * 0.18f, 4.0f);
            g.setColour (colours::track);
            g.fillRoundedRectangle (bar, 3.0f);
            g.setColour (items[i].colour);
            g.fillRoundedRectangle (bar.withHeight (bar.getHeight() * juce::jlimit (0.0f, 1.0f, items[i].value / items[i].range)), 3.0f);
        }
    }

private:
    StripProcessor& proc;
};

juce::AudioProcessorEditor* StripProcessor::createEditor()
{
    LayoutSpec l;
    l.displayHeight = 170;
    l.rows = {
        { { "Input", { knob ("in", "Gain", true), knob ("drive", "Drive"), toggle ("phase", "Invert") } },
          { "Filters", { knob ("hpf", "High-Pass"), knob ("lpf", "Low-Pass") } },
          { "Routing and Output", { chooser ("eqtype", "EQ Character"), chooser ("order", "Order"), knob ("out", "Output", true) } } },
        { { "Low", { knob ("lf_freq", "Freq"), knob ("lf_gain", "Gain", true), toggle ("lf_bell", "Bell") } },
          { "Low Mid", { knob ("lmf_freq", "Freq"), knob ("lmf_gain", "Gain", true), knob ("lmf_q", "Q") } },
          { "High Mid", { knob ("hmf_freq", "Freq"), knob ("hmf_gain", "Gain", true), knob ("hmf_q", "Q") } },
          { "High", { knob ("hf_freq", "Freq"), knob ("hf_gain", "Gain", true), toggle ("hf_bell", "Bell") } } },
        { { "Compressor", { knob ("c_thr", "Threshold"), knob ("c_ratio", "Ratio"), knob ("c_rel", "Release"), toggle ("c_fast", "Fast Atk"), knob ("c_makeup", "Makeup") } },
          { "Gate", { knob ("g_thr", "Threshold"), knob ("g_range", "Range"), knob ("g_rel", "Release"), chooser ("g_mode", "Mode") } } },
    };
    l.meters = {
        { "IN", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (dsp.getInputLevel()); } },
        { "OUT", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (dsp.getOutputLevel()); } },
    };
    return new GittoEditor (*this, std::move (l), std::make_unique<StripDisplay> (*this));
}

juce::AudioProcessor* createGittoChannelStrip() { return new StripProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoChannelStrip(); }
#endif
