// Gitto FX Opto Comp - plugin wrapper and interface.
#include "../../dsp/ClassicComps.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class OptoProcessor : public GittoProcessor
{
public:
    OptoProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        layout.add (floatParam ("reduction", "Peak Reduction", range (0.0f, 1.0f), 0.4f, Unit::Percent));
        layout.add (floatParam ("gain", "Gain", range (-6.0f, 36.0f), 0.0f, Unit::Db));
        layout.add (floatParam ("ratio", "Ratio", range (2.0f, 10.0f, 4.0f), 3.0f, Unit::Ratio));
        layout.add (choiceParam ("timing", "Timing", { "Fixed", "Manual" }, 0));
        layout.add (floatParam ("attack", "Attack", range (0.5f, 300.0f, 20.0f), 10.0f, Unit::Ms));
        layout.add (floatParam ("release", "Release", range (50.0f, 10000.0f, 800.0f), 500.0f, Unit::Ms));
        layout.add (floatParam ("schpf", "Sidechain High-Pass", range (20.0f, 500.0f, 100.0f), 20.0f, Unit::Hz));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        return layout;
    }

    juce::String getProductName() const override { return "Opto Comp"; }
    juce::Colour getAccent() const override { return juce::Colour (0xffe8c25a); }

    gitto::OptoParams readParams() const
    {
        gitto::OptoParams p;
        p.reduction = value ("reduction");
        p.gainDb = value ("gain");
        p.ratio = value ("ratio");
        p.manual = choice ("timing") == 1;
        p.attackMs = value ("attack");
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
            // Set on tracks peaking around -6 dBFS; Gain brings the level back to where it started.
            { "Smooth Vocal", { { "reduction", 0.55f }, { "ratio", 3 }, { "gain", 4 } } },
            { "Vocal Limit", { { "reduction", 0.59f }, { "ratio", 10 }, { "gain", 7 } } },
            { "Bass Leveller", { { "reduction", 0.49f }, { "ratio", 4 }, { "gain", 6 }, { "schpf", 20 } } },
            { "Gentle Keys", { { "reduction", 0.47f }, { "ratio", 2 }, { "gain", 2 } } },
            { "808 Hold", { { "reduction", 0.48f }, { "ratio", 6 }, { "timing", 1 }, { "attack", 30 }, { "release", 400 }, { "gain", 6 } } },
            { "Parallel Glow", { { "reduction", 0.53f }, { "ratio", 10 }, { "gain", 7 }, { "mix", 0.5f } } },
            { "Mix Glue", { { "reduction", 0.47f }, { "ratio", 2 }, { "gain", 1.5f }, { "schpf", 80 } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override
    {
        LayoutSpec l;
        l.displayHeight = 170;
        l.rows = { {
            { "Compression", { knob ("reduction", "Peak Reduction"), knob ("ratio", "Ratio"), knob ("gain", "Gain", true) } },
            { "Timing", { chooser ("timing", "Mode"), knob ("attack", "Attack"), knob ("release", "Release") } },
            { "Sidechain and Mix", { knob ("schpf", "High-Pass"), knob ("mix", "Mix") } },
        } };
        l.meters = {
            { "IN", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (comp.getInputLevel()); } },
            { "OUT", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (comp.getOutputLevel()); } },
        };
        auto display = std::make_unique<VuDisplay> ([this] { return comp.getGainReductionDb(); }, getAccent(), "Gain reduction", 20.0f);
        display->infoText = [this] { return "Threshold " + juce::String (-48.0f * value ("reduction"), 1) + " dB"; };
        auto* editor = new GittoEditor (*this, std::move (l), std::move (display));
        editor->onTimer = [this, editor]
        {
            const bool manual = choice ("timing") == 1;
            editor->setControlEnabled ("attack", manual);
            editor->setControlEnabled ("release", manual);
        };
        return editor;
    }

    gitto::OptoComp comp;
};

juce::AudioProcessor* createGittoOptoComp() { return new OptoProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoOptoComp(); }
#endif
