// Gitto FX Bus Comp - plugin wrapper and interface.
#include "../../dsp/ClassicComps.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class BusProcessor : public GittoProcessor
{
public:
    BusProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        layout.add (floatParam ("threshold", "Threshold", range (-40.0f, 0.0f), -16.0f, Unit::Db));
        layout.add (choiceParam ("ratio", "Ratio", { "1.5 : 1", "2 : 1", "4 : 1", "10 : 1" }, 2));
        layout.add (choiceParam ("attack", "Attack", { "0.1 ms", "0.3 ms", "1 ms", "3 ms", "10 ms", "30 ms" }, 3));
        layout.add (choiceParam ("release", "Release", { "0.1 s", "0.3 s", "0.6 s", "1.2 s", "Auto" }, 4));
        layout.add (floatParam ("makeup", "Makeup", range (0.0f, 24.0f), 0.0f, Unit::Db));
        layout.add (floatParam ("schpf", "Sidechain High-Pass", range (20.0f, 300.0f, 90.0f), 20.0f, Unit::Hz));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        return layout;
    }

    juce::String getProductName() const override { return "Bus Comp"; }
    juce::Colour getAccent() const override { return juce::Colour (0xffd9a066); }

    gitto::BusParams readParams() const
    {
        gitto::BusParams p;
        p.thresholdDb = value ("threshold");
        p.ratio = choice ("ratio");
        p.attack = choice ("attack");
        p.release = choice ("release");
        p.makeupDb = value ("makeup");
        p.scHpfHz = value ("schpf");
        p.mix = value ("mix");
        return p;
    }

    void prepareToPlay (double sr, int) override
    {
        comp.setParams (readParams());
        comp.prepare (sr);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        if (consumeDirty())
            comp.setParams (readParams());
        comp.process (buffer.getWritePointer (0), buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr, buffer.getNumSamples());
    }

    std::vector<Preset> getPresets() const override
    {
        // ratio: 0=1.5, 1=2, 2=4, 3=10. attack: 0..5 = 0.1..30 ms. release: 0..3 = 0.1..1.2 s, 4 = Auto
        return {
            { "Default", {} },
            // Thresholds are set on material peaking around -6 dBFS. Makeup is kept a little short of
            // the reduction on the mix and master presets so peaks do not end up higher than they began.
            { "Mix Bus Glue", { { "threshold", -21 }, { "ratio", 1 }, { "attack", 4 }, { "release", 4 }, { "makeup", 1 }, { "schpf", 60 } } },
            { "Master 2 dB", { { "threshold", -28 }, { "ratio", 0 }, { "attack", 5 }, { "release", 4 }, { "makeup", 0.5f }, { "schpf", 90 } } },
            { "Drum Bus Punch", { { "threshold", -22 }, { "ratio", 2 }, { "attack", 5 }, { "release", 0 }, { "makeup", 3 }, { "schpf", 80 } } },
            { "Drum Bus Smack", { { "threshold", -23 }, { "ratio", 3 }, { "attack", 4 }, { "release", 0 }, { "makeup", 4 } } },
            { "Beat Glue", { { "threshold", -22 }, { "ratio", 2 }, { "attack", 4 }, { "release", 1 }, { "makeup", 2.5f }, { "schpf", 110 } } },
            { "Parallel Bus", { { "threshold", -30 }, { "ratio", 3 }, { "attack", 0 }, { "release", 0 }, { "makeup", 11 }, { "mix", 0.4f } } },
            { "Piano Hold", { { "threshold", -22 }, { "ratio", 2 }, { "attack", 3 }, { "release", 2 }, { "makeup", 4 } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override
    {
        LayoutSpec l;
        l.displayHeight = 170;
        l.rows = { {
            { "Compression", { knob ("threshold", "Threshold"), chooser ("ratio", "Ratio"), knob ("makeup", "Makeup") } },
            { "Timing", { chooser ("attack", "Attack"), chooser ("release", "Release") } },
            { "Sidechain and Mix", { knob ("schpf", "High-Pass"), knob ("mix", "Mix") } },
        } };
        l.meters = {
            { "IN", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (comp.getInputLevel()); } },
            { "OUT", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (comp.getOutputLevel()); } },
        };
        return new GittoEditor (*this, std::move (l), std::make_unique<VuDisplay> ([this] { return comp.getGainReductionDb(); }, getAccent(), "Gain reduction", 20.0f));
    }

    gitto::BusComp comp;
};

juce::AudioProcessor* createGittoBusComp() { return new BusProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoBusComp(); }
#endif
