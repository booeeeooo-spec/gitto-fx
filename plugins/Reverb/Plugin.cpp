// Gitto FX Reverb - plugin wrapper and interface.
#include "../../dsp/Reverb.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
class ReverbProcessor : public GittoProcessor
{
public:
    ReverbProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
        allowMonoToStereo = true;
    }

    static juce::StringArray modeNames()
    {
        return { "Concert Hall", "Bright Hall", "Plate", "Room", "Chamber", "Ambience", "Cathedral",
                 "Random Hall", "Chorus Hall", "Dark Chamber", "Lo-Fi Hall", "Gated" };
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        layout.add (choiceParam ("mode", "Mode", modeNames(), 0));
        layout.add (choiceParam ("color", "Color", { "Vintage", "Warm", "Modern" }, 2));
        layout.add (floatParam ("decay", "Decay", range (0.2f, 60.0f, 3.0f), 2.2f, Unit::Sec));
        layout.add (floatParam ("size", "Size", range (0.0f, 1.0f), 0.5f, Unit::Percent));
        layout.add (floatParam ("predelay", "Pre-Delay", range (0.0f, 500.0f, 60.0f), 20.0f, Unit::Ms));
        layout.add (floatParam ("early", "Early Reflections", range (0.0f, 1.0f), 0.5f, Unit::Percent));
        layout.add (floatParam ("mix", "Mix", range (0.0f, 1.0f), 0.3f, Unit::Percent));
        layout.add (floatParam ("bassmult", "Bass Multiplier", range (0.25f, 4.0f, 1.0f), 1.2f, Unit::Times));
        layout.add (floatParam ("bassxover", "Bass Crossover", range (100.0f, 2000.0f, 500.0f), 400.0f, Unit::Hz));
        layout.add (floatParam ("highmult", "High Multiplier", range (0.1f, 1.0f), 0.5f, Unit::Times));
        layout.add (floatParam ("damp", "Damping Frequency", range (1000.0f, 20000.0f, 5000.0f), 6000.0f, Unit::Hz));
        layout.add (floatParam ("earlydiff", "Early Diffusion", range (0.0f, 1.0f), 0.7f, Unit::Percent));
        layout.add (floatParam ("latediff", "Late Diffusion", range (0.0f, 1.0f), 0.6f, Unit::Percent));
        layout.add (floatParam ("modrate", "Modulation Rate", range (0.05f, 5.0f, 0.8f), 0.5f, Unit::Hz));
        layout.add (floatParam ("moddepth", "Modulation Depth", range (0.0f, 1.0f), 0.3f, Unit::Percent));
        layout.add (floatParam ("lowcut", "Low Cut", range (20.0f, 1000.0f, 150.0f), 20.0f, Unit::Hz));
        layout.add (floatParam ("highcut", "High Cut", range (1000.0f, 20000.0f, 6000.0f), 20000.0f, Unit::Hz));
        layout.add (floatParam ("width", "Width", range (0.0f, 2.0f), 1.0f, Unit::Percent));
        layout.add (boolParam ("freeze", "Freeze", false));
        return layout;
    }

    juce::String getProductName() const override { return "Reverb"; }
    juce::Colour getAccent() const override { return juce::Colour (0xffa98bff); }
    double getTailLengthSeconds() const override { return (double) value ("decay") + 0.5; }

    gitto::ReverbParams readParams() const
    {
        gitto::ReverbParams p;
        p.mode = (gitto::ReverbMode) choice ("mode");
        p.color = (gitto::ReverbColor) choice ("color");
        p.decaySec = value ("decay");
        p.size = value ("size");
        p.predelayMs = value ("predelay");
        p.early = value ("early");
        p.mix = value ("mix");
        p.bassMult = value ("bassmult");
        p.bassXoverHz = value ("bassxover");
        p.highMult = value ("highmult");
        p.dampHz = value ("damp");
        p.earlyDiff = value ("earlydiff");
        p.lateDiff = value ("latediff");
        p.modRateHz = value ("modrate");
        p.modDepth = value ("moddepth");
        p.lowCutHz = value ("lowcut");
        p.highCutHz = value ("highcut");
        p.width = value ("width");
        p.freeze = flag ("freeze");
        return p;
    }

    void prepareToPlay (double sr, int) override
    {
        reverb.setParams (readParams());
        reverb.prepare (sr);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        if (consumeDirty())
            reverb.setParams (readParams());

        const int n = buffer.getNumSamples();
        float* l = buffer.getWritePointer (0);
        float* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
        if (r != nullptr && getTotalNumInputChannels() < 2)
            juce::FloatVectorOperations::copy (r, l, n); // mono in, stereo out
        reverb.process (l, r, n);
    }

    std::vector<Preset> getPresets() const override
    {
        // mode: 0 Concert Hall, 1 Bright Hall, 2 Plate, 3 Room, 4 Chamber, 5 Ambience, 6 Cathedral,
        //       7 Random Hall, 8 Chorus Hall, 9 Dark Chamber, 10 Lo-Fi Hall, 11 Gated
        return {
            { "Default Hall", {} },
            { "Vocal Plate", { { "mode", 2 }, { "decay", 1.8f }, { "predelay", 35 }, { "mix", 0.22f }, { "lowcut", 180 }, { "highcut", 11000 }, { "highmult", 0.7f } } },
            { "Snare Room", { { "mode", 3 }, { "decay", 0.7f }, { "size", 0.45f }, { "predelay", 5 }, { "mix", 0.25f }, { "early", 0.8f } } },
            { "Tight Ambience", { { "mode", 5 }, { "decay", 0.5f }, { "size", 0.4f }, { "predelay", 0 }, { "mix", 0.2f }, { "early", 1.0f } } },
            { "Big Cinematic Hall", { { "mode", 0 }, { "decay", 5.5f }, { "size", 0.85f }, { "predelay", 45 }, { "mix", 0.4f }, { "bassmult", 1.4f }, { "moddepth", 0.4f } } },
            { "Cathedral Wash", { { "mode", 6 }, { "decay", 9.0f }, { "size", 0.9f }, { "predelay", 60 }, { "mix", 0.45f }, { "highmult", 0.4f }, { "damp", 4500 } } },
            { "Lush Pad Space", { { "mode", 8 }, { "decay", 6.0f }, { "size", 0.7f }, { "mix", 0.5f }, { "modrate", 0.35f }, { "moddepth", 0.6f }, { "lowcut", 150 } } },
            { "Dark Trap Chamber", { { "mode", 9 }, { "decay", 2.6f }, { "predelay", 25 }, { "mix", 0.28f }, { "lowcut", 220 }, { "damp", 3500 } } },
            { "Lo-Fi Memory", { { "mode", 10 }, { "color", 0 }, { "decay", 3.2f }, { "mix", 0.35f }, { "moddepth", 0.7f }, { "highcut", 6000 } } },
            { "80s Gated Snare", { { "mode", 11 }, { "decay", 0.9f }, { "mix", 0.45f }, { "predelay", 0 }, { "early", 0.6f } } },
            { "Game Menu Shimmer", { { "mode", 1 }, { "decay", 4.0f }, { "size", 0.75f }, { "mix", 0.38f }, { "highmult", 0.9f }, { "damp", 12000 }, { "lowcut", 250 } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Reverb reverb;
};

//==============================================================================
// Shows what the settings mean in time: pre-delay gap, early reflections, and how
// long the low, mid and high ranges take to die away.
class ReverbDisplay : public juce::Component
{
public:
    explicit ReverbDisplay (ReverbProcessor& p) : proc (p) {}

    void paint (juce::Graphics& g) override
    {
        const juce::Colour accent = proc.getAccent();
        const auto area = getLocalBounds().toFloat().reduced (8.0f);
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        const auto r = area.reduced (10.0f, 8.0f);

        const auto p = proc.readParams();
        const auto& md = gitto::Reverb::modeData (p.mode);
        const bool gated = p.mode == gitto::ReverbMode::Gated;
        const float sizeScale = (0.4f + 1.2f * p.size) * md.sizeScale;
        const float decay = gated ? juce::jlimit (0.05f, 2.0f, p.decaySec) * 0.25f : p.decaySec;
        const float span = juce::jlimit (0.3f, 30.0f, (p.predelayMs * 0.001f + decay * juce::jmax (1.0f, p.bassMult)) * 1.05f);
        auto toX = [&] (float seconds) { return r.getX() + juce::jlimit (0.0f, 1.0f, seconds / span) * r.getWidth(); };
        auto toY = [&] (float db) { return r.getY() + juce::jlimit (0.0f, 1.0f, -db / 60.0f) * r.getHeight(); };

        // Time grid.
        g.setFont (font (9.5f));
        const float step = span > 12.0f ? 5.0f : (span > 5.0f ? 2.0f : (span > 2.0f ? 1.0f : (span > 0.8f ? 0.25f : 0.1f)));
        for (float t = step; t < span; t += step)
        {
            g.setColour (colours::grid);
            g.fillRect (toX (t), r.getY(), 1.0f, r.getHeight());
            g.setColour (colours::dim.withAlpha (0.7f));
            g.drawText (juce::String (t, step < 1.0f ? 2 : 0) + " s", juce::Rectangle<float> (toX (t) + 3.0f, r.getBottom() - 12.0f, 40.0f, 11.0f), juce::Justification::centredLeft);
        }
        for (float db : { -20.0f, -40.0f })
        {
            g.setColour (colours::grid);
            g.fillRect (r.getX(), toY (db), r.getWidth(), 1.0f);
        }

        const float pre = p.predelayMs * 0.001f;

        // Direct sound and early reflections.
        g.setColour (colours::text);
        g.fillRect (toX (0.0f), toY (0.0f), 2.0f, r.getBottom() - toY (0.0f));
        if (p.early > 0.01f && ! p.freeze)
        {
            g.setColour (accent.withAlpha (0.8f));
            for (int t = 0; t < gitto::Reverb::kEarlyTaps; ++t)
            {
                const float time = pre + md.earlyMs[t] * 0.001f * sizeScale;
                const float level = -8.0f + 20.0f * std::log10 (juce::jmax (0.02f, md.earlyGain[t] * p.early * md.earlyLevel));
                g.fillRect (toX (time), toY (level), 1.5f, r.getBottom() - toY (level));
            }
        }

        // Decay slopes for the three frequency ranges.
        auto slope = [&] (float rt, juce::Colour c, float thickness, bool fill)
        {
            juce::Path path;
            const float start = pre + md.lineMs[0] * 0.001f * sizeScale * 0.5f;
            const float endTime = gated ? start + decay : start + rt;
            path.startNewSubPath (toX (start), toY (-10.0f));
            path.lineTo (toX (endTime), gated ? toY (-12.0f) : toY (-60.0f));
            if (gated)
                path.lineTo (toX (endTime + 0.03f), toY (-60.0f));
            if (fill)
            {
                juce::Path f (path);
                f.lineTo (toX (start), toY (-60.0f));
                f.closeSubPath();
                g.setColour (c.withAlpha (0.14f));
                g.fillPath (f);
            }
            g.setColour (c);
            g.strokePath (path, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };
        if (p.freeze)
        {
            g.setColour (accent);
            g.fillRect (r.getX(), toY (-10.0f), r.getWidth(), 2.0f);
            g.setFont (font (11.0f, true));
            g.drawText ("FROZEN", r, juce::Justification::centred);
        }
        else
        {
            slope (decay * p.bassMult, juce::Colour (0xffff8a4c), 1.3f, false);
            slope (decay * p.highMult, juce::Colour (0xff4dabf7), 1.3f, false);
            slope (decay, accent, 2.2f, true);
        }

        // Legend.
        g.setFont (font (9.5f, true));
        auto legend = r.withTrimmedLeft (r.getWidth() - 150.0f).removeFromTop (12.0f);
        const std::pair<const char*, juce::Colour> keys[] = { { "LOW", juce::Colour (0xffff8a4c) }, { "MID", accent }, { "HIGH", juce::Colour (0xff4dabf7) } };
        for (auto& [name, colour] : keys)
        {
            auto cell = legend.removeFromLeft (50.0f);
            g.setColour (colour);
            g.fillRoundedRectangle (cell.removeFromLeft (10.0f).withSizeKeepingCentre (8.0f, 3.0f), 1.5f);
            g.setColour (colours::dim);
            g.drawText (name, cell.withTrimmedLeft (3.0f), juce::Justification::centredLeft);
        }
        g.setColour (colours::text);
        g.setFont (font (13.0f, true));
        g.drawText (ReverbProcessor::modeNames()[(int) p.mode].toUpperCase(), r.withHeight (16.0f).withTrimmedLeft (10.0f), juce::Justification::centredLeft);
    }

private:
    ReverbProcessor& proc;
};

juce::AudioProcessorEditor* ReverbProcessor::createEditor()
{
    LayoutSpec l;
    l.displayHeight = 170;
    l.rows = {
        { { "Space", { chooser ("mode", "Mode"), chooser ("color", "Color") } },
          { "Main", { knob ("decay", "Decay"), knob ("size", "Size"), knob ("predelay", "Pre-Delay"), knob ("early", "Early"), knob ("mix", "Mix") } },
          { "Modulation", { knob ("modrate", "Rate"), knob ("moddepth", "Depth") } } },
        { { "Decay Shape", { knob ("bassmult", "Bass Mult"), knob ("bassxover", "Bass Xover"), knob ("highmult", "High Mult"), knob ("damp", "Damping") } },
          { "Diffusion", { knob ("earlydiff", "Early"), knob ("latediff", "Late") } },
          { "Output", { knob ("lowcut", "Low Cut"), knob ("highcut", "High Cut"), knob ("width", "Width"), toggle ("freeze", "Freeze") } } },
    };
    return new GittoEditor (*this, std::move (l), std::make_unique<ReverbDisplay> (*this));
}

juce::AudioProcessor* createGittoReverb() { return new ReverbProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoReverb(); }
#endif
