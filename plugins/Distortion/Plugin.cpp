// Gitto FX Distortion - plugin wrapper and interface.
#include "../../dsp/Distortion.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
namespace
{
juce::String dsId (int band, const char* name) { return "b" + juce::String (band + 1) + "_" + name; }
}

class DistortionProcessor : public GittoProcessor
{
public:
    DistortionProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
    }

    static juce::StringArray styleNames()
    {
        juce::StringArray names;
        for (int i = 0; i < (int) gitto::ShapeStyle::Count; ++i)
            names.add (gitto::shapeName ((gitto::ShapeStyle) i));
        return names;
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        const int defStyle[4] = { 0, 2, 0, 3 };
        for (int b = 0; b < 4; ++b)
        {
            const juce::String n = "Band " + juce::String (b + 1) + " ";
            layout.add (boolParam (dsId (b, "on"), n + "On", true));
            layout.add (choiceParam (dsId (b, "style"), n + "Style", styleNames(), defStyle[b]));
            layout.add (floatParam (dsId (b, "drive"), n + "Drive", range (0.0f, 1.0f), 0.3f, Unit::Percent));
            layout.add (floatParam (dsId (b, "mix"), n + "Mix", range (0.0f, 1.0f), 1.0f, Unit::Percent));
            layout.add (floatParam (dsId (b, "fb"), n + "Feedback", range (0.0f, 0.9f), 0.0f, Unit::Percent));
            layout.add (floatParam (dsId (b, "dyn"), n + "Dynamics", range (-1.0f, 1.0f), 0.0f, Unit::Percent));
            layout.add (floatParam (dsId (b, "tone"), n + "Tone", range (-1.0f, 1.0f), 0.0f, Unit::Percent));
            layout.add (floatParam (dsId (b, "level"), n + "Level", range (-24.0f, 12.0f), 0.0f, Unit::Db));
        }
        layout.add (floatParam ("x1", "Crossover Low", range (30.0f, 1000.0f, 150.0f), 150.0f, Unit::Hz));
        layout.add (floatParam ("x2", "Crossover Mid", range (200.0f, 5000.0f, 1000.0f), 900.0f, Unit::Hz));
        layout.add (floatParam ("x3", "Crossover High", range (1000.0f, 16000.0f, 5000.0f), 4500.0f, Unit::Hz));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        layout.add (floatParam ("out", "Output", range (-24.0f, 12.0f), 0.0f, Unit::Db));
        return layout;
    }

    juce::String getProductName() const override { return "Distortion"; }
    juce::Colour getAccent() const override { return juce::Colour (0xffff5252); }

    gitto::DistortionParams readParams() const
    {
        gitto::DistortionParams p;
        for (int b = 0; b < 4; ++b)
        {
            auto& bp = p.bands[(size_t) b];
            bp.enabled = flag (dsId (b, "on"));
            bp.style = (gitto::ShapeStyle) choice (dsId (b, "style"));
            bp.drive = value (dsId (b, "drive"));
            bp.mix = value (dsId (b, "mix"));
            bp.feedback = value (dsId (b, "fb"));
            bp.dynamics = value (dsId (b, "dyn"));
            bp.tone = value (dsId (b, "tone"));
            bp.levelDb = value (dsId (b, "level"));
        }
        p.xover1 = value ("x1");
        p.xover2 = value ("x2");
        p.xover3 = value ("x3");
        p.mix = value ("mix");
        p.outputDb = value ("out");
        return p;
    }

    void prepareToPlay (double sr, int) override
    {
        dsp.setParams (readParams());
        dsp.prepare (sr);
        setLatencySamples (gitto::Distortion::kLatency);
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
        // styles: 0 Soft Tube, 1 Warm Tube, 2 Tape, 3 Transformer, 4 Console, 5 Diode, 6 Hard Clip, 7 Fuzz, 8 Rectify, 9 Foldback, 10 Sine Fold, 11 Crush
        return {
            { "Default", {} },
            { "Warm Mix Glue", { { "b1_style", 2 }, { "b1_drive", 0.2f }, { "b2_style", 1 }, { "b2_drive", 0.25f }, { "b3_style", 0 }, { "b3_drive", 0.2f }, { "b4_style", 2 }, { "b4_drive", 0.15f } } },
            { "808 Growl", { { "x1", 120 }, { "b1_style", 1 }, { "b1_drive", 0.55f }, { "b1_dyn", 0.4f }, { "b2_style", 5 }, { "b2_drive", 0.6f }, { "b2_tone", 0.2f }, { "b3_on", 0 }, { "b4_on", 0 }, { "out", 2.5f } } },
            { "Drum Smash", { { "b1_style", 2 }, { "b1_drive", 0.45f }, { "b2_style", 5 }, { "b2_drive", 0.6f }, { "b2_dyn", 0.5f }, { "b3_style", 6 }, { "b3_drive", 0.5f }, { "b4_style", 3 }, { "b4_drive", 0.35f }, { "out", 1 } } },
            { "Vocal Edge", { { "b1_on", 0 }, { "b2_style", 0 }, { "b2_drive", 0.3f }, { "b3_style", 5 }, { "b3_drive", 0.5f }, { "b3_mix", 0.5f }, { "b4_style", 2 }, { "b4_drive", 0.25f } } },
            { "Top End Fizz", { { "b1_on", 0 }, { "b2_on", 0 }, { "b3_style", 0 }, { "b3_drive", 0.25f }, { "b4_style", 6 }, { "b4_drive", 0.6f }, { "b4_mix", 0.4f } } },
            { "Fuzz Wall", { { "b1_style", 7 }, { "b1_drive", 0.7f }, { "b2_style", 7 }, { "b2_drive", 0.8f }, { "b3_style", 7 }, { "b3_drive", 0.8f }, { "b4_style", 7 }, { "b4_drive", 0.6f }, { "b4_tone", -0.4f } } },
            { "Folded Synth", { { "b1_style", 0 }, { "b1_drive", 0.2f }, { "b2_style", 9 }, { "b2_drive", 0.6f }, { "b3_style", 10 }, { "b3_drive", 0.6f }, { "b4_style", 9 }, { "b4_drive", 0.4f }, { "b4_level", -3 }, { "out", 3 } } },
            { "Lo-Fi Crush", { { "b1_style", 2 }, { "b1_drive", 0.3f }, { "b2_style", 11 }, { "b2_drive", 0.4f }, { "b3_style", 11 }, { "b3_drive", 0.5f }, { "b4_style", 11 }, { "b4_drive", 0.5f }, { "b4_tone", -0.5f } } },
            { "Resonant Scream", { { "b2_style", 5 }, { "b2_drive", 0.6f }, { "b2_fb", 0.6f }, { "b3_style", 6 }, { "b3_drive", 0.7f }, { "b3_fb", 0.7f }, { "b3_level", -4 }, { "mix", 0.6f } } },
            { "Octave Up Grit", { { "b1_on", 0 }, { "b2_style", 8 }, { "b2_drive", 0.6f }, { "b3_style", 8 }, { "b3_drive", 0.6f }, { "b4_style", 0 }, { "b4_drive", 0.2f }, { "mix", 0.5f } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Distortion dsp;
};

static LayoutSpec distortionLayout (int band)
{
    LayoutSpec l;
    l.displayHeight = 180;
    l.rows = {
        { { "Band " + juce::String (band + 1),
            { toggle (dsId (band, "on"), "Active"), chooser (dsId (band, "style"), "Style"), knob (dsId (band, "drive"), "Drive"), knob (dsId (band, "dyn"), "Dynamics", true),
              knob (dsId (band, "fb"), "Feedback"), knob (dsId (band, "tone"), "Tone", true), knob (dsId (band, "mix"), "Mix"), knob (dsId (band, "level"), "Level", true) } } },
        { { "Crossovers", { knob ("x1", "Low"), knob ("x2", "Mid"), knob ("x3", "High") } },
          { "Global", { knob ("mix", "Mix"), knob ("out", "Output", true) } } },
    };
    return l;
}

juce::AudioProcessorEditor* DistortionProcessor::createEditor()
{
    auto display = std::make_unique<BandSplitDisplay> (*this, juce::StringArray { "x1", "x2", "x3" });
    auto* d = display.get();
    // Bar height: how much each band is changing the signal, on a dB-like scale.
    d->bandValue = [this] (int b) { return juce::jlimit (0.0f, 1.0f, (gitto::gainToDb (dsp.getBandActivity (b)) + 60.0f) / 60.0f); };
    d->bandText = [this] (int b) { return styleNames()[choice (dsId (b, "style"))]; };
    d->bandEnabled = [this] (int b) { return flag (dsId (b, "on")); };
    auto* editor = new GittoEditor (*this, distortionLayout (d->selected), std::move (display));
    d->onSelect = [editor] (int band) { editor->setLayout (distortionLayout (band)); };
    return editor;
}

juce::AudioProcessor* createGittoDistortion() { return new DistortionProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoDistortion(); }
#endif
