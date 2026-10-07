// Gitto FX Chorus - plugin wrapper and interface.
#include "../../dsp/Chorus.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class ChorusProcessor : public GittoProcessor
{
public:
    ChorusProcessor()
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
        layout.add (choiceParam ("type", "Type", { "Dimension", "Classic", "Ensemble", "Modern" }, 0));
        layout.add (choiceParam ("mode", "Mode", { "I", "II", "III", "IV", "Manual" }, 1));
        layout.add (choiceParam ("shape", "LFO Shape", { "Triangle", "Sine", "Random" }, 0));
        layout.add (floatParam ("rate", "Rate", range (0.05f, 10.0f, 1.0f), 0.5f, Unit::Hz));
        layout.add (floatParam ("depth", "Depth", range (0.0f, 1.0f), 0.5f, Unit::Percent));
        layout.add (floatParam ("delay", "Delay", range (1.0f, 30.0f, 8.0f), 10.0f, Unit::Ms));
        layout.add (floatParam ("feedback", "Feedback", range (-0.9f, 0.9f), 0.0f, Unit::Percent));
        layout.add (floatParam ("tone", "Tone", range (1000.0f, 20000.0f, 6000.0f), 12000.0f, Unit::Hz));
        layout.add (floatParam ("warmth", "Warmth", range (0.0f, 1.0f), 0.3f, Unit::Percent));
        layout.add (floatParam ("width", "Width", range (0.0f, 2.0f), 1.0f, Unit::Percent));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 0.5f, Unit::Percent));
        layout.add (floatParam ("out", "Output", range (-24.0f, 12.0f), 0.0f, Unit::Db));
        return layout;
    }

    juce::String getProductName() const override { return "Chorus"; }
    juce::Colour getAccent() const override { return juce::Colour (0xffff7ac8); }
    double getTailLengthSeconds() const override { return 0.5; }

    gitto::ChorusParams readParams() const
    {
        gitto::ChorusParams p;
        p.type = (gitto::ChorusType) choice ("type");
        p.mode = (gitto::ChorusMode) choice ("mode");
        p.shape = (gitto::ChorusShape) choice ("shape");
        p.rateHz = value ("rate");
        p.depth = value ("depth");
        p.delayMs = value ("delay");
        p.feedback = value ("feedback");
        p.toneHz = value ("tone");
        p.warmth = value ("warmth");
        p.width = value ("width");
        p.mix = value ("mix");
        p.outputDb = value ("out");
        return p;
    }

    void prepareToPlay (double sr, int) override
    {
        chorus.setParams (readParams());
        chorus.prepare (sr);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        if (consumeDirty())
            chorus.setParams (readParams());

        const int n = buffer.getNumSamples();
        float* l = buffer.getWritePointer (0);
        float* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
        if (r != nullptr && getTotalNumInputChannels() < 2)
            juce::FloatVectorOperations::copy (r, l, n);
        chorus.process (l, r, n);
    }

    std::vector<Preset> getPresets() const override
    {
        // type: 0 Dimension, 1 Classic, 2 Ensemble, 3 Modern. mode: 0..3 = I..IV, 4 = Manual
        return {
            { "Default Dimension", {} },
            { "Subtle Widener", { { "type", 0 }, { "mode", 0 }, { "mix", 0.4f }, { "warmth", 0.2f } } },
            { "Wide Pad", { { "type", 0 }, { "mode", 3 }, { "mix", 0.55f }, { "width", 1.4f } } },
            { "80s Synth Chorus", { { "type", 1 }, { "mode", 0 }, { "mix", 0.5f }, { "warmth", 0.5f } } },
            { "Fast Classic II", { { "type", 1 }, { "mode", 1 }, { "mix", 0.5f }, { "warmth", 0.5f } } },
            { "String Ensemble", { { "type", 2 }, { "mode", 1 }, { "mix", 0.6f }, { "warmth", 0.6f }, { "tone", 8000 } } },
            { "Lush Six Voice", { { "type", 3 }, { "mode", 1 }, { "mix", 0.5f }, { "width", 1.3f } } },
            { "Slow Detune", { { "type", 3 }, { "mode", 3 }, { "mix", 0.45f } } },
            { "Guitar Shimmer", { { "type", 3 }, { "mode", 4 }, { "rate", 0.9f }, { "depth", 0.35f }, { "delay", 8 }, { "mix", 0.4f } } },
            { "Vocal Doubler", { { "type", 3 }, { "mode", 4 }, { "shape", 2 }, { "rate", 0.6f }, { "depth", 0.2f }, { "delay", 18 }, { "mix", 0.35f } } },
            { "Flange Sweep", { { "type", 1 }, { "mode", 4 }, { "shape", 1 }, { "rate", 0.18f }, { "depth", 0.9f }, { "delay", 2.5f }, { "feedback", 0.7f }, { "mix", 0.5f } } },
            { "Lo-Fi Wobble", { { "type", 1 }, { "mode", 4 }, { "shape", 2 }, { "rate", 2.2f }, { "depth", 0.3f }, { "delay", 6 }, { "warmth", 0.9f }, { "tone", 5000 }, { "mix", 0.6f } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Chorus chorus;
};

//==============================================================================
// Shows how each voice's delay time moves, so the rate, depth and voice spread are
// visible at a glance.
class ChorusDisplay : public juce::Component
{
public:
    explicit ChorusDisplay (ChorusProcessor& p) : proc (p) {}

    void paint (juce::Graphics& g) override
    {
        const juce::Colour accent = proc.getAccent();
        const auto area = getLocalBounds().toFloat().reduced (8.0f);
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        auto r = area.reduced (12.0f, 8.0f);
        auto header = r.removeFromTop (16.0f);

        const auto p = proc.readParams();
        float rate, depthMs, delayMs;
        model.setParams (p);
        model.getEffective (rate, depthMs, delayMs);
        const auto& td = gitto::Chorus::typeData (p.type);

        g.setColour (colours::text);
        g.setFont (font (12.0f, true));
        g.drawText (juce::String (rate, 2) + " Hz    " + juce::String (depthMs, 2) + " ms sweep    " + juce::String (delayMs, 1) + " ms delay",
                    header, juce::Justification::centredRight);
        g.setColour (colours::dim);
        g.setFont (font (9.5f, true));
        g.drawText (juce::String (td.numVoices) + " VOICES", header, juce::Justification::centredLeft);

        // Vertical scale: delay time in ms.
        float lo = 1.0e9f, hi = -1.0e9f;
        for (int v = 0; v < td.numVoices; ++v)
        {
            lo = juce::jmin (lo, delayMs + td.voices[v].delayOffsetMs - depthMs * (1.0f + td.fastLfoDepth));
            hi = juce::jmax (hi, delayMs + td.voices[v].delayOffsetMs + depthMs * (1.0f + td.fastLfoDepth));
        }
        const float pad = juce::jmax (0.5f, (hi - lo) * 0.15f);
        lo -= pad;
        hi += pad;
        auto toY = [&] (float ms) { return r.getBottom() - (ms - lo) / (hi - lo) * r.getHeight(); };

        g.setColour (colours::grid);
        for (int i = 1; i < 4; ++i)
            g.fillRect (r.getX(), r.getY() + r.getHeight() * (float) i / 4.0f, r.getWidth(), 1.0f);

        // Two LFO cycles across the width, scrolling with real time.
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        const float phaseNow = (float) std::fmod (now * rate, 1.0);
        const juce::Colour left = accent, right = juce::Colour (0xff6ec6ff);

        for (int v = 0; v < td.numVoices; ++v)
        {
            const auto& voice = td.voices[v];
            juce::Path path;
            const int steps = 160;
            for (int i = 0; i <= steps; ++i)
            {
                const float t = (float) i / (float) steps * 2.0f; // cycles
                float ph = (phaseNow + t) * voice.rateMul + voice.phase;
                ph -= std::floor (ph);
                float lfo;
                if (p.shape == gitto::ChorusShape::Sine)
                    lfo = std::sin (juce::MathConstants<float>::twoPi * ph);
                else if (p.shape == gitto::ChorusShape::Random)
                    lfo = 0.6f * std::sin (juce::MathConstants<float>::twoPi * ph) + 0.4f * std::sin (juce::MathConstants<float>::twoPi * ph * 2.7f + (float) v);
                else
                {
                    const float tri = 4.0f * std::abs (ph - 0.5f) - 1.0f;
                    lfo = tri * (1.12f - 0.12f * tri * tri);
                }
                if (td.fastLfoDepth > 0.0f)
                {
                    float fp = (phaseNow + t) * td.fastLfoRatio + voice.phase;
                    lfo += td.fastLfoDepth * std::sin (juce::MathConstants<float>::twoPi * fp);
                }
                const float x = r.getX() + (float) i / (float) steps * r.getWidth();
                const float y = toY (delayMs + voice.delayOffsetMs + depthMs * lfo);
                if (i == 0) path.startNewSubPath (x, y);
                else path.lineTo (x, y);
            }
            // Colour by where the voice sits in the stereo field.
            const float balance = juce::jlimit (0.0f, 1.0f, 0.5f + 0.5f * (std::abs (voice.outR) - std::abs (voice.outL)));
            g.setColour (left.interpolatedWith (right, balance).withAlpha (0.9f));
            g.strokePath (path, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setColour (colours::dim);
        g.setFont (font (9.5f));
        g.drawText (juce::String (hi, 1) + " ms", r.removeFromTop (12.0f), juce::Justification::centredLeft);
        g.drawText (juce::String (lo, 1) + " ms", r.removeFromBottom (12.0f), juce::Justification::centredLeft);
    }

private:
    ChorusProcessor& proc;
    gitto::Chorus model; // used only to resolve the mode into rate, depth and delay
};

juce::AudioProcessorEditor* ChorusProcessor::createEditor()
{
    LayoutSpec l;
    l.displayHeight = 150;
    l.rows = {
        { { "Character", { chooser ("type", "Type"), chooser ("mode", "Mode"), chooser ("shape", "LFO Shape") } },
          { "Tone", { knob ("tone", "Tone"), knob ("warmth", "Warmth") } } },
        { { "Manual Modulation", { knob ("rate", "Rate"), knob ("depth", "Depth"), knob ("delay", "Delay") } },
          { "Regeneration", { knob ("feedback", "Feedback", true) } },
          { "Output", { knob ("width", "Width"), knob ("mix", "Mix"), knob ("out", "Level", true) } } },
    };
    auto* editor = new GittoEditor (*this, std::move (l), std::make_unique<ChorusDisplay> (*this));
    editor->onTimer = [this, editor]
    {
        // Rate, Depth and Delay only apply in Manual mode; modes I to IV are fixed voicings.
        const bool manual = choice ("mode") == (int) gitto::ChorusMode::Manual;
        editor->setControlEnabled ("rate", manual);
        editor->setControlEnabled ("depth", manual);
        editor->setControlEnabled ("delay", manual);
    };
    return editor;
}

juce::AudioProcessor* createGittoChorus() { return new ChorusProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoChorus(); }
#endif
