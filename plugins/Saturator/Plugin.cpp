// Gitto FX Saturator - plugin wrapper and interface.
#include "../../dsp/Saturator.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class SaturatorProcessor : public GittoProcessor
{
public:
    SaturatorProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        layout.add (choiceParam ("style", "Style", { "Warm", "Tape", "Console", "Crunch", "Fuzz" }, 0));
        layout.add (floatParam ("drive", "Drive", range (0.0f, 1.0f), 0.35f, Unit::Percent));
        layout.add (boolParam ("boost", "Boost", false));
        layout.add (floatParam ("lowcut", "Low Cut", range (20.0f, 500.0f, 100.0f), 20.0f, Unit::Hz));
        layout.add (floatParam ("tone", "Tone", range (-1.0f, 1.0f), 0.0f, Unit::Percent));
        layout.add (floatParam ("highcut", "High Cut", range (2000.0f, 20000.0f, 8000.0f), 20000.0f, Unit::Hz));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        layout.add (floatParam ("out", "Output", range (-24.0f, 12.0f), 0.0f, Unit::Db));
        layout.add (boolParam ("autogain", "Auto Gain", true));
        return layout;
    }

    juce::String getProductName() const override { return "Saturator"; }
    juce::Colour getAccent() const override { return juce::Colour (0xffff6f91); }

    gitto::SaturatorParams readParams() const
    {
        gitto::SaturatorParams p;
        p.style = (gitto::SatStyle) choice ("style");
        p.drive = value ("drive");
        p.boost = flag ("boost");
        p.lowCutHz = value ("lowcut");
        p.tone = value ("tone");
        p.highCutHz = value ("highcut");
        p.mix = value ("mix");
        p.outputDb = value ("out");
        p.autoGain = flag ("autogain");
        return p;
    }

    void prepareToPlay (double sr, int) override
    {
        dsp.setParams (readParams());
        dsp.prepare (sr);
        setLatencySamples (gitto::Saturator::kLatency);
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
        // style: 0 Warm, 1 Tape, 2 Console, 3 Crunch, 4 Fuzz
        return {
            { "Default", {} },
            { "Vocal Warmth", { { "style", 0 }, { "drive", 0.3f }, { "lowcut", 90 }, { "tone", 0.1f } } },
            { "808 Harmonics", { { "style", 0 }, { "drive", 0.55f }, { "highcut", 6000 }, { "mix", 0.7f } } },
            { "Drum Bus Tape", { { "style", 1 }, { "drive", 0.45f }, { "tone", -0.1f }, { "out", -1 } } },
            { "Console Push", { { "style", 2 }, { "drive", 0.4f } } },
            { "Snare Crunch", { { "style", 3 }, { "drive", 0.55f }, { "lowcut", 120 }, { "mix", 0.6f } } },
            { "Bass Bite", { { "style", 3 }, { "drive", 0.5f }, { "lowcut", 40 }, { "highcut", 5000 }, { "mix", 0.45f }, { "out", 2 } } },
            { "Punish", { { "style", 3 }, { "drive", 0.7f }, { "boost", 1 }, { "highcut", 9000 }, { "out", -2.5f } } },
            { "Fuzz Lead", { { "style", 4 }, { "drive", 0.7f }, { "tone", -0.3f }, { "highcut", 7000 } } },
            { "Parallel Dirt", { { "style", 3 }, { "drive", 0.85f }, { "boost", 1 }, { "mix", 0.25f }, { "lowcut", 200 }, { "out", 2 } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Saturator dsp;
};

class SaturatorDisplay : public juce::Component
{
public:
    explicit SaturatorDisplay (SaturatorProcessor& p) : proc (p) {}

    void paint (juce::Graphics& g) override
    {
        const juce::Colour accent = proc.getAccent();
        auto area = getLocalBounds().toFloat().reduced (8.0f);
        auto curveBox = area.removeFromLeft (juce::jmin (area.getHeight(), area.getWidth() * 0.4f));
        area.removeFromLeft (10.0f);

        model.setParams (proc.readParams());
        drawTransferCurve (g, curveBox, accent, [this] (float x) { return model.transfer (x); }, proc.dsp.getInputLevel());

        // Harmonics the curve adds to a pure tone at -12 dBFS, 2nd to 9th.
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        constexpr int n = 256;
        std::vector<std::complex<float>> spec ((size_t) n);
        for (int i = 0; i < n; ++i)
            spec[(size_t) i] = model.transfer (0.25f * std::sin (juce::MathConstants<float>::twoPi * 4.0f * (float) i / (float) n));
        gitto::fft (spec);
        const float fundamental = std::abs (spec[4]) + 1.0e-9f;
        const auto plot = area.reduced (14.0f, 10.0f).withTrimmedTop (14.0f).withTrimmedBottom (14.0f);
        const float slot = plot.getWidth() / 8.0f;
        g.setFont (font (10.0f, true));
        for (int h = 2; h <= 9; ++h)
        {
            const float db = 20.0f * std::log10 (std::abs (spec[(size_t) (4 * h)]) / fundamental + 1.0e-9f);
            const float height = juce::jlimit (0.0f, 1.0f, (db + 80.0f) / 80.0f) * plot.getHeight();
            const auto bar = juce::Rectangle<float> (plot.getX() + slot * (float) (h - 2) + slot * 0.2f, plot.getBottom() - height, slot * 0.6f, height);
            g.setColour ((h % 2 == 0 ? accent : juce::Colour (0xff6ec6ff)).withAlpha (0.9f));
            g.fillRoundedRectangle (bar, 2.0f);
            g.setColour (colours::dim);
            g.drawText (juce::String (h), juce::Rectangle<float> (bar.getX() - 6.0f, plot.getBottom() + 2.0f, bar.getWidth() + 12.0f, 12.0f), juce::Justification::centred);
        }
        g.setColour (colours::dim);
        g.setFont (font (9.5f, true));
        g.drawText ("HARMONICS ADDED (EVEN / ODD)", area.reduced (10.0f, 8.0f), juce::Justification::topLeft);
        g.setColour (colours::text);
        g.setFont (font (12.0f, true));
        g.drawText ("+" + juce::String (model.driveDb(), 1) + " dB drive", area.reduced (10.0f, 8.0f), juce::Justification::topRight);
    }

private:
    SaturatorProcessor& proc;
    gitto::Saturator model; // used only for its static curve
};

juce::AudioProcessorEditor* SaturatorProcessor::createEditor()
{
    LayoutSpec l;
    l.displayHeight = 170;
    l.rows = { {
        { "Saturation", { chooser ("style", "Style"), knob ("drive", "Drive"), toggle ("boost", "Boost") } },
        { "Tone", { knob ("lowcut", "Low Cut"), knob ("tone", "Tone", true), knob ("highcut", "High Cut") } },
        { "Output", { knob ("mix", "Mix"), toggle ("autogain", "Auto Gain"), knob ("out", "Level", true) } },
    } };
    l.meters = {
        { "IN", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (dsp.getInputLevel()); } },
        { "OUT", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (dsp.getOutputLevel()); } },
    };
    return new GittoEditor (*this, std::move (l), std::make_unique<SaturatorDisplay> (*this));
}

juce::AudioProcessor* createGittoSaturator() { return new SaturatorProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoSaturator(); }
#endif
