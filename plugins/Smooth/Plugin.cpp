// Gitto FX Smooth - plugin wrapper and interface.
#include "../../dsp/Smooth.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class SmoothProcessor : public GittoProcessor
{
public:
    SmoothProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        layout.add (floatParam ("depth", "Depth", range (0.0f, 1.0f), 0.5f, Unit::Percent));
        layout.add (floatParam ("sharpness", "Sharpness", range (0.0f, 1.0f), 0.5f, Unit::Percent));
        layout.add (floatParam ("selectivity", "Selectivity", range (0.0f, 1.0f), 0.4f, Unit::Percent));
        layout.add (floatParam ("attack", "Attack", range (1.0f, 100.0f, 15.0f), 8.0f, Unit::Ms));
        layout.add (floatParam ("release", "Release", range (10.0f, 500.0f, 100.0f), 60.0f, Unit::Ms));
        layout.add (floatParam ("low", "Range Low", range (20.0f, 5000.0f, 500.0f), 150.0f, Unit::Hz));
        layout.add (floatParam ("high", "Range High", range (1000.0f, 20000.0f, 8000.0f), 16000.0f, Unit::Hz));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        layout.add (floatParam ("out", "Output", range (-12.0f, 12.0f), 0.0f, Unit::Db));
        layout.add (boolParam ("delta", "Delta", false));
        return layout;
    }

    juce::String getProductName() const override { return "Smooth"; }
    juce::Colour getAccent() const override { return juce::Colour (0xff7ee0c3); }

    gitto::SmoothParams readParams() const
    {
        gitto::SmoothParams p;
        p.depth = value ("depth");
        p.sharpness = value ("sharpness");
        p.selectivity = value ("selectivity");
        p.attackMs = value ("attack");
        p.releaseMs = value ("release");
        p.lowHz = value ("low");
        p.highHz = value ("high");
        p.mix = value ("mix");
        p.outputDb = value ("out");
        p.delta = flag ("delta");
        return p;
    }

    void prepareToPlay (double sr, int) override
    {
        sampleRate = sr;
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
        return {
            { "Default", {} },
            { "Harsh Vocal", { { "depth", 0.55f }, { "sharpness", 0.6f }, { "selectivity", 0.25f }, { "low", 1500 }, { "high", 12000 } } },
            { "Vocal Mud and Boxiness", { { "depth", 0.5f }, { "sharpness", 0.4f }, { "selectivity", 0.15f }, { "low", 150 }, { "high", 900 } } },
            { "Bright Hi-Hats", { { "depth", 0.6f }, { "sharpness", 0.7f }, { "selectivity", 0.1f }, { "low", 4000 }, { "high", 18000 }, { "attack", 2 }, { "release", 40 } } },
            { "Boomy 808", { { "depth", 0.5f }, { "sharpness", 0.5f }, { "selectivity", 0.2f }, { "low", 40 }, { "high", 400 }, { "attack", 20 }, { "release", 150 } } },
            { "Synth Resonance Tamer", { { "depth", 0.7f }, { "sharpness", 0.8f }, { "selectivity", 0.5f }, { "low", 300 }, { "high", 10000 } } },
            { "Gentle Mix Bus", { { "depth", 0.25f }, { "sharpness", 0.3f }, { "selectivity", 0.35f }, { "low", 200 }, { "high", 14000 }, { "release", 120 } } },
            { "Cymbal Wash", { { "depth", 0.65f }, { "sharpness", 0.5f }, { "selectivity", 0.15f }, { "low", 3000 }, { "high", 16000 } } },
            { "Hard Clean-Up", { { "depth", 1.0f }, { "sharpness", 0.6f }, { "selectivity", 0.1f }, { "low", 100 }, { "high", 18000 } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Smooth dsp;
    double sampleRate = 48000.0;
};

class SmoothDisplay : public juce::Component
{
public:
    explicit SmoothDisplay (SmoothProcessor& p) : proc (p) {}

    void paint (juce::Graphics& g) override
    {
        const juce::Colour accent = proc.getAccent();
        const auto area = getLocalBounds().toFloat().reduced (8.0f);
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        const auto r = area.reduced (4.0f);

        // Working range.
        const float lo = proc.value ("low"), hi = proc.value ("high");
        const float x0 = r.getX() + freqToX (juce::jlimit (20.0f, 20000.0f, lo), r.getWidth());
        const float x1 = r.getX() + freqToX (juce::jlimit (20.0f, 20000.0f, hi), r.getWidth());
        g.setColour (colours::background.brighter (0.06f));
        g.fillRect (x0, r.getY(), juce::jmax (0.0f, x1 - x0), r.getHeight());
        drawFrequencyGrid (g, r);

        const auto& mag = proc.dsp.getSpectrumDb();
        const auto& red = proc.dsp.getReductionDb();
        const int bins = (int) juce::jmin (mag.size(), red.size());
        if (bins < 8)
            return;
        const double binHz = proc.dsp.binHz();

        // Spectrum (tilted so typical music looks level) with the reduction hanging from the top.
        juce::Path spectrum, cut;
        bool started = false;
        const int steps = (int) r.getWidth() / 2;
        const float cutRange = 24.0f;
        cut.startNewSubPath (r.getX(), r.getY());
        for (int i = 0; i <= steps; ++i)
        {
            const float x = (float) i / (float) steps * r.getWidth();
            const float f0 = xToFreq (x, r.getWidth()), f1 = xToFreq (x + 2.0f, r.getWidth());
            const int k0 = juce::jlimit (1, bins - 1, (int) (f0 / binHz));
            const int k1 = juce::jlimit (k0 + 1, bins, (int) std::ceil (f1 / binHz));
            float peak = -140.0f, reduction = 0.0f;
            for (int k = k0; k < k1; ++k)
            {
                peak = juce::jmax (peak, mag[(size_t) k]);
                reduction = juce::jmax (reduction, red[(size_t) k]);
            }
            peak += 4.5f * std::log2 (juce::jmax (f0, 20.0f) / 1000.0f);
            const float y = juce::jmap (juce::jlimit (-96.0f, 0.0f, peak), -96.0f, 0.0f, r.getBottom(), r.getY() + 8.0f);
            if (! started)
            {
                spectrum.startNewSubPath (r.getX() + x, y);
                started = true;
            }
            else
                spectrum.lineTo (r.getX() + x, y);
            cut.lineTo (r.getX() + x, r.getY() + juce::jlimit (0.0f, 1.0f, reduction / cutRange) * r.getHeight() * 0.9f);
        }
        spectrum.lineTo (r.getRight(), r.getBottom());
        spectrum.lineTo (r.getX(), r.getBottom());
        spectrum.closeSubPath();
        g.setColour (colours::dim.withAlpha (0.25f));
        g.fillPath (spectrum);

        cut.lineTo (r.getRight(), r.getY());
        cut.closeSubPath();
        g.setColour (accent.withAlpha (0.35f));
        g.fillPath (cut);
        g.setColour (accent);
        g.strokePath (cut, juce::PathStrokeType (1.5f));

        g.setColour (colours::dim);
        g.setFont (font (9.5f, true));
        g.drawText (proc.flag ("delta") ? "DELTA: HEARING ONLY WHAT IS REMOVED" : "REDUCTION", r.reduced (8.0f, 14.0f), juce::Justification::bottomLeft);
        g.setColour (colours::text);
        g.setFont (font (18.0f, true));
        g.drawText (juce::String (proc.dsp.getMaxReductionDb(), 1) + " dB", r.reduced (8.0f, 12.0f), juce::Justification::bottomRight);
    }

private:
    SmoothProcessor& proc;
};

juce::AudioProcessorEditor* SmoothProcessor::createEditor()
{
    LayoutSpec l;
    l.displayHeight = 200;
    l.rows = { {
        { "Reduction", { knob ("depth", "Depth"), knob ("sharpness", "Sharpness"), knob ("selectivity", "Selectivity") } },
        { "Timing", { knob ("attack", "Attack"), knob ("release", "Release") } },
        { "Range", { knob ("low", "Low"), knob ("high", "High") } },
        { "Output", { knob ("mix", "Mix"), knob ("out", "Level", true), toggle ("delta", "Delta") } },
    } };
    return new GittoEditor (*this, std::move (l), std::make_unique<SmoothDisplay> (*this));
}

juce::AudioProcessor* createGittoSmooth() { return new SmoothProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoSmooth(); }
#endif
