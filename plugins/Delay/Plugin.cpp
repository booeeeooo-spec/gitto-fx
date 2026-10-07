// Gitto FX Delay - plugin wrapper and interface.
#include "../../dsp/Delay.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
namespace
{
struct NoteValue { const char* name; float beats; };
const NoteValue kNotes[] = {
    { "1/32", 0.125f }, { "1/16 T", 1.0f / 6.0f }, { "1/16", 0.25f }, { "1/16 D", 0.375f }, { "1/8 T", 1.0f / 3.0f },
    { "1/8", 0.5f }, { "1/8 D", 0.75f }, { "1/4 T", 2.0f / 3.0f }, { "1/4", 1.0f }, { "1/4 D", 1.5f },
    { "1/2 T", 4.0f / 3.0f }, { "1/2", 2.0f }, { "1/2 D", 3.0f }, { "1 bar", 4.0f },
};
constexpr int kNumNotes = (int) (sizeof (kNotes) / sizeof (kNotes[0]));
} // namespace

class DelayProcessor : public GittoProcessor
{
public:
    DelayProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
        allowMonoToStereo = true;
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        juce::StringArray notes, patterns;
        for (auto& n : kNotes)
            notes.add (n.name);
        for (int i = 0; i < gitto::kNumRhythmPatterns; ++i)
            patterns.add (gitto::rhythmPattern (i).name);

        layout.add (choiceParam ("mode", "Mode", { "Single", "Dual", "Ping-Pong", "Rhythm" }, 0));
        layout.add (choiceParam ("style", "Style", { "Digital", "Studio Tape", "Worn Tape", "Analog", "Tube", "Lo-Fi", "Telephone", "Diffuse" }, 1));
        layout.add (choiceParam ("pattern", "Rhythm Pattern", patterns, 0));
        layout.add (boolParam ("sync", "Tempo Sync", true));
        layout.add (floatParam ("timel", "Time Left", range (1.0f, 2000.0f, 300.0f), 375.0f, Unit::Ms));
        layout.add (floatParam ("timer", "Time Right", range (1.0f, 2000.0f, 300.0f), 500.0f, Unit::Ms));
        layout.add (choiceParam ("notel", "Note Left", notes, 6));
        layout.add (choiceParam ("noter", "Note Right", notes, 8));
        layout.add (floatParam ("feedback", "Feedback", range (0.0f, 1.1f), 0.4f, Unit::Percent));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 0.28f, Unit::Percent));
        layout.add (floatParam ("duck", "Ducking", range (0.0f, 1.0f), 0.0f, Unit::Percent));
        layout.add (floatParam ("lowcut", "Low Cut", range (20.0f, 2000.0f, 200.0f), 80.0f, Unit::Hz));
        layout.add (floatParam ("highcut", "High Cut", range (500.0f, 20000.0f, 4000.0f), 8000.0f, Unit::Hz));
        layout.add (floatParam ("sat", "Saturation", range (0.0f, 1.0f), 0.3f, Unit::Percent));
        layout.add (floatParam ("diffusion", "Diffusion", range (0.0f, 1.0f), 0.0f, Unit::Percent));
        layout.add (floatParam ("wobble", "Wobble", range (0.0f, 1.0f), 0.2f, Unit::Percent));
        layout.add (floatParam ("wobblerate", "Wobble Rate", range (0.1f, 5.0f, 1.0f), 0.8f, Unit::Hz));
        layout.add (floatParam ("groove", "Groove", range (-1.0f, 1.0f), 0.0f, Unit::Percent));
        layout.add (floatParam ("feel", "Feel", range (-1.0f, 1.0f), 0.0f, Unit::Percent));
        layout.add (floatParam ("width", "Width", range (0.0f, 2.0f), 1.0f, Unit::Percent));
        layout.add (floatParam ("out", "Output", range (-24.0f, 12.0f), 0.0f, Unit::Db));
        return layout;
    }

    juce::String getProductName() const override { return "Delay"; }
    juce::Colour getAccent() const override { return juce::Colour (0xff4fdc8c); }
    double getTailLengthSeconds() const override { return 10.0; }

    float noteMs (int index) const
    {
        const double tempo = juce::jlimit (30.0, 300.0, bpm.load());
        return (float) juce::jlimit (1.0, gitto::Delay::kMaxTimeMs, kNotes[juce::jlimit (0, kNumNotes - 1, index)].beats * 60000.0 / tempo);
    }

    gitto::DelayParams readParams() const
    {
        gitto::DelayParams p;
        p.mode = (gitto::DelayMode) choice ("mode");
        p.style = (gitto::DelayStyle) choice ("style");
        p.pattern = choice ("pattern");
        const bool sync = flag ("sync");
        p.timeLMs = sync ? noteMs (choice ("notel")) : value ("timel");
        p.timeRMs = sync ? noteMs (choice ("noter")) : value ("timer");
        if (p.mode == gitto::DelayMode::Rhythm)
            p.timeLMs = juce::jmin (p.timeLMs, (float) gitto::Delay::kMaxTimeMs);
        p.feedback = value ("feedback");
        p.mix = value ("mix");
        p.ducking = value ("duck");
        p.lowCutHz = value ("lowcut");
        p.highCutHz = value ("highcut");
        p.saturation = value ("sat");
        p.diffusion = value ("diffusion");
        p.wobble = value ("wobble");
        p.wobbleRateHz = value ("wobblerate");
        p.groove = value ("groove");
        p.feel = value ("feel");
        p.width = value ("width");
        p.outputDb = value ("out");
        return p;
    }

    void prepareToPlay (double sr, int) override
    {
        delay.setParams (readParams());
        delay.prepare (sr);
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
            delay.setParams (readParams());

        const int n = buffer.getNumSamples();
        float* l = buffer.getWritePointer (0);
        float* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
        if (r != nullptr && getTotalNumInputChannels() < 2)
            juce::FloatVectorOperations::copy (r, l, n);
        delay.process (l, r, n);
    }

    std::vector<Preset> getPresets() const override
    {
        // style: 0 Digital, 1 Studio Tape, 2 Worn Tape, 3 Analog, 4 Tube, 5 Lo-Fi, 6 Telephone, 7 Diffuse
        // notes: 2 = 1/16, 5 = 1/8, 6 = 1/8 D, 8 = 1/4, 9 = 1/4 D, 11 = 1/2
        return {
            { "Default Tape Echo", {} },
            { "Vocal Throw 1/4", { { "mode", 0 }, { "style", 1 }, { "notel", 8 }, { "feedback", 0.35f }, { "mix", 0.22f }, { "duck", 0.6f }, { "lowcut", 200 }, { "highcut", 5000 } } },
            { "Dotted Eighth Bounce", { { "mode", 2 }, { "style", 0 }, { "notel", 6 }, { "noter", 6 }, { "feedback", 0.45f }, { "mix", 0.25f }, { "highcut", 9000 } } },
            { "Slapback", { { "mode", 0 }, { "style", 4 }, { "sync", 0 }, { "timel", 95 }, { "feedback", 0.08f }, { "mix", 0.3f }, { "sat", 0.5f } } },
            { "Wide Stereo 1/8 + 1/4", { { "mode", 1 }, { "style", 1 }, { "notel", 5 }, { "noter", 8 }, { "feedback", 0.38f }, { "mix", 0.24f }, { "width", 1.4f } } },
            { "Dub Chamber", { { "mode", 2 }, { "style", 2 }, { "notel", 9 }, { "noter", 8 }, { "feedback", 0.72f }, { "mix", 0.35f }, { "wobble", 0.5f }, { "sat", 0.6f }, { "lowcut", 250 }, { "highcut", 3200 } } },
            { "Analog Haze", { { "mode", 0 }, { "style", 3 }, { "notel", 8 }, { "feedback", 0.55f }, { "mix", 0.3f }, { "wobble", 0.35f } } },
            { "Lo-Fi Tape Dust", { { "mode", 0 }, { "style", 5 }, { "notel", 5 }, { "feedback", 0.5f }, { "mix", 0.3f }, { "wobble", 0.6f } } },
            { "Phone Call Echo", { { "mode", 0 }, { "style", 6 }, { "notel", 8 }, { "feedback", 0.4f }, { "mix", 0.3f } } },
            { "Diffuse Wash", { { "mode", 1 }, { "style", 7 }, { "notel", 9 }, { "noter", 11 }, { "feedback", 0.7f }, { "mix", 0.4f }, { "diffusion", 1.0f }, { "highcut", 6000 } } },
            { "Swing Shuffle", { { "mode", 0 }, { "style", 1 }, { "notel", 5 }, { "feedback", 0.5f }, { "mix", 0.28f }, { "groove", 0.66f } } },
            { "Triplet Roll", { { "mode", 3 }, { "pattern", 2 }, { "style", 0 }, { "notel", 8 }, { "feedback", 0.3f }, { "mix", 0.3f } } },
            { "Dotted Gallop", { { "mode", 3 }, { "pattern", 1 }, { "style", 1 }, { "notel", 8 }, { "feedback", 0.35f }, { "mix", 0.3f } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Delay delay;
    std::atomic<double> bpm { 120.0 };
};

//==============================================================================
// Draws the echo pattern the current settings produce: left channel above the
// line, right channel below, time running left to right.
class DelayDisplay : public juce::Component
{
public:
    explicit DelayDisplay (DelayProcessor& p) : proc (p) {}

    void paint (juce::Graphics& g) override
    {
        const juce::Colour accent = proc.getAccent();
        const auto area = getLocalBounds().toFloat().reduced (8.0f);
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        const auto r = area.reduced (12.0f, 8.0f);
        const float midY = r.getCentreY();

        const auto p = proc.readParams();
        struct Echo { float time, level, pan; };
        std::vector<Echo> echoes;
        const float g2 = 0.5f * p.groove;
        const float fb = juce::jmin (p.feedback, 0.98f);

        if (p.mode == gitto::DelayMode::Rhythm)
        {
            const auto& pat = gitto::rhythmPattern (p.pattern);
            float loopLevel = 1.0f;
            for (int loop = 0; loop < 12 && loopLevel > 0.03f; ++loop)
            {
                for (int k = 0; k < pat.numTaps; ++k)
                    echoes.push_back ({ (pat.position[k] + (float) (loop * pat.steps)) * p.timeLMs, pat.level[k] * loopLevel, pat.pan[k] });
                loopLevel *= fb;
            }
        }
        else
        {
            const bool ping = p.mode == gitto::DelayMode::PingPong;
            const float tR = p.mode == gitto::DelayMode::Single ? p.timeLMs : p.timeRMs;
            for (int side = 0; side < (ping ? 1 : 2); ++side)
            {
                const float base = side == 0 ? p.timeLMs : tR;
                float t = 0.0f, level = 1.0f;
                for (int k = 0; k < 40 && level > 0.03f; ++k)
                {
                    const bool odd = (k % 2) == 0;
                    const float segBase = ping ? (odd ? p.timeLMs : p.timeRMs) : base;
                    t += segBase * (odd ? 1.0f + g2 : 1.0f - g2);
                    const float pan = ping ? (odd ? -1.0f : 1.0f) : (side == 0 ? -1.0f : 1.0f);
                    echoes.push_back ({ t, level, pan });
                    level *= fb;
                }
            }
        }

        float span = 1000.0f;
        for (auto& e : echoes)
            span = juce::jmax (span, e.time * 1.04f);
        span = juce::jmin (span, 8000.0f);
        auto toX = [&] (float ms) { return r.getX() + ms / span * r.getWidth(); };

        // Time grid.
        g.setFont (font (9.5f));
        const float step = span > 5000.0f ? 1000.0f : (span > 2000.0f ? 500.0f : 250.0f);
        for (float t = step; t < span; t += step)
        {
            g.setColour (colours::grid);
            g.fillRect (toX (t), r.getY(), 1.0f, r.getHeight());
            g.setColour (colours::dim.withAlpha (0.7f));
            g.drawText (t >= 1000.0f ? juce::String (t / 1000.0f, 1) + " s" : juce::String ((int) t) + " ms",
                        juce::Rectangle<float> (toX (t) + 3.0f, r.getBottom() - 12.0f, 50.0f, 11.0f), juce::Justification::centredLeft);
        }
        g.setColour (colours::panelEdge);
        g.fillRect (r.getX(), midY, r.getWidth(), 1.0f);

        const float half = r.getHeight() * 0.5f - 14.0f;
        g.setColour (colours::text);
        g.fillRoundedRectangle (toX (0.0f), midY - half, 3.0f, half * 2.0f, 1.5f);
        for (auto& e : echoes)
        {
            if (e.time > span)
                continue;
            const float x = toX (e.time);
            const float lGain = 1.0f - juce::jmax (0.0f, e.pan), rGain = 1.0f + juce::jmin (0.0f, e.pan);
            g.setColour (accent.withAlpha (juce::jlimit (0.25f, 1.0f, 0.3f + e.level * 0.7f)));
            if (lGain > 0.01f)
                g.fillRoundedRectangle (x, midY - half * e.level * lGain, 3.0f, half * e.level * lGain, 1.5f);
            if (rGain > 0.01f)
                g.fillRoundedRectangle (x, midY, 3.0f, half * e.level * rGain, 1.5f);
        }

        g.setColour (colours::dim);
        g.setFont (font (9.5f, true));
        g.drawText ("L", r.withTrimmedLeft (8.0f).withHeight (12.0f), juce::Justification::centredLeft);
        g.drawText ("R", r.withTrimmedLeft (8.0f).withTop (r.getBottom() - 26.0f).withHeight (12.0f), juce::Justification::centredLeft);

        juce::String info = juce::String (p.timeLMs, 0) + " ms";
        if (p.mode == gitto::DelayMode::Dual || p.mode == gitto::DelayMode::PingPong)
            info << "  /  " << juce::String (p.timeRMs, 0) << " ms";
        if (proc.flag ("sync"))
            info << "   at " << juce::String (proc.bpm.load(), 1) << " BPM";
        g.setColour (colours::text);
        g.setFont (font (12.0f, true));
        g.drawText (info, r.withHeight (14.0f), juce::Justification::centredRight);
    }

private:
    DelayProcessor& proc;
};

juce::AudioProcessorEditor* DelayProcessor::createEditor()
{
    LayoutSpec l;
    l.displayHeight = 160;
    l.rows = {
        { { "Echo", { chooser ("mode", "Mode"), chooser ("style", "Style"), chooser ("pattern", "Pattern") } },
          { "Repeats", { knob ("feedback", "Feedback"), knob ("mix", "Mix"), knob ("duck", "Ducking") } } },
        { { "Time", { toggle ("sync", "Sync"), chooser ("notel", "Note L"), chooser ("noter", "Note R"), knob ("timel", "Time L"), knob ("timer", "Time R") } },
          { "Feel", { knob ("groove", "Groove", true), knob ("feel", "Feel", true) } } },
        { { "Tone", { knob ("lowcut", "Low Cut"), knob ("highcut", "High Cut"), knob ("sat", "Saturation"), knob ("diffusion", "Diffusion") } },
          { "Tape", { knob ("wobble", "Wobble"), knob ("wobblerate", "Rate") } },
          { "Output", { knob ("width", "Width"), knob ("out", "Level", true) } } },
    };
    auto* editor = new GittoEditor (*this, std::move (l), std::make_unique<DelayDisplay> (*this));
    editor->onTimer = [this, editor]
    {
        const bool sync = flag ("sync");
        const auto mode = (gitto::DelayMode) choice ("mode");
        const bool twoTimes = mode == gitto::DelayMode::Dual || mode == gitto::DelayMode::PingPong;
        editor->setControlEnabled ("notel", sync);
        editor->setControlEnabled ("noter", sync && twoTimes);
        editor->setControlEnabled ("timel", ! sync);
        editor->setControlEnabled ("timer", ! sync && twoTimes);
        editor->setControlEnabled ("pattern", mode == gitto::DelayMode::Rhythm);
        editor->setControlEnabled ("groove", mode != gitto::DelayMode::Rhythm);
    };
    return editor;
}

juce::AudioProcessor* createGittoDelay() { return new DelayProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoDelay(); }
#endif
