// Gitto FX Space - plugin wrapper and interface.
#include "../../dsp/Space.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
namespace
{
struct SpaceNote { const char* name; float beats; };
const SpaceNote kSpaceNotes[] = {
    { "1/16", 0.25f }, { "1/8 T", 1.0f / 3.0f }, { "1/8", 0.5f }, { "1/8 D", 0.75f }, { "1/4 T", 2.0f / 3.0f },
    { "1/4", 1.0f }, { "1/4 D", 1.5f }, { "1/2", 2.0f }, { "1/2 D", 3.0f }, { "1 bar", 4.0f },
};
constexpr int kNumSpaceNotes = (int) (sizeof (kSpaceNotes) / sizeof (kSpaceNotes[0]));
}

class SpaceProcessor : public GittoProcessor
{
public:
    SpaceProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
        allowMonoToStereo = true;
    }

    static juce::StringArray modeNames()
    {
        juce::StringArray names;
        for (int i = 0; i < (int) gitto::SpaceMode::Count; ++i)
            names.add (gitto::Space::modeData ((gitto::SpaceMode) i).name);
        return names;
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        juce::StringArray notes;
        for (auto& n : kSpaceNotes)
            notes.add (n.name);
        layout.add (choiceParam ("mode", "Mode", modeNames(), 1));
        layout.add (boolParam ("sync", "Tempo Sync", false));
        layout.add (floatParam ("delay", "Delay", range (10.0f, 2000.0f, 300.0f), 300.0f, Unit::Ms));
        layout.add (choiceParam ("note", "Note", notes, 5));
        layout.add (floatParam ("warp", "Warp", range (0.0f, 1.0f), 0.6f, Unit::Percent));
        layout.add (floatParam ("feedback", "Feedback", range (0.0f, 1.0f), 0.6f, Unit::Percent));
        layout.add (floatParam ("density", "Density", range (0.0f, 1.0f), 0.6f, Unit::Percent));
        layout.add (floatParam ("modrate", "Modulation Rate", range (0.01f, 10.0f, 0.5f), 0.4f, Unit::Hz));
        layout.add (floatParam ("moddepth", "Modulation Depth", range (0.0f, 1.0f), 0.3f, Unit::Percent));
        layout.add (floatParam ("lowcut", "Low Cut", range (10.0f, 2000.0f, 150.0f), 20.0f, Unit::Hz));
        layout.add (floatParam ("highcut", "High Cut", range (200.0f, 20000.0f, 4000.0f), 9000.0f, Unit::Hz));
        layout.add (floatParam ("width", "Width", range (0.0f, 2.0f), 1.0f, Unit::Percent));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 0.35f, Unit::Percent));
        layout.add (boolParam ("freeze", "Freeze", false));
        return layout;
    }

    juce::String getProductName() const override { return "Space"; }
    juce::Colour getAccent() const override { return juce::Colour (0xff8f9bff); }
    double getTailLengthSeconds() const override { return 20.0; }

    gitto::SpaceParams readParams() const
    {
        gitto::SpaceParams p;
        p.mode = (gitto::SpaceMode) choice ("mode");
        if (flag ("sync"))
        {
            const double tempo = juce::jlimit (30.0, 300.0, bpm.load());
            p.delayMs = (float) juce::jlimit (10.0, gitto::Space::kMaxDelayMs, kSpaceNotes[juce::jlimit (0, kNumSpaceNotes - 1, choice ("note"))].beats * 60000.0 / tempo);
        }
        else
        {
            p.delayMs = value ("delay");
        }
        p.warp = value ("warp");
        p.feedback = value ("feedback");
        p.density = value ("density");
        p.modRateHz = value ("modrate");
        p.modDepth = value ("moddepth");
        p.lowCutHz = value ("lowcut");
        p.highCutHz = value ("highcut");
        p.width = value ("width");
        p.mix = value ("mix");
        p.freeze = flag ("freeze");
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
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
                if (auto tempo = pos->getBpm())
                    if (std::abs (*tempo - bpm.load()) > 0.001)
                    {
                        bpm.store (*tempo);
                        markDirty();
                    }
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
        // mode: 0 Drift, 1 Bloom, 2 Cascade, 3 Vapor, 4 Canyon, 5 Glacier, 6 Aurora, 7 Abyss
        return {
            { "Default Bloom", {} },
            { "Endless Pad", { { "mode", 5 }, { "delay", 900 }, { "feedback", 0.92f }, { "density", 0.9f }, { "warp", 0.7f }, { "mix", 0.5f }, { "highcut", 6000 } } },
            { "Vocal Cloud", { { "mode", 3 }, { "delay", 220 }, { "feedback", 0.7f }, { "density", 0.8f }, { "lowcut", 200 }, { "highcut", 8000 }, { "mix", 0.28f } } },
            { "Wide Echo Drift", { { "mode", 0 }, { "sync", 1 }, { "note", 3 }, { "feedback", 0.55f }, { "warp", 0.35f }, { "density", 0.3f }, { "mix", 0.3f } } },
            { "Canyon Ping-Pong", { { "mode", 4 }, { "sync", 1 }, { "note", 5 }, { "feedback", 0.6f }, { "warp", 1.0f }, { "density", 0.25f }, { "mix", 0.3f } } },
            { "Cascading Echoes", { { "mode", 2 }, { "delay", 500 }, { "feedback", 0.5f }, { "warp", 0.8f }, { "density", 0.4f }, { "mix", 0.35f } } },
            { "Shimmering Aurora", { { "mode", 6 }, { "delay", 400 }, { "feedback", 0.8f }, { "moddepth", 0.6f }, { "modrate", 0.25f }, { "lowcut", 300 }, { "highcut", 14000 }, { "mix", 0.4f } } },
            { "Deep Abyss", { { "mode", 7 }, { "delay", 1200 }, { "feedback", 0.85f }, { "density", 0.8f }, { "highcut", 3000 }, { "mix", 0.45f } } },
            { "Game Menu Ambience", { { "mode", 1 }, { "delay", 650 }, { "feedback", 0.88f }, { "density", 0.85f }, { "lowcut", 250 }, { "highcut", 7000 }, { "mix", 0.5f } } },
            { "Short Slap Space", { { "mode", 0 }, { "delay", 90 }, { "feedback", 0.3f }, { "warp", 0.9f }, { "density", 0.6f }, { "mix", 0.25f } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Space dsp;
    std::atomic<double> bpm { 120.0 };
};

// Each line of the network as a row of echoes, fading with the feedback setting.
class SpaceDisplay : public juce::Component
{
public:
    explicit SpaceDisplay (SpaceProcessor& p) : proc (p) {}

    void paint (juce::Graphics& g) override
    {
        const juce::Colour accent = proc.getAccent();
        const auto area = getLocalBounds().toFloat().reduced (8.0f);
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        auto r = area.reduced (12.0f, 8.0f);
        const auto header = r.removeFromTop (16.0f);

        const auto p = proc.readParams();
        model.setParams (p);
        const auto& md = gitto::Space::modeData (p.mode);
        const float span = juce::jlimit (400.0f, 12000.0f, p.delayMs * 5.0f);
        auto toX = [&] (float ms) { return r.getX() + juce::jlimit (0.0f, 1.0f, ms / span) * r.getWidth(); };

        g.setFont (font (9.5f));
        const float step = span > 6000.0f ? 2000.0f : (span > 2500.0f ? 1000.0f : (span > 1200.0f ? 500.0f : 250.0f));
        for (float t = step; t < span; t += step)
        {
            g.setColour (colours::grid);
            g.fillRect (toX (t), r.getY(), 1.0f, r.getHeight());
            g.setColour (colours::dim.withAlpha (0.7f));
            g.drawText (t >= 1000.0f ? juce::String (t / 1000.0f, 1) + " s" : juce::String ((int) t) + " ms",
                        juce::Rectangle<float> (toX (t) + 3.0f, r.getBottom() - 12.0f, 50.0f, 11.0f), juce::Justification::centredLeft);
        }

        const float fb = p.freeze ? 1.0f : 0.985f * p.feedback;
        const float rowH = (r.getHeight() - 14.0f) / (float) md.lines;
        for (int i = 0; i < md.lines; ++i)
        {
            const float delay = model.lineDelayMs (i);
            const float y = r.getY() + rowH * ((float) i + 0.5f);
            const juce::Colour colour = (i & 1) ? juce::Colour (0xff6ec6ff) : accent;
            float level = 1.0f;
            for (int k = 1; k < 80 && level > 0.04f; ++k)
            {
                const float t = delay * (float) k;
                if (t > span)
                    break;
                const float h = juce::jmax (2.0f, rowH * 0.85f * level);
                // Density smears each echo: draw it wider and softer.
                const float w = 2.0f + 10.0f * p.density;
                g.setColour (colour.withAlpha (juce::jlimit (0.15f, 1.0f, level) * (1.0f - 0.5f * p.density)));
                g.fillRoundedRectangle (toX (t) - w * 0.5f, y - h * 0.5f, w, h, 1.5f);
                level *= fb;
            }
        }

        g.setColour (colours::text);
        g.setFont (font (13.0f, true));
        g.drawText (juce::String (md.name).toUpperCase(), header, juce::Justification::centredLeft);
        g.setFont (font (12.0f, true));
        juce::String info = juce::String (p.delayMs, 0) + " ms";
        if (proc.flag ("sync"))
            info << "   at " << juce::String (proc.bpm.load(), 1) << " BPM";
        if (p.freeze)
            info << "   FROZEN";
        g.drawText (info, header, juce::Justification::centredRight);
    }

private:
    SpaceProcessor& proc;
    gitto::Space model; // used only to work out each line's delay
};

juce::AudioProcessorEditor* SpaceProcessor::createEditor()
{
    LayoutSpec l;
    l.displayHeight = 170;
    l.rows = {
        { { "Space", { chooser ("mode", "Mode"), knob ("feedback", "Feedback"), knob ("density", "Density"), knob ("warp", "Warp") } },
          { "Time", { toggle ("sync", "Sync"), chooser ("note", "Note"), knob ("delay", "Delay") } } },
        { { "Modulation", { knob ("modrate", "Rate"), knob ("moddepth", "Depth") } },
          { "Tone", { knob ("lowcut", "Low Cut"), knob ("highcut", "High Cut") } },
          { "Output", { knob ("width", "Width"), knob ("mix", "Mix"), toggle ("freeze", "Freeze") } } },
    };
    auto* editor = new GittoEditor (*this, std::move (l), std::make_unique<SpaceDisplay> (*this));
    editor->onTimer = [this, editor]
    {
        const bool sync = flag ("sync");
        editor->setControlEnabled ("note", sync);
        editor->setControlEnabled ("delay", ! sync);
    };
    return editor;
}

juce::AudioProcessor* createGittoSpace() { return new SpaceProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoSpace(); }
#endif
