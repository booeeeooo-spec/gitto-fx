// Gitto FX De-Esser - plugin wrapper and interface.
#include "../../dsp/DeEsser.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class DeEsserProcessor : public GittoProcessor
{
public:
    DeEsserProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        layout.add (choiceParam ("mode", "Detection", { "Vocal", "Wide" }, 0));
        layout.add (choiceParam ("band", "Reduction", { "Wide-Band", "Split-Band" }, 1));
        layout.add (floatParam ("threshold", "Threshold", range (-60.0f, 0.0f), -30.0f, Unit::Db));
        layout.add (floatParam ("range", "Range", range (0.0f, 24.0f), 9.0f, Unit::DbAmount));
        layout.add (floatParam ("freq", "Frequency", range (2000.0f, 12000.0f, 5500.0f), 5500.0f, Unit::Hz));
        layout.add (floatParam ("top", "Top", range (4000.0f, 20000.0f, 12000.0f), 14000.0f, Unit::Hz));
        layout.add (floatParam ("release", "Release", range (10.0f, 300.0f, 80.0f), 60.0f, Unit::Ms));
        layout.add (floatParam ("lookahead", "Lookahead", range (0.0f, 15.0f), 0.0f, Unit::Ms));
        layout.add (floatParam ("link", "Stereo Link", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        layout.add (boolParam ("listen", "Listen", false));
        return layout;
    }

    juce::String getProductName() const override { return "De-Esser"; }
    juce::Colour getAccent() const override { return juce::Colour (0xffa5e06b); }

    gitto::DeEsserParams readParams() const
    {
        gitto::DeEsserParams p;
        p.mode = (gitto::DeEssMode) choice ("mode");
        p.band = (gitto::DeEssBand) choice ("band");
        p.thresholdDb = value ("threshold");
        p.rangeDb = value ("range");
        p.freqHz = value ("freq");
        p.topHz = value ("top");
        p.releaseMs = value ("release");
        p.lookaheadMs = value ("lookahead");
        p.stereoLink = value ("link");
        p.listen = flag ("listen");
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
        {
            dsp.setParams (readParams());
            requestLatency (dsp.getLatencySamples());
        }
        dsp.process (buffer.getWritePointer (0), buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr, buffer.getNumSamples());
    }

    std::vector<Preset> getPresets() const override
    {
        // mode: 0 Vocal, 1 Wide. band: 0 Wide-Band, 1 Split-Band
        return {
            { "Default", {} },
            { "Female Vocal", { { "freq", 6500 }, { "threshold", -30 }, { "range", 8 } } },
            { "Male Vocal", { { "freq", 4800 }, { "threshold", -30 }, { "range", 8 } } },
            { "Rap Vocal Tight", { { "freq", 5500 }, { "threshold", -34 }, { "range", 12 }, { "release", 40 }, { "lookahead", 2 } } },
            { "Gentle Touch", { { "freq", 6000 }, { "threshold", -26 }, { "range", 4 } } },
            { "Heavy Lisp Fix", { { "freq", 5000 }, { "threshold", -36 }, { "range", 16 }, { "band", 1 }, { "lookahead", 3 } } },
            { "Wide-Band Classic", { { "band", 0 }, { "freq", 5500 }, { "threshold", -30 }, { "range", 6 } } },
            { "Hi-Hat Tamer", { { "mode", 1 }, { "freq", 7000 }, { "threshold", -28 }, { "range", 8 }, { "release", 30 } } },
            { "Mix Bus Harshness", { { "mode", 1 }, { "freq", 4000 }, { "top", 9000 }, { "threshold", -24 }, { "range", 4 }, { "release", 80 } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::DeEsser dsp;
    double sampleRate = 48000.0;
};

class DeEsserDisplay : public juce::Component
{
public:
    explicit DeEsserDisplay (DeEsserProcessor& p) : proc (p) { history.fill (0.0f); }

    void paint (juce::Graphics& g) override
    {
        const juce::Colour accent = proc.getAccent();
        auto area = getLocalBounds().toFloat().reduced (8.0f);
        auto left = area.removeFromLeft (area.getWidth() * 0.45f);
        area.removeFromLeft (10.0f);
        const auto p = proc.readParams();

        // ---- what the detector listens to ----
        g.setColour (colours::background);
        g.fillRoundedRectangle (left, 4.0f);
        const auto plot = left.reduced (4.0f);
        drawFrequencyGrid (g, plot);
        const float x0 = plot.getX() + freqToX (p.freqHz, plot.getWidth());
        const float x1 = plot.getX() + freqToX (juce::jmax (p.topHz, p.freqHz * 1.2f), plot.getWidth());
        g.setColour (accent.withAlpha (0.2f));
        g.fillRect (x0, plot.getY(), x1 - x0, plot.getHeight());
        g.setColour (accent);
        g.fillRect (x0 - 0.75f, plot.getY(), 1.5f, plot.getHeight());
        g.fillRect (x1 - 0.75f, plot.getY(), 1.5f, plot.getHeight());
        if (p.band == gitto::DeEssBand::SplitBand)
        {
            // Only the part above the frequency is turned down.
            const float depth = juce::jlimit (0.0f, 1.0f, -proc.dsp.getGainReductionDb() / 24.0f) * plot.getHeight() * 0.8f;
            g.setColour (juce::Colour (0xffff8a4c).withAlpha (0.55f));
            g.fillRect (x0, plot.getY(), plot.getRight() - x0, depth);
        }
        g.setColour (colours::dim);
        g.setFont (font (9.5f, true));
        g.drawText ("DETECTION BAND", left.reduced (10.0f, 8.0f), juce::Justification::topLeft);

        // ---- reduction over time ----
        const float gr = proc.dsp.getGainReductionDb();
        history[(size_t) writePos] = gr;
        writePos = (writePos + 1) % (int) history.size();
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        const float range = 18.0f;
        g.setFont (font (9.5f));
        for (float db : { 6.0f, 12.0f })
        {
            const float y = area.getY() + area.getHeight() * db / range;
            g.setColour (colours::grid);
            g.fillRect (area.getX(), y, area.getWidth(), 1.0f);
            g.setColour (colours::dim.withAlpha (0.7f));
            g.drawText ("-" + juce::String ((int) db), juce::Rectangle<float> (area.getX() + 4.0f, y - 12.0f, 30.0f, 11.0f), juce::Justification::centredLeft);
        }
        juce::Path path;
        path.startNewSubPath (area.getX(), area.getY());
        const int count = (int) history.size();
        for (int i = 0; i < count; ++i)
            path.lineTo (area.getX() + area.getWidth() * (float) i / (float) (count - 1),
                         area.getY() + area.getHeight() * juce::jlimit (0.0f, 1.0f, -history[(size_t) ((writePos + i) % count)] / range));
        path.lineTo (area.getRight(), area.getY());
        path.closeSubPath();
        g.setColour (accent.withAlpha (0.3f));
        g.fillPath (path);
        g.setColour (accent);
        g.strokePath (path, juce::PathStrokeType (1.3f));
        g.setColour (colours::dim);
        g.setFont (font (9.5f, true));
        g.drawText ("REDUCTION", area.reduced (10.0f, 8.0f), juce::Justification::bottomLeft);
        g.setColour (colours::text);
        g.setFont (font (18.0f, true));
        g.drawText (dbText (gr), area.reduced (10.0f, 6.0f), juce::Justification::bottomRight);
    }

private:
    DeEsserProcessor& proc;
    std::array<float, 240> history {};
    int writePos = 0;
};

juce::AudioProcessorEditor* DeEsserProcessor::createEditor()
{
    LayoutSpec l;
    l.displayHeight = 160;
    l.rows = {
        { { "Mode", { chooser ("mode", "Detection"), chooser ("band", "Reduction") } },
          { "Amount", { knob ("threshold", "Threshold"), knob ("range", "Range") } },
          { "Band", { knob ("freq", "Frequency"), knob ("top", "Top") } } },
        { { "Timing", { knob ("release", "Release"), knob ("lookahead", "Lookahead") } },
          { "Channels", { knob ("link", "Stereo Link") } },
          { "Monitor", { toggle ("listen", "Listen") } } },
    };
    return new GittoEditor (*this, std::move (l), std::make_unique<DeEsserDisplay> (*this));
}

juce::AudioProcessor* createGittoDeEsser() { return new DeEsserProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoDeEsser(); }
#endif
