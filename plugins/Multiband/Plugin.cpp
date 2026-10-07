// Gitto FX Multiband - plugin wrapper and interface.
#include "../../dsp/Multiband.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
namespace
{
juce::String mbId (int band, const char* name) { return "b" + juce::String (band + 1) + "_" + name; }
}

class MultibandProcessor : public GittoProcessor
{
public:
    MultibandProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        // Gentle on a track peaking around -6 dBFS: about a decibel of reduction until you ask for more.
        const float defThreshold[4] = { -18.0f, -18.0f, -20.0f, -24.0f };
        const float defAttack[4] = { 30.0f, 20.0f, 10.0f, 5.0f };
        const float defRelease[4] = { 200.0f, 150.0f, 100.0f, 80.0f };
        for (int b = 0; b < 4; ++b)
        {
            const juce::String n = "Band " + juce::String (b + 1) + " ";
            layout.add (boolParam (mbId (b, "on"), n + "On", true));
            layout.add (floatParam (mbId (b, "thr"), n + "Threshold", range (-60.0f, 0.0f), defThreshold[b], Unit::Db));
            layout.add (floatParam (mbId (b, "range"), n + "Range", range (-30.0f, 30.0f), -6.0f, Unit::Db));
            layout.add (floatParam (mbId (b, "ratio"), n + "Ratio", range (1.0f, 20.0f, 4.0f), 3.0f, Unit::Ratio));
            layout.add (floatParam (mbId (b, "attack"), n + "Attack", range (0.1f, 250.0f, 15.0f), defAttack[b], Unit::Ms));
            layout.add (floatParam (mbId (b, "release"), n + "Release", range (5.0f, 2500.0f, 150.0f), defRelease[b], Unit::Ms));
            layout.add (floatParam (mbId (b, "gain"), n + "Level", range (-18.0f, 18.0f), 0.0f, Unit::Db));
            layout.add (choiceParam (mbId (b, "place"), n + "Placement", { "Stereo", "Mid", "Side" }, 0));
        }
        layout.add (floatParam ("x1", "Crossover Low", range (30.0f, 1000.0f, 150.0f), 120.0f, Unit::Hz));
        layout.add (floatParam ("x2", "Crossover Mid", range (200.0f, 5000.0f, 1000.0f), 800.0f, Unit::Hz));
        layout.add (floatParam ("x3", "Crossover High", range (1000.0f, 16000.0f, 5000.0f), 5000.0f, Unit::Hz));
        layout.add (floatParam ("lookahead", "Lookahead", range (0.0f, 20.0f), 0.0f, Unit::Ms));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        layout.add (floatParam ("out", "Output", range (-24.0f, 24.0f), 0.0f, Unit::Db));
        return layout;
    }

    juce::String getProductName() const override { return "Multiband"; }
    juce::Colour getAccent() const override { return juce::Colour (0xff5ec4ff); }

    gitto::MultibandParams readParams() const
    {
        gitto::MultibandParams p;
        for (int b = 0; b < 4; ++b)
        {
            auto& bp = p.bands[(size_t) b];
            bp.enabled = flag (mbId (b, "on"));
            bp.thresholdDb = value (mbId (b, "thr"));
            bp.rangeDb = value (mbId (b, "range"));
            bp.ratio = value (mbId (b, "ratio"));
            bp.attackMs = value (mbId (b, "attack"));
            bp.releaseMs = value (mbId (b, "release"));
            bp.gainDb = value (mbId (b, "gain"));
            bp.placement = (gitto::BandPlacement) choice (mbId (b, "place"));
        }
        p.xover1 = value ("x1");
        p.xover2 = value ("x2");
        p.xover3 = value ("x3");
        p.lookaheadMs = value ("lookahead");
        p.mix = value ("mix");
        p.outputDb = value ("out");
        return p;
    }

    void prepareToPlay (double sr, int) override
    {
        dsp.setParams (readParams());
        dsp.prepare (sr);
        setLatencySamples (dsp.getLatencySamples());
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        if (consumeDirty())
        {
            dsp.setParams (readParams());
            requestLatency (dsp.getLatencySamples());
        }
        dsp.process (buffer.getWritePointer (0), buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr, buffer.getNumSamples());
    }

    std::vector<Preset> getPresets() const override
    {
        return {
            { "Default", {} },
            { "Master Polish", { { "b1_range", -3 }, { "b1_thr", -20 }, { "b1_ratio", 2 }, { "b2_range", -2 }, { "b2_thr", -22 }, { "b2_ratio", 2 },
                                 { "b3_range", -2 }, { "b3_thr", -24 }, { "b3_ratio", 2 }, { "b4_range", -3 }, { "b4_thr", -28 }, { "b4_ratio", 2 }, { "out", 1 } } },
            { "Tighten the Low End", { { "b1_range", -9 }, { "b1_thr", -22 }, { "b1_ratio", 4 }, { "b1_attack", 25 }, { "b2_range", 0 }, { "b3_range", 0 }, { "b4_range", 0 } } },
            { "808 and Kick Control", { { "x1", 90 }, { "b1_range", -10 }, { "b1_thr", -18 }, { "b1_ratio", 6 }, { "b1_attack", 15 }, { "b1_release", 180 },
                                        { "b2_range", -4 }, { "b2_thr", -24 }, { "b3_range", 0 }, { "b4_range", 0 } } },
            { "Vocal De-Harsh", { { "x2", 1800 }, { "x3", 6000 }, { "b1_range", 0 }, { "b2_range", 0 }, { "b3_range", -8 }, { "b3_thr", -30 }, { "b3_ratio", 4 }, { "b3_attack", 3 },
                                  { "b4_range", -5 }, { "b4_thr", -34 }, { "b4_ratio", 4 } } },
            // Upward expansion with thresholds up near the hits and a fast attack, so it is the hits
            // that get lifted, not everything.
            { "Punch Up (Expand)", { { "b1_range", 4 }, { "b1_thr", -16 }, { "b1_attack", 0.5f }, { "b1_release", 80 }, { "b2_range", 3 }, { "b2_thr", -18 }, { "b2_attack", 0.5f }, { "b2_release", 70 },
                                     { "b3_range", 3 }, { "b3_thr", -20 }, { "b3_attack", 0.3f }, { "b3_release", 60 }, { "b4_range", 2 }, { "b4_thr", -24 }, { "b4_attack", 0.3f }, { "b4_release", 50 }, { "lookahead", 3 } } },
            // Deep range and quick attacks so peaks come down further than the body, then the
            // output brings the whole thing up: about 3 dB louder with peaks no higher than before.
            { "Loud and Dense", { { "b1_range", -24 }, { "b1_thr", -24 }, { "b1_ratio", 4 }, { "b1_attack", 4 }, { "b1_release", 120 }, { "b2_range", -24 }, { "b2_thr", -26 }, { "b2_ratio", 4 }, { "b2_attack", 2 }, { "b2_release", 80 },
                                  { "b3_range", -24 }, { "b3_thr", -28 }, { "b3_ratio", 4 }, { "b3_attack", 1 }, { "b3_release", 50 }, { "b4_range", -24 }, { "b4_thr", -32 }, { "b4_ratio", 4 }, { "b4_attack", 0.5f }, { "b4_release", 40 },
                                  { "lookahead", 5 }, { "out", 7.5f } } },
            { "Tame Wide Highs", { { "b1_range", 0 }, { "b2_range", 0 }, { "b3_range", 0 }, { "b4_range", -8 }, { "b4_thr", -36 }, { "b4_ratio", 4 }, { "b4_place", 2 } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Multiband dsp;
};

static LayoutSpec multibandLayout (int band)
{
    LayoutSpec l;
    l.displayHeight = 190;
    l.rows = {
        { { "Band " + juce::String (band + 1),
            { toggle (mbId (band, "on"), "Active"), knob (mbId (band, "thr"), "Threshold"), knob (mbId (band, "range"), "Range", true), knob (mbId (band, "ratio"), "Ratio"),
              knob (mbId (band, "attack"), "Attack"), knob (mbId (band, "release"), "Release"), knob (mbId (band, "gain"), "Level", true), chooser (mbId (band, "place"), "Placement") } } },
        { { "Crossovers", { knob ("x1", "Low"), knob ("x2", "Mid"), knob ("x3", "High") } },
          { "Global", { knob ("lookahead", "Lookahead"), knob ("mix", "Mix"), knob ("out", "Output", true) } } },
    };
    return l;
}

juce::AudioProcessorEditor* MultibandProcessor::createEditor()
{
    auto display = std::make_unique<BandSplitDisplay> (*this, juce::StringArray { "x1", "x2", "x3" });
    auto* d = display.get();
    d->bandValue = [this] (int b) { return dsp.getBandGainDb (b) / 18.0f; };
    d->bandText = [this] (int b)
    {
        const float g = dsp.getBandGainDb (b);
        return dbText (g, true);
    };
    d->bandEnabled = [this] (int b) { return flag (mbId (b, "on")); };
    auto* editor = new GittoEditor (*this, multibandLayout (d->selected), std::move (display));
    d->onSelect = [editor] (int band) { editor->setLayout (multibandLayout (band)); };
    return editor;
}

juce::AudioProcessor* createGittoMultiband() { return new MultibandProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoMultiband(); }
#endif
