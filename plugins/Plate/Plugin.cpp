// Gitto FX Plate - plugin wrapper and interface.
#include "../../dsp/Plate.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class PlateProcessor : public GittoProcessor
{
public:
    PlateProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
        allowMonoToStereo = true;
    }

    static juce::StringArray typeNames() { return { "Classic", "Bright", "Dark", "Dense", "Thin", "Vintage" }; }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        layout.add (choiceParam ("type", "Plate", typeNames(), 0));
        layout.add (floatParam ("decay", "Decay", range (0.3f, 20.0f, 2.5f), 2.0f, Unit::Sec));
        layout.add (floatParam ("predelay", "Pre-Delay", range (0.0f, 250.0f, 40.0f), 10.0f, Unit::Ms));
        layout.add (floatParam ("size", "Size", range (0.0f, 1.0f), 0.5f, Unit::Percent));
        layout.add (floatParam ("damp", "Damping", range (1000.0f, 16000.0f, 5000.0f), 7000.0f, Unit::Hz));
        layout.add (floatParam ("lowcut", "Low Cut", range (20.0f, 500.0f, 120.0f), 80.0f, Unit::Hz));
        layout.add (floatParam ("mod", "Modulation", range (0.0f, 1.0f), 0.3f, Unit::Percent));
        layout.add (floatParam ("width", "Width", range (0.0f, 2.0f), 1.0f, Unit::Percent));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 0.3f, Unit::Percent));
        return layout;
    }

    juce::String getProductName() const override { return "Plate"; }
    juce::Colour getAccent() const override { return juce::Colour (0xffb7c4d6); }
    double getTailLengthSeconds() const override { return (double) value ("decay") + 0.5; }

    gitto::PlateParams readParams() const
    {
        gitto::PlateParams p;
        p.type = (gitto::PlateType) choice ("type");
        p.decaySec = value ("decay");
        p.predelayMs = value ("predelay");
        p.size = value ("size");
        p.dampHz = value ("damp");
        p.lowCutHz = value ("lowcut");
        p.modDepth = value ("mod");
        p.width = value ("width");
        p.mix = value ("mix");
        return p;
    }

    void prepareToPlay (double sr, int) override
    {
        dsp.setParams (readParams());
        dsp.prepare (sr);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        if (consumeDirty())
            dsp.setParams (readParams());
        const int n = buffer.getNumSamples();
        float* l = buffer.getWritePointer (0);
        float* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
        if (r != nullptr && getTotalNumInputChannels() < 2)
            juce::FloatVectorOperations::copy (r, l, n);
        dsp.process (l, r, n);
    }

    std::vector<Preset> getPresets() const override
    {
        // type: 0 Classic, 1 Bright, 2 Dark, 3 Dense, 4 Thin, 5 Vintage
        return {
            { "Default", {} },
            { "Vocal Plate", { { "type", 0 }, { "decay", 1.8f }, { "predelay", 40 }, { "lowcut", 180 }, { "mix", 0.22f } } },
            { "Bright Vocal Sheen", { { "type", 1 }, { "decay", 1.4f }, { "predelay", 30 }, { "lowcut", 250 }, { "damp", 12000 }, { "mix", 0.2f } } },
            { "Snare Plate", { { "type", 3 }, { "decay", 1.1f }, { "predelay", 5 }, { "lowcut", 200 }, { "mix", 0.3f } } },
            { "Short Drum Plate", { { "type", 4 }, { "decay", 0.6f }, { "predelay", 0 }, { "size", 0.3f }, { "mix", 0.25f } } },
            { "Dark Ballad", { { "type", 2 }, { "decay", 3.2f }, { "predelay", 60 }, { "damp", 4000 }, { "mix", 0.3f } } },
            { "70s Vintage", { { "type", 5 }, { "decay", 2.4f }, { "predelay", 20 }, { "mod", 0.5f }, { "mix", 0.3f } } },
            { "Long Shimmer", { { "type", 1 }, { "decay", 6.0f }, { "predelay", 50 }, { "size", 0.9f }, { "damp", 14000 }, { "lowcut", 300 }, { "mix", 0.35f } } },
            { "Wide Keys", { { "type", 0 }, { "decay", 2.8f }, { "width", 1.5f }, { "mod", 0.5f }, { "mix", 0.3f } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Plate dsp;
};

// Decay over time: how long the body of the plate rings, and how much sooner the
// damped top end dies away.
class PlateDisplay : public juce::Component
{
public:
    explicit PlateDisplay (PlateProcessor& p) : proc (p) {}

    void paint (juce::Graphics& g) override
    {
        const juce::Colour accent = proc.getAccent();
        const auto area = getLocalBounds().toFloat().reduced (8.0f);
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        const auto r = area.reduced (12.0f, 8.0f);
        const auto p = proc.readParams();
        const auto& td = gitto::Plate::typeData (p.type);

        // Decay envelope over time: one curve for the body, one for the damped top end.
        const float span = juce::jlimit (0.5f, 24.0f, p.decaySec * 1.15f + p.predelayMs * 0.001f);
        auto toX = [&] (float seconds) { return r.getX() + juce::jlimit (0.0f, 1.0f, seconds / span) * r.getWidth(); };
        auto toY = [&] (float db) { return r.getY() + 18.0f + juce::jlimit (0.0f, 1.0f, -db / 60.0f) * (r.getHeight() - 18.0f); };
        g.setFont (font (9.5f));
        const float step = span > 12.0f ? 5.0f : (span > 5.0f ? 2.0f : (span > 2.0f ? 1.0f : 0.5f));
        for (float t = step; t < span; t += step)
        {
            g.setColour (colours::grid);
            g.fillRect (toX (t), r.getY() + 18.0f, 1.0f, r.getHeight() - 18.0f);
            g.setColour (colours::dim.withAlpha (0.7f));
            g.drawText (juce::String (t, step < 1.0f ? 1 : 0) + " s", juce::Rectangle<float> (toX (t) + 3.0f, r.getBottom() - 12.0f, 40.0f, 11.0f), juce::Justification::centredLeft);
        }
        const float pre = p.predelayMs * 0.001f;
        g.setColour (colours::text);
        g.fillRect (toX (0.0f), toY (0.0f), 2.0f, r.getBottom() - toY (0.0f));

        // Damping shortens the top: estimate how much from the damping frequency.
        const float highRatio = juce::jlimit (0.15f, 1.0f, 0.25f + 0.75f * (p.dampHz * td.brightness - 1000.0f) / 15000.0f);
        auto slope = [&] (float rt, juce::Colour c, float thickness, bool fill)
        {
            juce::Path path;
            path.startNewSubPath (toX (pre), toY (-8.0f));
            path.lineTo (toX (pre + rt), toY (-60.0f));
            if (fill)
            {
                juce::Path f (path);
                f.lineTo (toX (pre), toY (-60.0f));
                f.closeSubPath();
                g.setColour (c.withAlpha (0.14f));
                g.fillPath (f);
            }
            g.setColour (c);
            g.strokePath (path, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };
        slope (p.decaySec * highRatio, juce::Colour (0xff4dabf7), 1.3f, false);
        slope (p.decaySec, accent, 2.2f, true);

        g.setColour (colours::text);
        g.setFont (font (13.0f, true));
        g.drawText (PlateProcessor::typeNames()[(int) p.type].toUpperCase() + " PLATE", r.withHeight (16.0f), juce::Justification::centredLeft);
        g.setColour (colours::dim);
        g.setFont (font (9.5f, true));
        g.drawText ("BODY  /  TOP END", r.withHeight (16.0f), juce::Justification::centredRight);
    }

private:
    PlateProcessor& proc;
};

juce::AudioProcessorEditor* PlateProcessor::createEditor()
{
    LayoutSpec l;
    l.displayHeight = 150;
    l.rows = {
        { { "Plate", { chooser ("type", "Type"), knob ("decay", "Decay"), knob ("size", "Size"), knob ("predelay", "Pre-Delay") } },
          { "Tone", { knob ("damp", "Damping"), knob ("lowcut", "Low Cut") } } },
        { { "Movement", { knob ("mod", "Modulation") } },
          { "Output", { knob ("width", "Width"), knob ("mix", "Mix") } } },
    };
    return new GittoEditor (*this, std::move (l), std::make_unique<PlateDisplay> (*this));
}

juce::AudioProcessor* createGittoPlate() { return new PlateProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoPlate(); }
#endif
