// Gitto FX Compressor - plugin wrapper and interface.
#include "../../dsp/Compressor.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class CompProcessor : public GittoProcessor
{
public:
    CompProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                                           .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false),
                          createLayout())
    {
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        layout.add (choiceParam ("style", "Style", { "Clean", "Punch", "Glue", "Opto", "Vari-Mu", "FET" }, 0));
        layout.add (floatParam ("threshold", "Threshold", range (-60.0f, 0.0f), -18.0f, Unit::Db));
        layout.add (floatParam ("ratio", "Ratio", range (1.0f, 20.0f, 4.0f), 4.0f, Unit::Ratio));
        layout.add (floatParam ("knee", "Knee", range (0.0f, 36.0f), 6.0f, Unit::DbAmount));
        layout.add (floatParam ("range", "Range", range (0.0f, 60.0f), 60.0f, Unit::DbAmount));
        layout.add (floatParam ("attack", "Attack", range (0.01f, 250.0f, 10.0f), 10.0f, Unit::Ms));
        layout.add (floatParam ("release", "Release", range (5.0f, 2500.0f, 150.0f), 120.0f, Unit::Ms));
        layout.add (boolParam ("autorelease", "Auto Release", false));
        layout.add (floatParam ("hold", "Hold", range (0.0f, 500.0f, 50.0f), 0.0f, Unit::Ms));
        layout.add (floatParam ("lookahead", "Lookahead", range (0.0f, 20.0f), 0.0f, Unit::Ms));
        layout.add (floatParam ("schpf", "Sidechain High-Pass", range (20.0f, 500.0f, 100.0f), 20.0f, Unit::Hz));
        layout.add (boolParam ("extsc", "External Sidechain", false));
        layout.add (floatParam ("link", "Stereo Link", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        layout.add (floatParam ("makeup", "Makeup Gain", range (-12.0f, 36.0f), 0.0f, Unit::Db));
        layout.add (boolParam ("automakeup", "Auto Makeup", false));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        return layout;
    }

    juce::String getProductName() const override { return "Compressor"; }
    juce::Colour getAccent() const override { return juce::Colour (0xffffa63d); }

    gitto::CompParams readParams() const
    {
        gitto::CompParams p;
        p.style = (gitto::CompStyle) choice ("style");
        p.thresholdDb = value ("threshold");
        p.ratio = value ("ratio");
        p.kneeDb = value ("knee");
        p.rangeDb = value ("range");
        p.attackMs = value ("attack");
        p.releaseMs = value ("release");
        p.autoRelease = flag ("autorelease");
        p.holdMs = value ("hold");
        p.lookaheadMs = value ("lookahead");
        p.scHpfHz = value ("schpf");
        p.externalSidechain = flag ("extsc");
        p.stereoLink = value ("link");
        p.makeupDb = value ("makeup");
        p.autoMakeup = flag ("automakeup");
        p.mix = value ("mix");
        return p;
    }

    void pushParams()
    {
        comp.setParams (readParams());
        requestLatency (comp.getLatencySamples());
    }

    void prepareToPlay (double sr, int) override
    {
        comp.setParams (readParams());
        comp.prepare (sr);
        setLatencySamples (comp.getLatencySamples());
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        if (consumeDirty())
            pushParams();

        auto mainBus = getBusBuffer (buffer, true, 0);
        const int n = buffer.getNumSamples();
        float* l = mainBus.getWritePointer (0);
        float* r = mainBus.getNumChannels() > 1 ? mainBus.getWritePointer (1) : nullptr;

        const float* scL = nullptr;
        const float* scR = nullptr;
        if (getBusCount (true) > 1)
        {
            auto sc = getBusBuffer (buffer, true, 1);
            if (sc.getNumChannels() > 0)
            {
                scL = sc.getReadPointer (0);
                scR = sc.getNumChannels() > 1 ? sc.getReadPointer (1) : scL;
            }
        }
        comp.process (l, r, scL, scR, n);
    }

    std::vector<Preset> getPresets() const override
    {
        // style: 0 Clean, 1 Punch, 2 Glue, 3 Opto, 4 Vari-Mu, 5 FET
        return {
            { "Default", {} },
            { "Vocal Leveller", { { "style", 3 }, { "threshold", -24 }, { "ratio", 3 }, { "knee", 12 }, { "attack", 8 }, { "release", 180 }, { "makeup", 2.5f } } },
            { "Rap Vocal Up Front", { { "style", 5 }, { "threshold", -17 }, { "ratio", 8 }, { "knee", 3 }, { "attack", 3 }, { "release", 80 }, { "makeup", 7 } } },
            { "Drum Bus Glue", { { "style", 2 }, { "threshold", -16 }, { "ratio", 4 }, { "knee", 6 }, { "attack", 30 }, { "release", 100 }, { "autorelease", 1 }, { "schpf", 90 }, { "makeup", 2 } } },
            { "Kick Punch", { { "style", 1 }, { "threshold", -18 }, { "ratio", 5 }, { "knee", 2 }, { "attack", 25 }, { "release", 90 }, { "makeup", 3 } } },
            { "808 Control", { { "style", 0 }, { "threshold", -14 }, { "ratio", 6 }, { "knee", 6 }, { "attack", 15 }, { "release", 220 }, { "makeup", 2 } } },
            { "Parallel Smash", { { "style", 5 }, { "threshold", -36 }, { "ratio", 20 }, { "knee", 0 }, { "attack", 1 }, { "release", 60 }, { "makeup", 22 }, { "mix", 0.4f } } },
            { "Mix Bus Warmth", { { "style", 4 }, { "threshold", -14 }, { "ratio", 2 }, { "knee", 18 }, { "attack", 30 }, { "release", 300 }, { "schpf", 60 }, { "makeup", 1 } } },
            { "Sidechain Pump", { { "style", 0 }, { "threshold", -30 }, { "ratio", 10 }, { "knee", 0 }, { "attack", 1 }, { "release", 180 }, { "extsc", 1 } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Compressor comp;
};

//==============================================================================
class CompDisplay : public juce::Component
{
public:
    explicit CompDisplay (CompProcessor& p) : proc (p) { history.fill (0.0f); }

    void paint (juce::Graphics& g) override
    {
        const juce::Colour accent = proc.getAccent();
        auto area = getLocalBounds().toFloat().reduced (8.0f);
        const float side = juce::jmin (area.getHeight(), area.getWidth() * 0.4f);
        auto curveArea = area.removeFromLeft (side);
        area.removeFromLeft (10.0f);

        // ---- transfer curve: input level across, output level up ----
        const float lo = -60.0f, hi = 0.0f;
        auto toX = [&] (float db) { return curveArea.getX() + (db - lo) / (hi - lo) * curveArea.getWidth(); };
        auto toY = [&] (float db) { return curveArea.getBottom() - (juce::jlimit (lo, hi, db) - lo) / (hi - lo) * curveArea.getHeight(); };

        g.setColour (colours::background);
        g.fillRoundedRectangle (curveArea, 4.0f);
        g.setColour (colours::grid);
        for (float db = -48.0f; db < 0.0f; db += 12.0f)
        {
            g.fillRect (toX (db), curveArea.getY(), 1.0f, curveArea.getHeight());
            g.fillRect (curveArea.getX(), toY (db), curveArea.getWidth(), 1.0f);
        }
        g.setColour (colours::panelEdge);
        g.drawLine (toX (lo), toY (lo), toX (hi), toY (hi), 1.0f);

        model.setParams (proc.readParams());
        juce::Path curve;
        for (int i = 0; i <= 120; ++i)
        {
            const float in = lo + (hi - lo) * (float) i / 120.0f;
            const float out = in + model.gainReductionFor (in);
            if (i == 0) curve.startNewSubPath (toX (in), toY (out));
            else curve.lineTo (toX (in), toY (out));
        }
        g.setColour (accent);
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const float inDb = gitto::gainToDb (proc.comp.getInputLevel());
        const float grDb = proc.comp.getGainReductionDb();
        if (inDb > lo)
        {
            const float x = toX (juce::jmin (inDb, hi)), y = toY (inDb + grDb);
            g.setColour (colours::text);
            g.fillEllipse (x - 4.0f, y - 4.0f, 8.0f, 8.0f);
        }
        g.setColour (colours::dim);
        g.setFont (font (9.5f));
        g.drawText ("IN", curveArea.reduced (4.0f).removeFromBottom (11.0f), juce::Justification::centredRight);
        g.drawText ("OUT", curveArea.reduced (4.0f).removeFromTop (11.0f), juce::Justification::centredLeft);

        // ---- gain reduction history, newest on the right ----
        history[(size_t) writePos] = grDb;
        writePos = (writePos + 1) % (int) history.size();

        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        const float range = 24.0f;
        g.setFont (font (9.5f));
        for (float db : { 6.0f, 12.0f, 18.0f })
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
            const float x = area.getX() + area.getWidth() * (float) i / (float) (count - 1);
            gr.lineTo (x, area.getY() + area.getHeight() * juce::jlimit (0.0f, 1.0f, -v / range));
        }
        gr.lineTo (area.getRight(), area.getY());
        gr.closeSubPath();
        g.setColour (accent.withAlpha (0.3f));
        g.fillPath (gr);
        g.setColour (accent);
        g.strokePath (gr, juce::PathStrokeType (1.3f));

        g.setColour (colours::text);
        g.setFont (font (20.0f, true));
        g.drawText (dbText (grDb), area.reduced (10.0f, 8.0f), juce::Justification::bottomRight);
        g.setColour (colours::dim);
        g.setFont (font (9.5f, true));
        g.drawText ("GAIN REDUCTION", area.reduced (10.0f, 8.0f), juce::Justification::bottomLeft);
    }

private:
    CompProcessor& proc;
    gitto::Compressor model; // used only for its static curve
    std::array<float, 240> history {};
    int writePos = 0;
};

juce::AudioProcessorEditor* CompProcessor::createEditor()
{
    LayoutSpec l;
    l.displayHeight = 190;
    l.rows = {
        { { "Style", { chooser ("style", "Circuit") } },
          { "Dynamics", { knob ("threshold", "Threshold"), knob ("ratio", "Ratio"), knob ("knee", "Knee"), knob ("range", "Range") } },
          { "Time", { knob ("attack", "Attack"), knob ("release", "Release"), toggle ("autorelease", "Auto Rel"), knob ("hold", "Hold"), knob ("lookahead", "Lookahead") } } },
        { { "Sidechain", { knob ("schpf", "High-Pass"), toggle ("extsc", "External"), knob ("link", "Stereo Link") } },
          { "Output", { knob ("makeup", "Makeup", true), toggle ("automakeup", "Auto Gain"), knob ("mix", "Mix") } } },
    };
    l.meters = {
        { "IN", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (comp.getInputLevel()); } },
        { "GR", MeterSpec::Kind::Reduction, [this] { return comp.getGainReductionDb(); } },
        { "OUT", MeterSpec::Kind::Level, [this] { return gitto::gainToDb (comp.getOutputLevel()); } },
    };
    auto* editor = new GittoEditor (*this, std::move (l), std::make_unique<CompDisplay> (*this));
    editor->onTimer = [this, editor]
    {
        const auto style = (gitto::CompStyle) choice ("style");
        // Opto always uses its own program-dependent release.
        editor->setControlEnabled ("autorelease", style != gitto::CompStyle::Opto);
    };
    return editor;
}

juce::AudioProcessor* createGittoCompressor() { return new CompProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoCompressor(); }
#endif
