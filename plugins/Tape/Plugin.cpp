// Gitto FX Tape - plugin wrapper and interface.
#include "../../dsp/Tape.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class TapeProcessor : public GittoProcessor
{
public:
    TapeProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        layout.add (floatParam ("input", "Input", range (-12.0f, 24.0f), 0.0f, Unit::Db));
        layout.add (floatParam ("out", "Output", range (-24.0f, 12.0f), 0.0f, Unit::Db));
        layout.add (boolParam ("autogain", "Auto Gain", true));
        layout.add (choiceParam ("speed", "Speed", { "7.5 ips", "15 ips", "30 ips" }, 1));
        layout.add (choiceParam ("formula", "Formula", { "Modern", "Vintage", "Cassette" }, 0));
        layout.add (floatParam ("bias", "Bias", range (-1.0f, 1.0f), 0.0f, Unit::Percent));
        layout.add (floatParam ("bump", "Head Bump", range (0.0f, 1.0f), 0.5f, Unit::Percent));
        layout.add (floatParam ("wow", "Wow", range (0.0f, 1.0f), 0.1f, Unit::Percent));
        layout.add (floatParam ("flutter", "Flutter", range (0.0f, 1.0f), 0.1f, Unit::Percent));
        layout.add (floatParam ("hiss", "Hiss", range (0.0f, 1.0f), 0.0f, Unit::Percent));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        return layout;
    }

    juce::String getProductName() const override { return "Tape"; }
    juce::Colour getAccent() const override { return juce::Colour (0xffc9a27e); }

    gitto::TapeParams readParams() const
    {
        gitto::TapeParams p;
        p.inputDb = value ("input");
        p.outputDb = value ("out");
        p.autoGain = flag ("autogain");
        p.speed = (gitto::TapeSpeed) choice ("speed");
        p.formula = (gitto::TapeFormula) choice ("formula");
        p.bias = value ("bias");
        p.headBump = value ("bump");
        p.wow = value ("wow");
        p.flutter = value ("flutter");
        p.hiss = value ("hiss");
        p.mix = value ("mix");
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
            dsp.setParams (readParams());
        dsp.process (buffer.getWritePointer (0), buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr, buffer.getNumSamples());
    }

    std::vector<Preset> getPresets() const override
    {
        // speed: 0 = 7.5, 1 = 15, 2 = 30 ips. formula: 0 Modern, 1 Vintage, 2 Cassette
        return {
            { "Default", {} },
            { "Master at 30 ips", { { "speed", 2 }, { "formula", 0 }, { "input", 3 }, { "bump", 0.4f }, { "wow", 0.03f }, { "flutter", 0.03f } } },
            { "Mix Bus 15 ips", { { "speed", 1 }, { "formula", 0 }, { "input", 5 }, { "bump", 0.5f }, { "wow", 0.06f }, { "flutter", 0.06f } } },
            { "Vintage Drums", { { "speed", 1 }, { "formula", 1 }, { "input", 9 }, { "bump", 0.7f }, { "bias", 0.2f } } },
            { "Fat 808", { { "speed", 0 }, { "formula", 1 }, { "input", 8 }, { "bump", 1.0f }, { "bias", 0.4f }, { "wow", 0.0f }, { "flutter", 0.0f }, { "out", -3.5f } } },
            { "Hot Vocal", { { "speed", 1 }, { "formula", 1 }, { "input", 7 }, { "bias", -0.2f }, { "bump", 0.2f } } },
            { "Slow and Dark", { { "speed", 0 }, { "formula", 1 }, { "input", 6 }, { "bias", 0.6f }, { "wow", 0.25f }, { "flutter", 0.2f }, { "hiss", 0.2f } } },
            { "Old Cassette", { { "speed", 0 }, { "formula", 2 }, { "input", 8 }, { "wow", 0.55f }, { "flutter", 0.5f }, { "hiss", 0.45f }, { "bump", 0.6f } } },
            { "Lo-Fi Beat Tape", { { "speed", 0 }, { "formula", 2 }, { "input", 12 }, { "wow", 0.7f }, { "flutter", 0.4f }, { "hiss", 0.3f }, { "bias", 0.3f } } },
            { "Wobbly Keys", { { "speed", 1 }, { "formula", 1 }, { "input", 4 }, { "wow", 0.85f }, { "flutter", 0.3f } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Tape dsp;
};

class TapeDisplay : public juce::Component
{
public:
    explicit TapeDisplay (TapeProcessor& p) : proc (p) {}

    void paint (juce::Graphics& g) override
    {
        const juce::Colour accent = proc.getAccent();
        auto area = getLocalBounds().toFloat().reduced (8.0f);
        auto curveBox = area.removeFromLeft (juce::jmin (area.getHeight(), area.getWidth() * 0.36f));
        area.removeFromLeft (10.0f);

        const auto p = proc.readParams();
        model.setParams (p);
        drawTransferCurve (g, curveBox, accent, [this] (float x) { return model.transfer (x); }, proc.dsp.getInputLevel());

        // Two reels, turning at a rate set by the tape speed.
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        static const float turnsPerSecond[3] = { 0.25f, 0.5f, 1.0f };
        angle += turnsPerSecond[juce::jlimit (0, 2, (int) p.speed)] * juce::MathConstants<float>::twoPi / 30.0f;
        const float radius = juce::jmin (area.getHeight() * 0.36f, area.getWidth() * 0.2f);
        const float cy = area.getCentreY() + 4.0f;
        for (int reel = 0; reel < 2; ++reel)
        {
            const float cx = area.getCentreX() + (reel == 0 ? -1.0f : 1.0f) * radius * 1.45f;
            g.setColour (colours::panelEdge);
            g.fillEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
            g.setColour (accent.withAlpha (0.55f));
            const float pack = radius * (reel == 0 ? 0.82f : 0.6f);
            g.fillEllipse (cx - pack, cy - pack, pack * 2.0f, pack * 2.0f);
            g.setColour (colours::background);
            for (int k = 0; k < 3; ++k)
            {
                const float a = angle + (float) k * juce::MathConstants<float>::twoPi / 3.0f;
                const float hx = cx + std::sin (a) * radius * 0.36f, hy = cy - std::cos (a) * radius * 0.36f;
                g.fillEllipse (hx - radius * 0.15f, hy - radius * 0.15f, radius * 0.3f, radius * 0.3f);
            }
            g.fillEllipse (cx - radius * 0.1f, cy - radius * 0.1f, radius * 0.2f, radius * 0.2f);
        }
        static const char* speeds[3] = { "7.5 IPS", "15 IPS", "30 IPS" };
        static const char* formulas[3] = { "MODERN", "VINTAGE", "CASSETTE" };
        g.setColour (colours::dim);
        g.setFont (font (9.5f, true));
        g.drawText (juce::String (speeds[juce::jlimit (0, 2, (int) p.speed)]) + "  |  " + formulas[juce::jlimit (0, 2, (int) p.formula)],
                    area.reduced (10.0f, 8.0f), juce::Justification::topLeft);
    }

private:
    TapeProcessor& proc;
    gitto::Tape model; // used only for its static curve
    float angle = 0.0f;
};

juce::AudioProcessorEditor* TapeProcessor::createEditor()
{
    LayoutSpec l;
    l.displayHeight = 170;
    l.rows = {
        { { "Machine", { chooser ("speed", "Speed"), chooser ("formula", "Formula") } },
          { "Level", { knob ("input", "Input", true), toggle ("autogain", "Auto Gain"), knob ("out", "Output", true), knob ("mix", "Mix") } } },
        { { "Tone", { knob ("bias", "Bias", true), knob ("bump", "Head Bump") } },
          { "Transport", { knob ("wow", "Wow"), knob ("flutter", "Flutter") } },
          { "Noise", { knob ("hiss", "Hiss") } } },
    };
    l.meters = {
        { "IN", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (dsp.getInputLevel()); } },
        { "OUT", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (dsp.getOutputLevel()); } },
    };
    return new GittoEditor (*this, std::move (l), std::make_unique<TapeDisplay> (*this));
}

juce::AudioProcessor* createGittoTape() { return new TapeProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoTape(); }
#endif
