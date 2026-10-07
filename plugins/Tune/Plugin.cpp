// Gitto FX Tune - plugin wrapper and interface.
#include "../../dsp/Tune.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class TuneProcessor : public GittoProcessor
{
public:
    TuneProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
    }

    static juce::StringArray keyNames() { return { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }; }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        layout.add (choiceParam ("key", "Key", keyNames(), 0));
        layout.add (choiceParam ("scale", "Scale", { "Chromatic", "Major", "Minor", "Harmonic Minor", "Major Pentatonic", "Minor Pentatonic", "Blues", "Dorian", "Mixolydian" }, 0));
        layout.add (choiceParam ("range", "Voice Range", { "Low", "Mid", "High" }, 1));
        layout.add (floatParam ("retune", "Retune Speed", range (0.0f, 400.0f, 60.0f), 20.0f, Unit::Ms));
        layout.add (floatParam ("humanize", "Humanize", range (0.0f, 1.0f), 0.0f, Unit::Percent));
        layout.add (floatParam ("amount", "Amount", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { "transpose", 1 }, "Transpose", range (-12.0f, 12.0f, 0.0f, 1.0f), 0.0f,
            Attr().withStringFromValueFunction ([] (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " st"; })));
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { "formant", 1 }, "Formant", range (-6.0f, 6.0f), 0.0f,
            Attr().withStringFromValueFunction ([] (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " st"; })));
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { "tuning", 1 }, "Tuning", range (415.0f, 466.0f), 440.0f,
            Attr().withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1) + " Hz"; })));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        layout.add (floatParam ("out", "Output", range (-24.0f, 12.0f), 0.0f, Unit::Db));
        return layout;
    }

    juce::String getProductName() const override { return "Tune"; }
    juce::Colour getAccent() const override { return juce::Colour (0xff59e0e0); }

    gitto::TuneParams readParams() const
    {
        gitto::TuneParams p;
        p.key = choice ("key");
        p.scale = (gitto::TuneScale) choice ("scale");
        p.range = (gitto::TuneRange) choice ("range");
        p.retuneMs = value ("retune");
        p.humanize = value ("humanize");
        p.amount = value ("amount");
        p.transpose = value ("transpose");
        p.formant = value ("formant");
        p.tuningHz = value ("tuning");
        p.mix = value ("mix");
        p.outputDb = value ("out");
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
        {
            dsp.setParams (readParams());
            requestLatency (dsp.getLatencySamples());
        }
        dsp.process (buffer.getWritePointer (0), buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr, buffer.getNumSamples());
    }

    std::vector<Preset> getPresets() const override
    {
        // scale: 0 Chromatic, 1 Major, 2 Minor, 5 Minor Pentatonic. range: 0 Low, 1 Mid, 2 High
        return {
            { "Default", {} },
            { "Natural Touch-Up", { { "retune", 80 }, { "humanize", 0.6f }, { "amount", 0.8f } } },
            { "Pop Vocal", { { "retune", 30 }, { "humanize", 0.3f } } },
            { "Hard Tune", { { "retune", 0 }, { "humanize", 0.0f } } },
            { "Hard Tune A Minor", { { "retune", 0 }, { "key", 9 }, { "scale", 2 } } },
            { "Trap Melody (Minor Pentatonic)", { { "retune", 5 }, { "key", 9 }, { "scale", 5 } } },
            { "Deep Voice", { { "retune", 20 }, { "formant", -3 }, { "range", 0 } } },
            { "Chipmunk", { { "retune", 10 }, { "transpose", 7 }, { "formant", 4 }, { "range", 2 } } },
            { "Octave Down Double", { { "retune", 20 }, { "transpose", -12 }, { "mix", 0.5f }, { "range", 0 } } },
            { "Gentle Bass Tune", { { "retune", 50 }, { "range", 0 }, { "humanize", 0.4f } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Tune dsp;
};

// One octave of keys with the notes in the scale lit, the note being sung, and how
// far it is being moved.
class TuneDisplay : public juce::Component
{
public:
    explicit TuneDisplay (TuneProcessor& p) : proc (p) { history.fill (-1000.0f); }

    void paint (juce::Graphics& g) override
    {
        const juce::Colour accent = proc.getAccent();
        auto area = getLocalBounds().toFloat().reduced (8.0f);
        auto keys = area.removeFromLeft (area.getWidth() * 0.52f);
        area.removeFromLeft (10.0f);

        const auto p = proc.readParams();
        const float detected = proc.dsp.getDetectedMidi(), target = proc.dsp.getTargetMidi();
        const float corr = proc.dsp.getCorrectionCents();
        const int mask = gitto::Tune::scaleMask (p.scale);
        const auto names = TuneProcessor::keyNames();

        // ---- keyboard ----
        g.setColour (colours::background);
        g.fillRoundedRectangle (keys, 4.0f);
        const auto kb = keys.reduced (10.0f, 26.0f).withTrimmedBottom (-10.0f);
        const float keyW = kb.getWidth() / 12.0f;
        const int sung = detected > 0.0f ? ((int) std::lround (detected) % 12 + 12) % 12 : -1;
        const int aimed = target > 0.0f ? ((int) std::lround (target - p.transpose) % 12 + 12) % 12 : -1;
        for (int n = 0; n < 12; ++n)
        {
            const bool allowed = (mask & (1 << (((n - p.key) % 12 + 12) % 12))) != 0;
            const bool black = n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
            auto key = juce::Rectangle<float> (kb.getX() + keyW * (float) n, kb.getY(), keyW, kb.getHeight()).reduced (1.5f, 0.0f);
            juce::Colour fill = allowed ? (black ? colours::panelEdge : colours::track.brighter (0.25f)) : colours::panel;
            if (n == aimed)
                fill = accent;
            g.setColour (fill);
            g.fillRoundedRectangle (key, 3.0f);
            if (n == sung && n != aimed)
            {
                g.setColour (juce::Colour (0xffff8a4c));
                g.drawRoundedRectangle (key.reduced (1.0f), 3.0f, 2.0f);
            }
            g.setColour (n == aimed ? colours::background : (allowed ? colours::text : colours::panelEdge));
            g.setFont (font (10.0f, true));
            g.drawText (names[n], key.removeFromBottom (16.0f), juce::Justification::centred);
        }
        g.setColour (colours::dim);
        g.setFont (font (9.5f, true));
        g.drawText (names[p.key].toUpperCase() + "  |  NOTES IN THE SCALE ARE LIT", keys.reduced (10.0f, 8.0f), juce::Justification::topLeft);

        // ---- correction over time ----
        history[(size_t) writePos] = detected > 0.0f ? corr : -1000.0f;
        writePos = (writePos + 1) % (int) history.size();
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        const float range = 100.0f; // cents
        const float mid = area.getCentreY();
        g.setFont (font (9.5f));
        for (float c : { -50.0f, 50.0f })
        {
            const float y = mid - c / range * (area.getHeight() * 0.5f - 8.0f);
            g.setColour (colours::grid);
            g.fillRect (area.getX(), y, area.getWidth(), 1.0f);
            g.setColour (colours::dim.withAlpha (0.7f));
            g.drawText ((c > 0 ? "+" : "") + juce::String ((int) c), juce::Rectangle<float> (area.getX() + 4.0f, y - 12.0f, 30.0f, 11.0f), juce::Justification::centredLeft);
        }
        g.setColour (colours::panelEdge);
        g.fillRect (area.getX(), mid, area.getWidth(), 1.0f);
        juce::Path path;
        bool drawing = false;
        const int count = (int) history.size();
        for (int i = 0; i < count; ++i)
        {
            const float v = history[(size_t) ((writePos + i) % count)];
            if (v < -900.0f)
            {
                drawing = false;
                continue;
            }
            const float x = area.getX() + area.getWidth() * (float) i / (float) (count - 1);
            const float y = mid - juce::jlimit (-range, range, v) / range * (area.getHeight() * 0.5f - 8.0f);
            if (! drawing)
            {
                path.startNewSubPath (x, y);
                drawing = true;
            }
            else
                path.lineTo (x, y);
        }
        g.setColour (accent);
        g.strokePath (path, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setColour (colours::dim);
        g.setFont (font (9.5f, true));
        g.drawText ("CORRECTION (CENTS)", area.reduced (10.0f, 8.0f), juce::Justification::topLeft);
        g.setColour (colours::text);
        g.setFont (font (18.0f, true));
        juce::String readout ("--");
        if (detected > 0.0f)
        {
            const int note = (int) std::lround (target);
            readout = names[((note % 12) + 12) % 12] + juce::String (note / 12 - 1) + "   " + (corr > 0.0f ? "+" : "") + juce::String (juce::roundToInt (corr)) + " ct";
        }
        g.drawText (readout, area.reduced (10.0f, 6.0f), juce::Justification::bottomRight);
    }

private:
    TuneProcessor& proc;
    std::array<float, 240> history {};
    int writePos = 0;
};

juce::AudioProcessorEditor* TuneProcessor::createEditor()
{
    LayoutSpec l;
    l.displayHeight = 170;
    l.rows = {
        { { "Scale", { chooser ("key", "Key"), chooser ("scale", "Scale"), chooser ("range", "Voice Range") } },
          { "Correction", { knob ("retune", "Retune Speed"), knob ("humanize", "Humanize"), knob ("amount", "Amount") } } },
        { { "Pitch and Voice", { knob ("transpose", "Transpose", true), knob ("formant", "Formant", true), knob ("tuning", "Tuning", true) } },
          { "Output", { knob ("mix", "Mix"), knob ("out", "Level", true) } } },
    };
    return new GittoEditor (*this, std::move (l), std::make_unique<TuneDisplay> (*this));
}

juce::AudioProcessor* createGittoTune() { return new TuneProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoTune(); }
#endif
