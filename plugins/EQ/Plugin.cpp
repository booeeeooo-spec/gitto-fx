// Gitto FX EQ - plugin wrapper and interface.
#include "../../dsp/EQ.h"
#include "../../shared/Gitto.h"

namespace gittofx
{
namespace
{
constexpr int kBands = gitto::Equaliser::kNumBands;
juce::String bandId (int band, const char* name) { return "b" + juce::String (band + 1) + "_" + name; }

// Lock-free ring of recent mono samples for the spectrum display.
struct SpectrumFifo
{
    static constexpr int kSize = 8192;
    void push (const float* l, const float* r, int n) noexcept
    {
        int w = write.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            data[(size_t) w] = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i];
            w = (w + 1) & (kSize - 1);
        }
        write.store (w, std::memory_order_relaxed);
    }
    void copyLatest (float* dest, int n) const noexcept
    {
        const int w = write.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
            dest[i] = data[(size_t) ((w - n + i) & (kSize - 1))];
    }
    std::array<float, kSize> data {};
    std::atomic<int> write { 0 };
};
} // namespace

//==============================================================================
class EqProcessor : public GittoProcessor
{
public:
    EqProcessor()
        : GittoProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true),
                          createLayout())
    {
    }

    static params::Layout createLayout()
    {
        using namespace params;
        Layout layout;
        const juce::StringArray types { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass", "Tilt" };
        const juce::StringArray slopes { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" };
        const juce::StringArray stereo { "Stereo", "Mid", "Side", "Left", "Right" };
        const int defType[kBands] = { 3, 1, 0, 0, 0, 0, 2, 4 };
        const float defFreq[kBands] = { 30.0f, 100.0f, 250.0f, 700.0f, 2000.0f, 5000.0f, 10000.0f, 18000.0f };
        const bool defOn[kBands] = { false, true, true, true, true, true, true, false };

        for (int b = 0; b < kBands; ++b)
        {
            const juce::String n = "Band " + juce::String (b + 1) + " ";
            const bool shelfOrCut = defType[b] != 0;
            layout.add (boolParam (bandId (b, "on"), n + "On", defOn[b]));
            layout.add (choiceParam (bandId (b, "type"), n + "Type", types, defType[b]));
            layout.add (floatParam (bandId (b, "freq"), n + "Frequency", range (10.0f, 30000.0f, 1000.0f), defFreq[b], Unit::Hz));
            layout.add (floatParam (bandId (b, "gain"), n + "Gain", range (-30.0f, 30.0f), 0.0f, Unit::Db));
            layout.add (floatParam (bandId (b, "q"), n + "Q", range (0.1f, 30.0f, 1.0f), shelfOrCut ? 0.71f : 1.0f, Unit::None));
            layout.add (choiceParam (bandId (b, "slope"), n + "Slope", slopes, 1));
            layout.add (choiceParam (bandId (b, "stereo"), n + "Stereo Placement", stereo, 0));
            layout.add (floatParam (bandId (b, "dyn"), n + "Dynamic Range", range (-24.0f, 24.0f), 0.0f, Unit::Db));
            layout.add (floatParam (bandId (b, "thr"), n + "Dynamic Threshold", range (-80.0f, 0.0f), -30.0f, Unit::Db));
        }
        layout.add (floatParam ("out", "Output Gain", range (-24.0f, 24.0f), 0.0f, Unit::Db));
        layout.add (choiceParam ("analyzer", "Analyzer", { "Off", "Input", "Output", "Both" }, 3));
        return layout;
    }

    juce::String getProductName() const override { return "EQ"; }
    juce::Colour getAccent() const override { return juce::Colour (0xff35d0e6); }

    gitto::EqBandParams bandParams (int b) const
    {
        gitto::EqBandParams p;
        p.enabled = flag (bandId (b, "on"));
        p.type = (gitto::EqType) choice (bandId (b, "type"));
        p.freq = value (bandId (b, "freq"));
        p.gainDb = value (bandId (b, "gain"));
        p.q = value (bandId (b, "q"));
        p.slope = choice (bandId (b, "slope"));
        p.stereo = (gitto::EqStereo) choice (bandId (b, "stereo"));
        p.dynRangeDb = value (bandId (b, "dyn"));
        p.dynThreshDb = value (bandId (b, "thr"));
        return p;
    }

    void pushParams()
    {
        for (int b = 0; b < kBands; ++b)
            eq.setBand (b, bandParams (b));
        eq.setOutputGainDb (value ("out"));
    }

    void prepareToPlay (double sr, int) override
    {
        sampleRate = sr;
        pushParams();
        eq.prepare (sr);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        if (consumeDirty())
            pushParams();

        const int n = buffer.getNumSamples();
        float* l = buffer.getWritePointer (0);
        float* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
        preFifo.push (l, r, n);
        eq.process (l, r, n);
        postFifo.push (l, r, n);
    }

    std::vector<Preset> getPresets() const override
    {
        // type indices: 0 Bell, 1 Low Shelf, 2 High Shelf, 3 Low Cut, 4 High Cut
        return {
            { "Flat", {} },
            { "Vocal Presence", { { "b1_on", 1 }, { "b1_freq", 90 }, { "b3_freq", 280 }, { "b3_gain", -2.5f }, { "b3_q", 1.4f },
                                  { "b5_freq", 3200 }, { "b5_gain", 2.5f }, { "b5_q", 0.9f }, { "b7_freq", 11000 }, { "b7_gain", 3.0f } } },
            { "Kick Punch", { { "b1_on", 1 }, { "b1_freq", 32 }, { "b2_type", 0 }, { "b2_freq", 62 }, { "b2_gain", 3.5f }, { "b2_q", 1.2f },
                              { "b4_freq", 380 }, { "b4_gain", -4.0f }, { "b4_q", 1.6f }, { "b6_freq", 3800 }, { "b6_gain", 3.0f }, { "b6_q", 1.3f } } },
            { "808 Clean-Up", { { "b1_on", 1 }, { "b1_freq", 26 }, { "b1_slope", 2 }, { "b3_freq", 210 }, { "b3_gain", -3.0f }, { "b3_q", 1.1f },
                                { "b5_freq", 900 }, { "b5_gain", 1.5f }, { "b8_on", 1 }, { "b8_freq", 9000 } } },
            { "Mud Cut", { { "b3_freq", 300 }, { "b3_gain", -4.0f }, { "b3_q", 1.0f }, { "b4_freq", 520 }, { "b4_gain", -1.5f }, { "b4_q", 1.4f } } },
            { "Air Lift", { { "b7_freq", 12000 }, { "b7_gain", 4.0f }, { "b7_q", 0.6f }, { "b6_freq", 6500 }, { "b6_gain", -1.0f }, { "b6_q", 2.0f } } },
            { "Master Smile", { { "b2_freq", 80 }, { "b2_gain", 1.5f }, { "b4_freq", 600 }, { "b4_gain", -0.8f }, { "b4_q", 0.5f },
                                { "b7_freq", 9000 }, { "b7_gain", 1.5f } } },
            { "De-Harsh (Dynamic)", { { "b5_freq", 3000 }, { "b5_q", 2.2f }, { "b5_dyn", -8.0f }, { "b5_thr", -32.0f },
                                      { "b6_freq", 6500 }, { "b6_q", 2.5f }, { "b6_dyn", -6.0f }, { "b6_thr", -36.0f } } },
            { "Side Width", { { "b7_stereo", 2 }, { "b7_freq", 7000 }, { "b7_gain", 3.5f }, { "b2_stereo", 2 }, { "b2_type", 3 }, { "b2_freq", 140 } } },
        };
    }

    juce::AudioProcessorEditor* createEditor() override;

    gitto::Equaliser eq;
    SpectrumFifo preFifo, postFifo;
    double sampleRate = 48000.0;
};

//==============================================================================
class EqDisplay : public juce::Component
{
public:
    explicit EqDisplay (EqProcessor& p) : proc (p)
    {
        const int bins = kFftSize / 2;
        smoothedPre.assign ((size_t) bins, -120.0f);
        smoothedPost.assign ((size_t) bins, -120.0f);
        window.resize ((size_t) kFftSize);
        for (int i = 0; i < kFftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (kFftSize - 1));
    }

    std::function<void (int)> onSelect;
    int selected = 2;

    void paint (juce::Graphics& g) override
    {
        const auto r = plot();
        const float w = r.getWidth();
        const juce::Colour accent = proc.getAccent();

        // Grid.
        drawFrequencyGrid (g, r);
        g.setFont (font (9.5f));
        for (int db = -24; db <= 24; db += 12)
        {
            const float y = gainToY ((float) db);
            g.setColour (db == 0 ? colours::panelEdge.brighter (0.25f) : colours::grid);
            g.fillRect (r.getX(), y, w, 1.0f);
            g.setColour (colours::dim.withAlpha (0.7f));
            g.drawText ((db > 0 ? "+" : "") + juce::String (db), juce::Rectangle<float> (r.getX() + 3.0f, y - 12.0f, 30.0f, 11.0f), juce::Justification::centredLeft);
        }

        // Spectrum.
        const int mode = proc.choice ("analyzer");
        if (mode == 1 || mode == 3)
            drawSpectrum (g, proc.preFifo, smoothedPre, colours::dim.withAlpha (0.35f), false);
        if (mode == 2 || mode == 3)
            drawSpectrum (g, proc.postFifo, smoothedPost, accent.withAlpha (0.22f), true);

        // Band curves.
        const double sr = proc.sampleRate;
        const int points = juce::jmax (64, (int) w / 2);
        std::vector<float> total ((size_t) points, 0.0f), single ((size_t) points, 0.0f);
        std::vector<double> omega ((size_t) points);
        for (int i = 0; i < points; ++i)
            omega[(size_t) i] = gitto::kTwoPi * juce::jmin ((double) xToFreq ((float) i / (float) (points - 1) * w, w), sr * 0.499) / sr;

        for (int b = 0; b < kBands; ++b)
        {
            const auto p = proc.bandParams (b);
            if (! p.enabled)
                continue;
            gitto::BiquadCoefs c[gitto::EqBand::kMaxSections];
            const int sections = gitto::EqBand::design (p, (double) p.gainDb + proc.eq.getBand (b).getDynamicGainDb(), sr, c);
            for (int i = 0; i < points; ++i)
            {
                double mag = 1.0;
                for (int s = 0; s < sections; ++s)
                    mag *= c[s].magnitude (omega[(size_t) i]);
                const float db = (float) (20.0 * std::log10 (juce::jmax (mag, 1.0e-6)));
                total[(size_t) i] += db;
                if (b == selected)
                    single[(size_t) i] = db;
            }
        }

        auto makePath = [&] (const std::vector<float>& v)
        {
            juce::Path path;
            for (int i = 0; i < points; ++i)
            {
                const float x = r.getX() + (float) i / (float) (points - 1) * w;
                const float y = juce::jlimit (r.getY() - 2.0f, r.getBottom() + 2.0f, gainToY (v[(size_t) i]));
                if (i == 0) path.startNewSubPath (x, y);
                else path.lineTo (x, y);
            }
            return path;
        };

        if (proc.flag (bandId (selected, "on")))
        {
            auto fill = makePath (single);
            fill.lineTo (r.getRight(), gainToY (0.0f));
            fill.lineTo (r.getX(), gainToY (0.0f));
            fill.closeSubPath();
            g.setColour (bandColour (selected).withAlpha (0.16f));
            g.fillPath (fill);
        }

        g.setColour (accent);
        g.strokePath (makePath (total), juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Band handles.
        for (int b = 0; b < kBands; ++b)
        {
            const auto pos = nodePosition (b);
            const bool on = proc.flag (bandId (b, "on"));
            const float radius = b == selected ? 9.0f : 7.5f;
            const auto col = bandColour (b);
            g.setColour (on ? col.withAlpha (b == selected ? 1.0f : 0.8f) : colours::panelEdge);
            g.fillEllipse (pos.x - radius, pos.y - radius, radius * 2.0f, radius * 2.0f);
            if (b == selected)
            {
                g.setColour (colours::text);
                g.drawEllipse (pos.x - radius - 2.0f, pos.y - radius - 2.0f, radius * 2.0f + 4.0f, radius * 2.0f + 4.0f, 1.4f);
            }
            g.setColour (on ? colours::background : colours::dim);
            g.setFont (font (10.0f, true));
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (pos.x - radius, pos.y - radius, radius * 2.0f, radius * 2.0f), juce::Justification::centred);
        }

        g.setColour (colours::dim.withAlpha (0.8f));
        g.setFont (font (9.5f));
        g.drawText ("Drag a band to move it  |  double-click to switch it on or off  |  scroll to change Q",
                    r.reduced (6.0f, 3.0f).removeFromTop (12.0f), juce::Justification::centredRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = -1;
        float best = 16.0f;
        for (int b = 0; b < kBands; ++b)
        {
            const float d = nodePosition (b).getDistanceFrom (e.position);
            if (d < best)
            {
                best = d;
                dragging = b;
            }
        }
        if (dragging >= 0)
        {
            select (dragging);
            for (auto* name : { "freq", "gain" })
                if (auto* p = proc.apvts.getParameter (bandId (dragging, name)))
                    p->beginChangeGesture();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;
        const auto r = plot();
        setParam (bandId (dragging, "freq"), juce::jlimit (20.0f, 20000.0f, xToFreq (e.position.x - r.getX(), r.getWidth())));
        if (gitto::EqBand::typeHasGain ((gitto::EqType) proc.choice (bandId (dragging, "type"))))
            setParam (bandId (dragging, "gain"), juce::jlimit (-30.0f, 30.0f, yToGain (e.position.y)));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0)
            for (auto* name : { "freq", "gain" })
                if (auto* p = proc.apvts.getParameter (bandId (dragging, name)))
                    p->endChangeGesture();
        dragging = -1;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        for (int b = 0; b < kBands; ++b)
            if (nodePosition (b).getDistanceFrom (e.position) < 14.0f)
            {
                if (auto* p = proc.apvts.getParameter (bandId (b, "on")))
                {
                    p->beginChangeGesture();
                    p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.0f : 1.0f);
                    p->endChangeGesture();
                }
                select (b);
                return;
            }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        for (int b = 0; b < kBands; ++b)
            if (nodePosition (b).getDistanceFrom (e.position) < 18.0f)
            {
                const float q = proc.value (bandId (b, "q"));
                if (auto* p = proc.apvts.getParameter (bandId (b, "q")))
                {
                    p->beginChangeGesture();
                    p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (0.1f, 30.0f, q * std::pow (2.0f, wheel.deltaY * 1.5f))));
                    p->endChangeGesture();
                }
                select (b);
                return;
            }
    }

private:
    static constexpr int kFftSize = 4096;

    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (6.0f, 6.0f); }
    float gainToY (float db) const
    {
        const auto r = plot();
        return r.getCentreY() - db / 30.0f * (r.getHeight() * 0.5f - 8.0f);
    }
    float yToGain (float y) const
    {
        const auto r = plot();
        return (r.getCentreY() - y) / (r.getHeight() * 0.5f - 8.0f) * 30.0f;
    }
    juce::Point<float> nodePosition (int b) const
    {
        const auto r = plot();
        const auto type = (gitto::EqType) proc.choice (bandId (b, "type"));
        const float gain = gitto::EqBand::typeHasGain (type) ? proc.value (bandId (b, "gain")) : 0.0f;
        return { r.getX() + freqToX (juce::jlimit (20.0f, 20000.0f, proc.value (bandId (b, "freq"))), r.getWidth()), gainToY (gain) };
    }
    static juce::Colour bandColour (int b)
    {
        static const juce::uint32 cols[kBands] = { 0xffff6b6b, 0xffffa94d, 0xffffd43b, 0xff69db7c, 0xff38d9a9, 0xff4dabf7, 0xff9775fa, 0xfff783ac };
        return juce::Colour (cols[b]);
    }
    void select (int b)
    {
        if (b != selected)
        {
            selected = b;
            if (onSelect)
                onSelect (b);
        }
    }
    void setParam (const juce::String& id, float v)
    {
        if (auto* p = proc.apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    }

    void drawSpectrum (juce::Graphics& g, const SpectrumFifo& fifo, std::vector<float>& smoothed, juce::Colour colour, bool filled)
    {
        std::vector<float> samples ((size_t) kFftSize);
        fifo.copyLatest (samples.data(), kFftSize);
        std::vector<std::complex<float>> spec ((size_t) kFftSize);
        for (int i = 0; i < kFftSize; ++i)
            spec[(size_t) i] = samples[(size_t) i] * window[(size_t) i];
        gitto::fft (spec);

        const auto r = plot();
        const double sr = proc.sampleRate;
        const int bins = kFftSize / 2;
        for (int k = 1; k < bins; ++k)
        {
            const float mag = std::abs (spec[(size_t) k]) * (4.0f / (float) kFftSize);
            const float db = 20.0f * std::log10 (juce::jmax (mag, 1.0e-7f));
            float& s = smoothed[(size_t) k];
            s = db > s ? db : s + (db - s) * 0.12f;
        }

        juce::Path path;
        bool started = false;
        const int steps = (int) r.getWidth() / 2;
        for (int i = 0; i <= steps; ++i)
        {
            const float x = (float) i / (float) steps * r.getWidth();
            const float f0 = xToFreq (x, r.getWidth()), f1 = xToFreq (x + 2.0f, r.getWidth());
            const int k0 = juce::jlimit (1, bins - 1, (int) (f0 * kFftSize / sr));
            const int k1 = juce::jlimit (k0 + 1, bins, (int) std::ceil (f1 * kFftSize / sr));
            float peak = -120.0f;
            for (int k = k0; k < k1; ++k)
                peak = juce::jmax (peak, smoothed[(size_t) k]);
            // Tilt by 4.5 dB/octave around 1 kHz so typical music looks level.
            peak += 4.5f * std::log2 (juce::jmax (f0, 20.0f) / 1000.0f);
            const float y = juce::jmap (juce::jlimit (-96.0f, 0.0f, peak), -96.0f, 0.0f, r.getBottom(), r.getY() + 8.0f);
            if (! started)
            {
                path.startNewSubPath (r.getX() + x, y);
                started = true;
            }
            else
                path.lineTo (r.getX() + x, y);
        }
        if (filled)
        {
            path.lineTo (r.getRight(), r.getBottom());
            path.lineTo (r.getX(), r.getBottom());
            path.closeSubPath();
            g.setColour (colour);
            g.fillPath (path);
        }
        else
        {
            g.setColour (colour);
            g.strokePath (path, juce::PathStrokeType (1.2f));
        }
    }

    EqProcessor& proc;
    std::vector<float> smoothedPre, smoothedPost, window;
    int dragging = -1;
};

//==============================================================================
static LayoutSpec eqLayout (int band)
{
    LayoutSpec l;
    l.displayHeight = 250;
    l.rows = { {
        { "Band " + juce::String (band + 1),
          { toggle (bandId (band, "on"), "Active"), chooser (bandId (band, "type"), "Shape"), knob (bandId (band, "freq"), "Frequency"),
            knob (bandId (band, "gain"), "Gain", true), knob (bandId (band, "q"), "Q"), chooser (bandId (band, "slope"), "Slope"),
            chooser (bandId (band, "stereo"), "Placement") } },
        { "Dynamic", { knob (bandId (band, "dyn"), "Range", true), knob (bandId (band, "thr"), "Threshold") } },
        { "Output", { knob ("out", "Gain", true), chooser ("analyzer", "Analyzer") } },
    } };
    return l;
}

juce::AudioProcessorEditor* EqProcessor::createEditor()
{
    auto display = std::make_unique<EqDisplay> (*this);
    auto* d = display.get();
    auto* editor = new GittoEditor (*this, eqLayout (d->selected), std::move (display));
    d->onSelect = [editor] (int band) { editor->setLayout (eqLayout (band)); };
    editor->onTimer = [this, editor, d]
    {
        // Grey out the controls that do nothing for the selected band's shape.
        const int b = d->selected;
        const auto type = (gitto::EqType) choice (bandId (b, "type"));
        const bool isCut = type == gitto::EqType::LowCut || type == gitto::EqType::HighCut;
        editor->setControlEnabled (bandId (b, "gain"), gitto::EqBand::typeHasGain (type));
        editor->setControlEnabled (bandId (b, "slope"), isCut);
        editor->setControlEnabled (bandId (b, "q"), type != gitto::EqType::Tilt);
        editor->setControlEnabled (bandId (b, "dyn"), gitto::EqBand::typeCanBeDynamic (type));
        editor->setControlEnabled (bandId (b, "thr"), gitto::EqBand::typeCanBeDynamic (type));
    };
    return editor;
}

juce::AudioProcessor* createGittoEq() { return new EqProcessor(); }
} // namespace gittofx

#ifndef GITTO_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return gittofx::createGittoEq(); }
#endif
