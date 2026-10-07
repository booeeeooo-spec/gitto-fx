// Gitto FX Shaper - plugin wrapper and interface.
#include "../../dsp/Shaper.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class ShaperProcessor : public GittoProcessor
{
public:
    using Nodes = std::vector<juce::Point<float>>;
    static constexpr int kMaxNodes = 32;

    ShaperProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
        setCustomNodes (builtInNodes (1));
    }

    static juce::StringArray shapeNames()
    {
        juce::StringArray names { "Own Curve" };
        for (int i = 0; i < gitto::Shaper::kNumShapes; ++i)
            names.add (gitto::Shaper::builtIn (i).name);
        return names;
    }

    static Nodes builtInNodes (int shapeParam)
    {
        const auto& s = gitto::Shaper::builtIn (juce::jmax (0, shapeParam - 1));
        Nodes nodes;
        for (int i = 0; i < s.count; ++i)
            nodes.push_back ({ s.x[i], s.y[i] });
        return nodes;
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        layout.add (choiceParam ("shape", "Shape", shapeNames(), 1));
        layout.add (choiceParam ("length", "Length", { "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars" }, 2));
        layout.add (choiceParam ("trigger", "Trigger", { "Song Position", "Free Running", "Audio Hit" }, 0));
        layout.add (floatParam ("smooth", "Smooth", range (0.0f, 1.0f), 0.15f, Unit::Percent));
        layout.add (floatParam ("threshold", "Hit Threshold", range (-60.0f, 0.0f), -24.0f, Unit::Db));
        layout.add (floatParam ("volume", "Volume Depth", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        layout.add (floatParam ("filter", "Filter Depth", range (0.0f, 1.0f), 0.0f, Unit::Percent));
        layout.add (choiceParam ("ftype", "Filter Type", { "Low-Pass", "High-Pass", "Band-Pass" }, 0));
        layout.add (floatParam ("flow", "Filter Low", range (20.0f, 5000.0f, 400.0f), 200.0f, Unit::Hz));
        layout.add (floatParam ("fhigh", "Filter High", range (200.0f, 20000.0f, 4000.0f), 16000.0f, Unit::Hz));
        layout.add (floatParam ("res", "Resonance", range (0.0f, 1.0f), 0.2f, Unit::Percent));
        layout.add (floatParam ("pan", "Pan Depth", range (0.0f, 1.0f), 0.0f, Unit::Percent));
        layout.add (floatParam ("width", "Width Depth", range (0.0f, 1.0f), 0.0f, Unit::Percent));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 1.0f, Unit::Percent));
        return layout;
    }

    juce::String getProductName() const override { return "Shaper"; }
    juce::Colour getAccent() const override { return juce::Colour (0xffc78bff); }

    gitto::ShaperParams readParams() const
    {
        gitto::ShaperParams p;
        p.shape = choice ("shape");
        p.length = choice ("length");
        p.trigger = (gitto::ShaperTrigger) choice ("trigger");
        p.smooth = value ("smooth");
        p.thresholdDb = value ("threshold");
        p.volume = value ("volume");
        p.filter = value ("filter");
        p.filterType = (gitto::ShaperFilter) choice ("ftype");
        p.filterLowHz = value ("flow");
        p.filterHighHz = value ("fhigh");
        p.resonance = value ("res");
        p.pan = value ("pan");
        p.width = value ("width");
        p.mix = value ("mix");
        return p;
    }

    //==============================================================================
    // The user's own curve. Edited on the message thread; the audio thread only ever
    // sees the finished lookup table.
    Nodes getCustomNodes() const
    {
        const juce::ScopedLock lock (nodeLock);
        return customNodes;
    }

    void setCustomNodes (Nodes nodes)
    {
        std::sort (nodes.begin(), nodes.end(), [] (auto a, auto b) { return a.x < b.x; });
        if (nodes.size() > (size_t) kMaxNodes)
            nodes.resize ((size_t) kMaxNodes);
        float xs[kMaxNodes], ys[kMaxNodes];
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            xs[i] = juce::jlimit (0.0f, 1.0f, nodes[i].x);
            ys[i] = juce::jlimit (0.0f, 1.0f, nodes[i].y);
        }
        float table[gitto::Shaper::kTable + 1];
        gitto::Shaper::buildTable (xs, ys, (int) nodes.size(), table);
        dsp.setCustomTable (table);
        const juce::ScopedLock lock (nodeLock);
        customNodes = std::move (nodes);
    }

    void saveExtraState (juce::ValueTree& state) override
    {
        juce::String text;
        for (auto& n : getCustomNodes())
            text << juce::String (n.x, 4) << ":" << juce::String (n.y, 4) << ";";
        state.setProperty ("curve", text, nullptr);
    }

    void loadExtraState (const juce::ValueTree& state) override
    {
        const auto text = state.getProperty ("curve").toString();
        if (text.isEmpty())
            return;
        Nodes nodes;
        for (auto& token : juce::StringArray::fromTokens (text, ";", ""))
            if (token.contains (":"))
                nodes.push_back ({ token.upToFirstOccurrenceOf (":", false, false).getFloatValue(),
                                   token.fromFirstOccurrenceOf (":", false, false).getFloatValue() });
        if (nodes.size() >= 2)
            setCustomNodes (std::move (nodes));
    }

    //==============================================================================
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

        double ppq = 0.0, tempo = 120.0;
        bool playing = false;
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
            {
                if (auto t = pos->getBpm()) tempo = *t;
                if (auto q = pos->getPpqPosition()) ppq = *q;
                playing = pos->getIsPlaying();
            }
        bpm.store (tempo);
        dsp.setTransport (ppq, tempo, playing);
        dsp.process (buffer.getWritePointer (0), buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr, buffer.getNumSamples());
    }

    std::vector<Preset> getPresets() const override
    {
        // shape: 1 Duck, 2 Pump, 3 Half Gate, 4 Gate x4, 5 Trance Gate, 6 Saw Down, 7 Saw Up, 8 Triangle, 9 Stairs, 10 Swell
        // length: 0=1/16, 1=1/8, 2=1/4, 3=1/2, 4=1 bar. trigger: 0 song, 1 free, 2 audio hit
        return {
            { "Sidechain Duck 1/4", {} },
            { "Hard Pump 1/4", { { "shape", 2 }, { "length", 2 }, { "smooth", 0.1f } } },
            { "Gentle Breathing 1/2", { { "shape", 1 }, { "length", 3 }, { "volume", 0.5f }, { "smooth", 0.4f } } },
            { "Trance Gate 1 Bar", { { "shape", 5 }, { "length", 4 }, { "smooth", 0.08f } } },
            { "Stutter 1/8", { { "shape", 4 }, { "length", 2 }, { "smooth", 0.05f } } },
            { "Filter Sweep Down", { { "shape", 6 }, { "length", 4 }, { "volume", 0.0f }, { "filter", 1.0f }, { "ftype", 0 }, { "res", 0.45f }, { "smooth", 0.3f } } },
            { "Wobble Bass 1/8", { { "shape", 8 }, { "length", 1 }, { "volume", 0.0f }, { "filter", 0.9f }, { "flow", 120 }, { "fhigh", 4000 }, { "res", 0.6f }, { "smooth", 0.2f } } },
            { "Auto-Pan 1/2", { { "shape", 8 }, { "length", 3 }, { "volume", 0.0f }, { "pan", 0.8f }, { "smooth", 0.5f } } },
            { "Width Pulse", { { "shape", 8 }, { "length", 2 }, { "volume", 0.0f }, { "width", 1.0f }, { "smooth", 0.4f } } },
            { "Kick-Triggered Duck", { { "shape", 1 }, { "length", 1 }, { "trigger", 2 }, { "threshold", -20 } } },
            { "Reverse Swell 1 Bar", { { "shape", 10 }, { "length", 4 }, { "smooth", 0.2f } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Shaper dsp;
    std::atomic<double> bpm { 120.0 };

private:
    juce::CriticalSection nodeLock;
    Nodes customNodes;
};

//==============================================================================
// The curve editor. Click to add a point, drag to move it, double-click to remove it.
class ShaperDisplay : public juce::Component
{
public:
    explicit ShaperDisplay (ShaperProcessor& p) : proc (p) {}

    void paint (juce::Graphics& g) override
    {
        const juce::Colour accent = proc.getAccent();
        const auto area = getLocalBounds().toFloat().reduced (8.0f);
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        const auto r = plot();

        // Grid: quarters of the loop, with lighter eighths.
        for (int i = 0; i <= 8; ++i)
        {
            g.setColour ((i % 2) == 0 ? colours::panelEdge : colours::grid);
            g.fillRect (r.getX() + r.getWidth() * (float) i / 8.0f, r.getY(), 1.0f, r.getHeight());
        }
        for (int i = 0; i <= 4; ++i)
        {
            g.setColour (colours::grid);
            g.fillRect (r.getX(), r.getY() + r.getHeight() * (float) i / 4.0f, r.getWidth(), 1.0f);
        }

        const bool custom = proc.choice ("shape") == 0;
        const auto nodes = custom ? proc.getCustomNodes() : ShaperProcessor::builtInNodes (proc.choice ("shape"));
        juce::Path curve;
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const auto pt = toScreen (nodes[i]);
            if (i == 0)
            {
                curve.startNewSubPath (r.getX(), pt.y);
                curve.lineTo (pt);
            }
            else
                curve.lineTo (pt);
        }
        if (! nodes.empty())
            curve.lineTo (r.getRight(), toScreen (nodes.back()).y);
        juce::Path fill (curve);
        fill.lineTo (r.getRight(), r.getBottom());
        fill.lineTo (r.getX(), r.getBottom());
        fill.closeSubPath();
        g.setColour (accent.withAlpha (0.16f));
        g.fillPath (fill);
        g.setColour (accent);
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));

        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const auto pt = toScreen (nodes[i]);
            g.setColour (custom ? colours::text : colours::dim.withAlpha (0.6f));
            g.fillEllipse (pt.x - 4.0f, pt.y - 4.0f, 8.0f, 8.0f);
        }

        // Where the loop is right now, and the value being applied.
        const float phase = proc.dsp.getPhase();
        const float px = r.getX() + phase * r.getWidth();
        g.setColour (colours::text.withAlpha (0.8f));
        g.fillRect (px - 0.75f, r.getY(), 1.5f, r.getHeight());
        const float value = proc.dsp.getValue();
        g.fillEllipse (px - 5.0f, r.getBottom() - value * r.getHeight() - 5.0f, 10.0f, 10.0f);

        g.setColour (colours::dim.withAlpha (0.85f));
        g.setFont (font (9.5f));
        g.drawText (juce::String (proc.bpm.load(), 1) + " BPM   |   "
                        + (custom ? "Click to add a point  |  drag to move  |  double-click to remove"
                                  : "Built-in shape: click anywhere to turn it into your own curve"),
                    area.reduced (10.0f, 5.0f), juce::Justification::bottomRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto nodes = editableNodes();
        dragging = nearest (nodes, e.position);
        if (dragging < 0 && (int) nodes.size() < ShaperProcessor::kMaxNodes)
        {
            nodes.push_back (fromScreen (e.position));
            std::sort (nodes.begin(), nodes.end(), [] (auto a, auto b) { return a.x < b.x; });
            proc.setCustomNodes (nodes);
            dragging = nearest (nodes, e.position);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;
        auto nodes = proc.getCustomNodes();
        if (dragging >= (int) nodes.size())
            return;
        auto pt = fromScreen (e.position);
        // Points keep their order: each one can only move between its neighbours.
        const float lo = dragging > 0 ? nodes[(size_t) dragging - 1].x : 0.0f;
        const float hi = dragging + 1 < (int) nodes.size() ? nodes[(size_t) dragging + 1].x : 1.0f;
        pt.x = juce::jlimit (lo, hi, pt.x);
        if (dragging == 0) pt.x = 0.0f;
        if (dragging == (int) nodes.size() - 1) pt.x = 1.0f;
        nodes[(size_t) dragging] = pt;
        proc.setCustomNodes (nodes);
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = -1; }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        auto nodes = editableNodes();
        const int index = nearest (nodes, e.position);
        // The first and last points anchor the loop and stay.
        if (index > 0 && index < (int) nodes.size() - 1)
        {
            nodes.erase (nodes.begin() + index);
            proc.setCustomNodes (nodes);
        }
        dragging = -1;
    }

private:
    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (20.0f, 22.0f).withTrimmedTop (-8.0f); }
    juce::Point<float> toScreen (juce::Point<float> n) const
    {
        const auto r = plot();
        return { r.getX() + n.x * r.getWidth(), r.getBottom() - n.y * r.getHeight() };
    }
    juce::Point<float> fromScreen (juce::Point<float> p) const
    {
        const auto r = plot();
        return { juce::jlimit (0.0f, 1.0f, (p.x - r.getX()) / r.getWidth()), juce::jlimit (0.0f, 1.0f, (r.getBottom() - p.y) / r.getHeight()) };
    }
    int nearest (const ShaperProcessor::Nodes& nodes, juce::Point<float> p) const
    {
        int best = -1;
        float bestDist = 11.0f;
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const float d = toScreen (nodes[i]).getDistanceFrom (p);
            if (d < bestDist)
            {
                bestDist = d;
                best = (int) i;
            }
        }
        return best;
    }

    // Returns the user's curve, first turning a built-in shape into one if needed.
    ShaperProcessor::Nodes editableNodes()
    {
        const int shape = proc.choice ("shape");
        if (shape != 0)
        {
            auto nodes = ShaperProcessor::builtInNodes (shape);
            if (nodes.front().x > 0.0f) nodes.insert (nodes.begin(), { 0.0f, nodes.front().y });
            if (nodes.back().x < 1.0f) nodes.push_back ({ 1.0f, nodes.back().y });
            proc.setCustomNodes (nodes);
            if (auto* p = proc.apvts.getParameter ("shape"))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (0.0f);
                p->endChangeGesture();
            }
        }
        return proc.getCustomNodes();
    }

    ShaperProcessor& proc;
    int dragging = -1;
};

juce::AudioProcessorEditor* ShaperProcessor::createEditor()
{
    LayoutSpec l;
    l.displayHeight = 220;
    l.rows = {
        { { "Curve", { chooser ("shape", "Shape"), chooser ("length", "Length"), knob ("smooth", "Smooth") } },
          { "Trigger", { chooser ("trigger", "Mode"), knob ("threshold", "Hit Threshold") } },
          { "Volume", { knob ("volume", "Depth") } } },
        { { "Filter", { knob ("filter", "Depth"), chooser ("ftype", "Type"), knob ("flow", "Low"), knob ("fhigh", "High"), knob ("res", "Resonance") } },
          { "Stereo", { knob ("pan", "Pan"), knob ("width", "Width") } },
          { "Output", { knob ("mix", "Mix") } } },
    };
    auto* editor = new GittoEditor (*this, std::move (l), std::make_unique<ShaperDisplay> (*this));
    editor->onTimer = [this, editor]
    {
        editor->setControlEnabled ("threshold", choice ("trigger") == (int) gitto::ShaperTrigger::Audio);
        const bool filterOn = value ("filter") > 0.005f;
        for (auto* id : { "ftype", "flow", "fhigh", "res" })
            editor->setControlEnabled (id, filterOn);
    };
    return editor;
}

juce::AudioProcessor* createGittoShaper() { return new ShaperProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoShaper(); }
#endif
