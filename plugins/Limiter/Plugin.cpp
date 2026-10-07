// Gitto FX Limiter - plugin wrapper and interface.
#include "../../dsp/Limiter.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class LimiterProcessor : public GittoProcessor
{
public:
    LimiterProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        layout.add (choiceParam ("style", "Style", { "Transparent", "Punchy", "Dynamic", "Aggressive", "Safe" }, 0));
        layout.add (floatParam ("gain", "Gain", range (0.0f, 30.0f), 0.0f, Unit::Db));
        layout.add (floatParam ("ceiling", "Ceiling", range (-12.0f, 0.0f), -1.0f, Unit::Db));
        layout.add (floatParam ("lookahead", "Lookahead", range (0.1f, 10.0f, 2.0f), 3.0f, Unit::Ms));
        layout.add (floatParam ("release", "Release", range (1.0f, 1000.0f, 100.0f), 120.0f, Unit::Ms));
        layout.add (floatParam ("link", "Stereo Link", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        layout.add (boolParam ("truepeak", "True Peak", true));
        layout.add (choiceParam ("dither", "Dither", { "Off", "16-bit", "24-bit" }, 0));
        return layout;
    }

    juce::String getProductName() const override { return "Limiter"; }
    juce::Colour getAccent() const override { return juce::Colour (0xffff5f6d); }

    gitto::LimiterParams readParams() const
    {
        gitto::LimiterParams p;
        p.style = (gitto::LimiterStyle) choice ("style");
        p.gainDb = value ("gain");
        p.ceilingDb = value ("ceiling");
        p.lookaheadMs = value ("lookahead");
        p.releaseMs = value ("release");
        p.stereoLink = value ("link");
        p.truePeak = flag ("truepeak");
        const int d = choice ("dither");
        p.ditherBits = d == 1 ? 16 : (d == 2 ? 24 : 0);
        return p;
    }

    void prepareToPlay (double sr, int) override
    {
        limiter.setParams (readParams());
        limiter.prepare (sr);
        setLatencySamples (limiter.getLatencySamples());
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        if (consumeDirty())
        {
            limiter.setParams (readParams());
            requestLatency (limiter.getLatencySamples());
        }
        float* l = buffer.getWritePointer (0);
        float* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
        limiter.process (l, r, buffer.getNumSamples());
    }

    std::vector<Preset> getPresets() const override
    {
        // style: 0 Transparent, 1 Punchy, 2 Dynamic, 3 Aggressive, 4 Safe
        return {
            { "Default", {} },
            { "Streaming Master (-1 dBTP)", { { "style", 0 }, { "gain", 4 }, { "ceiling", -1 }, { "lookahead", 3 }, { "release", 150 } } },
            { "Loud Beat Master", { { "style", 2 }, { "gain", 8 }, { "ceiling", -0.3f }, { "lookahead", 2 }, { "release", 80 } } },
            { "Trap Slam", { { "style", 3 }, { "gain", 10 }, { "ceiling", -0.3f }, { "lookahead", 1.5f }, { "release", 40 } } },
            { "Punchy Drums", { { "style", 1 }, { "gain", 5 }, { "ceiling", -0.5f }, { "lookahead", 2 }, { "release", 60 } } },
            { "Safe Game Audio", { { "style", 4 }, { "gain", 3 }, { "ceiling", -2 }, { "lookahead", 5 }, { "release", 250 } } },
            { "Peak Catcher", { { "style", 0 }, { "gain", 0 }, { "ceiling", -0.1f }, { "lookahead", 1 }, { "release", 50 } } },
            { "CD Master 16-bit", { { "style", 0 }, { "gain", 5 }, { "ceiling", -0.3f }, { "dither", 1 } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Limiter limiter;
};

//==============================================================================
class LimiterDisplay : public juce::Component
{
public:
    explicit LimiterDisplay (LimiterProcessor& p) : proc (p)
    {
        history.fill (0.0f);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void mouseDown (const juce::MouseEvent&) override { proc.limiter.resetMeters(); }

    void paint (juce::Graphics& g) override
    {
        const juce::Colour accent = proc.getAccent();
        auto area = getLocalBounds().toFloat().reduced (8.0f);
        auto readouts = area.removeFromRight (236.0f);
        area.removeFromRight (10.0f);

        // ---- gain reduction history ----
        const float grDb = proc.limiter.getGainReductionDb();
        history[(size_t) writePos] = grDb;
        writePos = (writePos + 1) % (int) history.size();

        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        const float range = 18.0f;
        g.setFont (font (9.5f));
        for (float db : { 3.0f, 6.0f, 9.0f, 12.0f, 15.0f })
        {
            const float y = area.getY() + area.getHeight() * db / range;
            g.setColour (colours::grid);
            g.fillRect (area.getX(), y, area.getWidth(), 1.0f);
            g.setColour (colours::dim.withAlpha (0.7f));
            g.drawText ("-" + juce::String ((int) db), juce::Rectangle<float> (area.getX() + 4.0f, y - 12.0f, 30.0f, 11.0f), juce::Justification::centredLeft);
        }
        juce::Path gr;
        gr.startNewSubPath (area.getX(), area.getY());
        const int count = (int) history.size();
        for (int i = 0; i < count; ++i)
        {
            const float v = history[(size_t) ((writePos + i) % count)];
            gr.lineTo (area.getX() + area.getWidth() * (float) i / (float) (count - 1),
                       area.getY() + area.getHeight() * juce::jlimit (0.0f, 1.0f, -v / range));
        }
        gr.lineTo (area.getRight(), area.getY());
        gr.closeSubPath();
        g.setColour (accent.withAlpha (0.3f));
        g.fillPath (gr);
        g.setColour (accent);
        g.strokePath (gr, juce::PathStrokeType (1.3f));
        g.setColour (colours::dim);
        g.setFont (font (9.5f, true));
        g.drawText ("GAIN REDUCTION", area.reduced (10.0f, 8.0f).withTrimmedBottom (28.0f), juce::Justification::bottomRight);
        g.setColour (colours::text);
        g.setFont (font (20.0f, true));
        g.drawText (dbText (grDb), area.reduced (10.0f, 8.0f), juce::Justification::bottomRight);

        // ---- loudness and true-peak readouts ----
        auto& lm = proc.limiter.loudness;
        struct Item { const char* label; float value; const char* unit; };
        const float tp = gitto::gainToDb (proc.limiter.getMaxTruePeak());
        const Item items[] = { { "INTEGRATED", lm.getIntegrated(), "LUFS" },
                               { "SHORT-TERM", lm.getShortTerm(), "LUFS" },
                               { "MOMENTARY", lm.getMomentary(), "LUFS" },
                               { "MAX TRUE PEAK", tp, "dBTP" } };
        const float cellH = readouts.getHeight() / 2.0f, cellW = readouts.getWidth() / 2.0f;
        for (int i = 0; i < 4; ++i)
        {
            auto cell = juce::Rectangle<float> (readouts.getX() + (float) (i % 2) * cellW, readouts.getY() + (float) (i / 2) * cellH, cellW, cellH).reduced (3.0f);
            g.setColour (colours::background);
            g.fillRoundedRectangle (cell, 4.0f);
            g.setColour (colours::dim);
            g.setFont (font (9.5f, true));
            g.drawText (items[i].label, cell.reduced (8.0f, 6.0f), juce::Justification::topLeft);
            g.setColour (i == 0 ? accent : colours::text);
            g.setFont (font (22.0f, true));
            const juce::String text = items[i].value < -90.0f ? juce::String ("--") : juce::String (items[i].value, 1);
            g.drawText (text, cell.reduced (8.0f, 4.0f).withTrimmedTop (10.0f), juce::Justification::centredLeft);
            g.setColour (colours::dim);
            g.setFont (font (9.5f));
            g.drawText (items[i].unit, cell.reduced (8.0f, 6.0f), juce::Justification::bottomRight);
        }
        g.setColour (colours::dim.withAlpha (0.8f));
        g.setFont (font (9.0f));
        g.drawText ("Click to reset the meters", area.reduced (10.0f, 8.0f), juce::Justification::bottomLeft);
    }

private:
    LimiterProcessor& proc;
    std::array<float, 300> history {};
    int writePos = 0;
};

juce::AudioProcessorEditor* LimiterProcessor::createEditor()
{
    LayoutSpec l;
    l.displayHeight = 190;
    l.rows = { {
        { "Style", { chooser ("style", "Algorithm") } },
        { "Level", { knob ("gain", "Gain"), knob ("ceiling", "Ceiling") } },
        { "Time", { knob ("lookahead", "Lookahead"), knob ("release", "Release") } },
        { "Output", { knob ("link", "Stereo Link"), toggle ("truepeak", "True Peak"), chooser ("dither", "Dither") } },
    } };
    l.meters = {
        { "IN", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (limiter.getInputLevel()); } },
        { "GR", MeterSpec::Kind::Reduction, [this] { return limiter.getGainReductionDb(); } },
        { "OUT", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (limiter.getOutputLevel()); } },
    };
    return new GittoEditor (*this, std::move (l), std::make_unique<LimiterDisplay> (*this));
}

juce::AudioProcessor* createGittoLimiter() { return new LimiterProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoLimiter(); }
#endif
