// Gitto FX FET Comp - plugin wrapper and interface.
#include "../../dsp/ClassicComps.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class FetProcessor : public GittoProcessor
{
public:
    FetProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        layout.add (floatParam ("input", "Input", range (-12.0f, 36.0f), 0.0f, Unit::Db));
        layout.add (floatParam ("output", "Output", range (-36.0f, 12.0f), 0.0f, Unit::Db));
        layout.add (choiceParam ("ratio", "Ratio", { "4 : 1", "8 : 1", "12 : 1", "20 : 1", "All" }, 0));
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { "attack", 1 }, "Attack", range (0.0f, 1.0f), 0.5f,
            Attr().withStringFromValueFunction ([] (float v, int) { return juce::String (0.02f * std::pow (40.0f, v), 2) + " ms"; })));
        layout.add (floatParam ("release", "Release", range (50.0f, 1100.0f, 300.0f), 250.0f, Unit::Ms));
        layout.add (floatParam ("schpf", "Sidechain High-Pass", range (20.0f, 500.0f, 100.0f), 20.0f, Unit::Hz));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        return layout;
    }

    juce::String getProductName() const override { return "FET Comp"; }
    juce::Colour getAccent() const override { return juce::Colour (0xffff8a5b); }

    gitto::FetParams readParams() const
    {
        gitto::FetParams p;
        p.inputDb = value ("input");
        p.outputDb = value ("output");
        p.ratio = choice ("ratio");
        p.attack = value ("attack");
        p.releaseMs = value ("release");
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
        return {
            { "Default", {} },
            // Input and Output are set on tracks peaking around -6 dBFS: the stated reduction is the
            // average on that material, and Output brings the level back to where it started.
            { "Rap Vocal", { { "input", 6 }, { "output", 0 }, { "ratio", 0 }, { "attack", 0.6f }, { "release", 120 } } },
            { "Vocal Aggressive", { { "input", 13 }, { "output", -2.5f }, { "ratio", 1 }, { "attack", 0.3f }, { "release", 80 } } },
            { "Snare Crack", { { "input", 9 }, { "output", -4 }, { "ratio", 0 }, { "attack", 1.0f }, { "release", 60 } } },
            { "Kick Thump", { { "input", 4 }, { "output", 0 }, { "ratio", 0 }, { "attack", 1.0f }, { "release", 200 }, { "schpf", 60 } } },
            { "Bass Pin", { { "input", 13 }, { "output", -5 }, { "ratio", 2 }, { "attack", 0.7f }, { "release", 300 } } },
            { "Room Crush (All)", { { "input", 20 }, { "output", -4.5f }, { "ratio", 4 }, { "attack", 0.0f }, { "release", 50 } } },
            { "Parallel Drums (All)", { { "input", 17 }, { "output", -1 }, { "ratio", 4 }, { "attack", 0.2f }, { "release", 70 }, { "mix", 0.5f } } },
            { "Peak Limit", { { "input", 7 }, { "output", -4.5f }, { "ratio", 3 }, { "attack", 0.0f }, { "release", 100 } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override
    {
        LayoutSpec l;
        l.displayHeight = 170;
        l.rows = { {
            { "Level", { knob ("input", "Input", true), knob ("output", "Output", true) } },
            { "Compression", { chooser ("ratio", "Ratio"), knob ("attack", "Attack"), knob ("release", "Release") } },
            { "Sidechain and Mix", { knob ("schpf", "High-Pass"), knob ("mix", "Mix") } },
        } };
        l.meters = {
            { "IN", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (comp.getInputLevel()); } },
            { "OUT", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (comp.getOutputLevel()); } },
        };
        auto display = std::make_unique<VuDisplay> ([this] { return comp.getGainReductionDb(); }, getAccent(), "Gain reduction", 20.0f);
        display->infoText = [] { return juce::String ("Fixed threshold: turn Input up for more"); };
        return new GittoEditor (*this, std::move (l), std::move (display));
    }

    gitto::FetComp comp;
};

juce::AudioProcessor* createGittoFetComp() { return new FetProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoFetComp(); }
#endif
