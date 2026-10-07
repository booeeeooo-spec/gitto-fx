// Gitto FX - shared plugin framework: look and feel, parameter helpers,
// processor base class and the editor that every plugin in the suite uses.
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>
#include <map>

namespace gittofx
{
//==============================================================================
namespace colours
{
const juce::Colour background { 0xff11131a };
const juce::Colour panel { 0xff1a1d27 };
const juce::Colour panelEdge { 0xff2a2f3d };
const juce::Colour track { 0xff2b303f };
const juce::Colour text { 0xffe9ebf2 };
const juce::Colour dim { 0xff8b92a5 };
const juce::Colour grid { 0xff262a37 };
}

inline juce::Font font (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

//==============================================================================
// Parameter helpers. Every value is shown with a sensible unit and precision.
namespace params
{
using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;
using Attr = juce::AudioParameterFloatAttributes;

inline juce::NormalisableRange<float> range (float lo, float hi, float centre = 0.0f, float step = 0.0f)
{
    juce::NormalisableRange<float> r (lo, hi, step);
    if (centre > lo && centre < hi)
        r.setSkewForCentre (centre);
    return r;
}

inline juce::String trimNumber (float v, int decimals) { return juce::String (v, decimals); }

enum class Unit { None, Db, DbAmount, Hz, Ms, Sec, Percent, Ratio, Times };

inline std::unique_ptr<juce::AudioParameterFloat> floatParam (const juce::String& id, const juce::String& name,
                                                              juce::NormalisableRange<float> r, float def, Unit unit)
{
    Attr attr;
    switch (unit)
    {
        case Unit::Db:
            attr = attr.withStringFromValueFunction ([] (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; });
            break;
        case Unit::DbAmount: // a size in dB (knee width, range), shown without a sign
            attr = attr.withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1) + " dB"; });
            break;
        case Unit::Hz:
            attr = attr.withStringFromValueFunction ([] (float v, int)
            {
                if (v >= 1000.0f) return juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz";
                return juce::String (v, v < 100.0f ? 1 : 0) + " Hz";
            }).withValueFromStringFunction ([] (const juce::String& s)
            {
                const float v = s.getFloatValue();
                return s.containsIgnoreCase ("k") ? v * 1000.0f : v;
            });
            break;
        case Unit::Ms:
            attr = attr.withStringFromValueFunction ([] (float v, int)
            {
                if (v >= 1000.0f) return juce::String (v / 1000.0f, 2) + " s";
                return juce::String (v, v < 10.0f ? 2 : (v < 100.0f ? 1 : 0)) + " ms";
            }).withValueFromStringFunction ([] (const juce::String& s)
            {
                const float v = s.getFloatValue();
                return (s.containsIgnoreCase ("s") && ! s.containsIgnoreCase ("ms")) ? v * 1000.0f : v;
            });
            break;
        case Unit::Sec:
            attr = attr.withStringFromValueFunction ([] (float v, int) { return juce::String (v, v < 10.0f ? 2 : 1) + " s"; });
            break;
        case Unit::Percent:
            attr = attr.withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; })
                       .withValueFromStringFunction ([] (const juce::String& s) { return s.getFloatValue() / 100.0f; });
            break;
        case Unit::Ratio:
            attr = attr.withStringFromValueFunction ([] (float v, int) { return v >= 19.9f ? juce::String ("Limit") : juce::String (v, 1) + " : 1"; });
            break;
        case Unit::Times:
            attr = attr.withStringFromValueFunction ([] (float v, int) { return juce::String (v, 2) + " x"; });
            break;
        case Unit::None:
            attr = attr.withStringFromValueFunction ([] (float v, int) { return juce::String (v, 2); });
            break;
    }
    return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, r, def, attr);
}

inline std::unique_ptr<juce::AudioParameterChoice> choiceParam (const juce::String& id, const juce::String& name,
                                                                const juce::StringArray& choices, int def)
{
    return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, choices, def);
}

inline std::unique_ptr<juce::AudioParameterBool> boolParam (const juce::String& id, const juce::String& name, bool def)
{
    return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def);
}
} // namespace params

//==============================================================================
struct Preset
{
    juce::String name;
    std::vector<std::pair<juce::String, float>> values; // parameter id -> value in real units (choices: index)
};

//==============================================================================
class GittoProcessor : public juce::AudioProcessor,
                       private juce::AudioProcessorValueTreeState::Listener,
                       private juce::AsyncUpdater
{
public:
    GittoProcessor (const BusesProperties& buses, params::Layout layout)
        : juce::AudioProcessor (buses),
          apvts (*this, nullptr, "GittoFX", std::move (layout))
    {
        for (auto* p : getParameters())
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
                apvts.addParameterListener (withId->paramID, this);
    }

    ~GittoProcessor() override
    {
        cancelPendingUpdate();
        for (auto* p : getParameters())
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
                apvts.removeParameterListener (withId->paramID, this);
    }

    //==============================================================================
    virtual juce::String getProductName() const = 0; // e.g. "Reverb"
    virtual juce::Colour getAccent() const = 0;
    virtual std::vector<Preset> getPresets() const { return {}; }

    float value (const juce::String& id) const
    {
        if (auto* v = apvts.getRawParameterValue (id))
            return v->load (std::memory_order_relaxed);
        jassertfalse;
        return 0.0f;
    }
    int choice (const juce::String& id) const { return juce::roundToInt (value (id)); }
    bool flag (const juce::String& id) const { return value (id) > 0.5f; }

    // True once per change of any parameter; the audio thread uses it to refresh the DSP.
    bool consumeDirty() noexcept { return dirty.exchange (false); }
    void markDirty() noexcept { dirty.store (true); }

    void applyPreset (const Preset& preset)
    {
        for (auto* p : getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                rp->setValueNotifyingHost (rp->getDefaultValue());
        for (auto& [id, v] : preset.values)
            if (auto* rp = apvts.getParameter (id))
                rp->setValueNotifyingHost (rp->convertTo0to1 (v));
        currentPresetName = preset.name;
    }

    // Reports a latency change to the host from the message thread.
    void requestLatency (int samples)
    {
        if (pendingLatency.exchange (samples) != samples)
            triggerAsyncUpdate();
    }

    //==============================================================================
    const juce::String getName() const override { return "Gitto FX " + getProductName(); }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    bool hasEditor() const override { return true; }
    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        const auto in = layouts.getMainInputChannelSet();
        const auto out = layouts.getMainOutputChannelSet();
        if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
            return false;
        if (in != out && ! (allowMonoToStereo && in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo()))
            return false;
        if (layouts.inputBuses.size() > 1)
        {
            const auto sc = layouts.getChannelSet (true, 1);
            if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo())
                return false;
        }
        return true;
    }

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        auto state = apvts.copyState();
        state.setProperty ("presetName", currentPresetName, nullptr);
        state.setProperty ("uiScale", uiScale, nullptr);
        saveExtraState (state);
        if (auto xml = state.createXml())
            copyXmlToBinary (*xml, dest);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            if (state.isValid() && state.hasType (apvts.state.getType()))
            {
                currentPresetName = state.getProperty ("presetName", "").toString();
                uiScale = (float) (double) state.getProperty ("uiScale", 1.0);
                apvts.replaceState (state);
                loadExtraState (apvts.state);
                markDirty();
            }
        }
    }

    // For plugins that keep something other than parameters (the Shaper's drawn curve).
    virtual void saveExtraState (juce::ValueTree&) {}
    virtual void loadExtraState (const juce::ValueTree&) {}

    juce::AudioProcessorValueTreeState apvts;
    juce::String currentPresetName;
    float uiScale = 1.0f;
    bool allowMonoToStereo = false;

private:
    void parameterChanged (const juce::String&, float) override { dirty.store (true); }
    void handleAsyncUpdate() override { setLatencySamples (pendingLatency.load()); }

    std::atomic<bool> dirty { true };
    std::atomic<int> pendingLatency { 0 };
};

//==============================================================================
class GittoLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit GittoLookAndFeel (juce::Colour accentColour) : accent (accentColour)
    {
        setColour (juce::ResizableWindow::backgroundColourId, colours::background);
        setColour (juce::Slider::textBoxTextColourId, colours::text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.4f));
        setColour (juce::Label::textColourId, colours::text);
        setColour (juce::Label::textWhenEditingColourId, colours::text);
        setColour (juce::Label::backgroundWhenEditingColourId, colours::background);
        setColour (juce::Label::outlineWhenEditingColourId, accent);
        setColour (juce::TextEditor::textColourId, colours::text);
        setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.4f));
        setColour (juce::CaretComponent::caretColourId, accent);
        setColour (juce::ComboBox::backgroundColourId, colours::background);
        setColour (juce::ComboBox::textColourId, colours::text);
        setColour (juce::ComboBox::outlineColourId, colours::panelEdge);
        setColour (juce::ComboBox::arrowColourId, colours::dim);
        setColour (juce::PopupMenu::backgroundColourId, colours::panel);
        setColour (juce::PopupMenu::textColourId, colours::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.25f));
        setColour (juce::PopupMenu::highlightedTextColourId, colours::text);
        setColour (juce::TextButton::buttonColourId, colours::background);
        setColour (juce::TextButton::buttonOnColourId, accent);
        setColour (juce::TextButton::textColourOffId, colours::dim);
        setColour (juce::TextButton::textColourOnId, colours::background);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (4.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float angle = startAngle + pos * (endAngle - startAngle);
        const float stroke = juce::jmax (3.0f, radius * 0.14f);
        const float arcRadius = radius - stroke * 0.5f;
        const float alpha = slider.isEnabled() ? 1.0f : 0.3f;

        juce::Path trackArc;
        trackArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
        g.setColour (colours::track.withMultipliedAlpha (alpha));
        g.strokePath (trackArc, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Bipolar controls fill from the centre, everything else from the start.
        const bool bipolar = slider.getProperties().getWithDefault ("bipolar", false);
        const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
        if (std::abs (angle - from) > 0.01f)
        {
            juce::Path valueArc;
            valueArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
            g.setColour (accent.withMultipliedAlpha (alpha));
            g.strokePath (valueArc, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        const float capRadius = arcRadius - stroke * 1.1f;
        g.setColour (colours::panelEdge.withMultipliedAlpha (alpha));
        g.fillEllipse (centre.x - capRadius, centre.y - capRadius, capRadius * 2.0f, capRadius * 2.0f);
        g.setColour (colours::background.withMultipliedAlpha (alpha));
        g.fillEllipse (centre.x - capRadius + 1.5f, centre.y - capRadius + 1.5f, capRadius * 2.0f - 3.0f, capRadius * 2.0f - 3.0f);

        juce::Path pointer;
        pointer.addRoundedRectangle (-1.5f, -capRadius + 3.0f, 3.0f, capRadius * 0.5f, 1.5f);
        g.setColour (colours::text.withMultipliedAlpha (alpha));
        g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
        const float alpha = box.isEnabled() ? 1.0f : 0.35f;
        g.setColour (colours::background.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour ((box.hasKeyboardFocus (true) ? accent : colours::panelEdge).withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);

        juce::Path arrow;
        const float ax = (float) width - 14.0f, ay = (float) height * 0.5f;
        arrow.startNewSubPath (ax - 4.0f, ay - 2.0f);
        arrow.lineTo (ax, ay + 2.5f);
        arrow.lineTo (ax + 4.0f, ay - 2.0f);
        g.setColour (colours::dim.withMultipliedAlpha (alpha));
        g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    juce::Label* createSliderTextBox (juce::Slider& slider) override
    {
        auto* l = juce::LookAndFeel_V4::createSliderTextBox (slider);
        l->setFont (font (12.5f));
        l->setColour (juce::Label::textColourId, colours::text);
        l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        return l;
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (13.0f); }
    juce::Font getPopupMenuFont() override { return font (13.5f); }
    juce::Font getLabelFont (juce::Label& l) override { return font (l.getFont().getHeight()); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return font (12.0f, true); }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (8, 0, box.getWidth() - 28, box.getHeight());
        label.setFont (getComboBoxFont (box));
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        const bool on = b.getToggleState();
        const float alpha = b.isEnabled() ? 1.0f : 0.35f;
        auto fill = on ? accent : colours::background;
        if (down) fill = fill.darker (0.2f);
        else if (over) fill = fill.brighter (0.08f);
        g.setColour (fill.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour ((on ? accent : colours::panelEdge).withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
    }

    juce::Colour accent;
};

//==============================================================================
// Layout description. A plugin lists rows of titled sections; the editor does the rest.
struct ControlSpec
{
    enum class Type { Knob, Choice, Toggle };
    Type type;
    juce::String paramId, label;
    bool bipolar = false;
};

inline ControlSpec knob (const juce::String& id, const juce::String& label, bool bipolar = false) { return { ControlSpec::Type::Knob, id, label, bipolar }; }
inline ControlSpec chooser (const juce::String& id, const juce::String& label) { return { ControlSpec::Type::Choice, id, label, false }; }
inline ControlSpec toggle (const juce::String& id, const juce::String& label) { return { ControlSpec::Type::Toggle, id, label, false }; }

struct SectionSpec
{
    juce::String title;
    std::vector<ControlSpec> controls;
};

struct MeterSpec
{
    enum class Kind { Level, Reduction };
    juce::String label;
    Kind kind;
    std::function<float()> valueDb; // level in dBFS, or gain reduction in dB (<= 0)
};

struct LayoutSpec
{
    std::vector<std::vector<SectionSpec>> rows;
    int displayHeight = 190;
    std::vector<MeterSpec> meters;
};

//==============================================================================
class Meter : public juce::Component
{
public:
    Meter (MeterSpec s, juce::Colour accentColour) : spec (std::move (s)), accent (accentColour)
    {
        if (spec.kind == MeterSpec::Kind::Reduction)
            shown = 0.0f;
    }

    void update()
    {
        const float v = spec.valueDb ? spec.valueDb() : -100.0f;
        if (spec.kind == MeterSpec::Kind::Level)
        {
            shown = v > shown ? v : juce::jmax (v, shown - 1.2f);
            if (v >= hold)
            {
                hold = v;
                holdFrames = 45;
            }
            else if (--holdFrames < 0)
                hold = juce::jmax (v, hold - 0.6f);
        }
        else
        {
            shown = v < shown ? v : juce::jmin (v, shown + 0.8f);
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const auto labelArea = r.removeFromBottom (16.0f);
        g.setColour (colours::dim);
        g.setFont (font (10.0f, true));
        g.drawText (spec.label, labelArea, juce::Justification::centred);

        const auto bar = r.reduced (3.0f, 2.0f);
        g.setColour (colours::background);
        g.fillRoundedRectangle (bar, 3.0f);

        if (spec.kind == MeterSpec::Kind::Level)
        {
            const float lo = -60.0f, hi = 6.0f;
            auto norm = [&] (float db) { return juce::jlimit (0.0f, 1.0f, (db - lo) / (hi - lo)); };
            const float top = bar.getBottom() - bar.getHeight() * norm (shown);
            const auto fill = bar.withTop (top);
            g.setColour (shown > 0.0f ? juce::Colour (0xffff5a5a) : accent);
            g.fillRoundedRectangle (fill, 3.0f);
            const float zeroY = bar.getBottom() - bar.getHeight() * norm (0.0f);
            g.setColour (colours::dim.withAlpha (0.6f));
            g.fillRect (bar.getX(), zeroY, bar.getWidth(), 1.0f);
            const float holdY = bar.getBottom() - bar.getHeight() * norm (hold);
            g.setColour (colours::text);
            g.fillRect (bar.getX(), holdY, bar.getWidth(), 1.5f);
        }
        else
        {
            const float range = 24.0f;
            const float amount = juce::jlimit (0.0f, 1.0f, -shown / range);
            g.setColour (juce::Colour (0xffff8a4c));
            g.fillRoundedRectangle (bar.withHeight (bar.getHeight() * amount), 3.0f);
            g.setColour (colours::dim.withAlpha (0.35f));
            for (float db : { 6.0f, 12.0f, 18.0f })
                g.fillRect (bar.getX(), bar.getY() + bar.getHeight() * db / range, bar.getWidth(), 1.0f);
        }
    }

private:
    MeterSpec spec;
    juce::Colour accent;
    float shown = -100.0f, hold = -100.0f;
    int holdFrames = 0;
};

//==============================================================================
class GittoEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    static constexpr int kMargin = 12, kHeader = 50, kGap = 8, kKnobW = 82, kChoiceW = 132, kToggleW = 82;
    static constexpr int kControlH = 98, kSectionTitleH = 22, kSectionPad = 8, kMeterW = 22;

    GittoEditor (GittoProcessor& p, LayoutSpec layout, std::unique_ptr<juce::Component> displayComponent = nullptr)
        : juce::AudioProcessorEditor (p), proc (p), lnf (p.getAccent()), display (std::move (displayComponent))
    {
        setLookAndFeel (&lnf);
        addAndMakeVisible (content);
        content.setPaintingIsUnclipped (false);
        content.onPaint = [this] (juce::Graphics& g) { paintContent (g); };

        presets = proc.getPresets();
        presetBox.setTextWhenNothingSelected ("Presets");
        for (int i = 0; i < (int) presets.size(); ++i)
            presetBox.addItem (presets[(size_t) i].name, i + 1);
        presetBox.onChange = [this]
        {
            const int index = presetBox.getSelectedId() - 1;
            if (index >= 0 && index < (int) presets.size())
                proc.applyPreset (presets[(size_t) index]);
        };
        for (int i = 0; i < (int) presets.size(); ++i)
            if (presets[(size_t) i].name == proc.currentPresetName)
                presetBox.setSelectedId (i + 1, juce::dontSendNotification);
        if (! presets.empty())
            content.addAndMakeVisible (presetBox);

        if (display != nullptr)
            content.addAndMakeVisible (*display);

        setLayout (std::move (layout));

        // Read the saved scale first: applying the resize limits triggers resized().
        const float scale = juce::jlimit (0.7f, 2.0f, proc.uiScale);
        setResizable (true, true);
        const double ratio = (double) baseWidth / (double) baseHeight;
        getConstrainer()->setFixedAspectRatio (ratio);
        setResizeLimits (juce::roundToInt (baseWidth * 0.7f), juce::roundToInt (baseHeight * 0.7f),
                         juce::roundToInt (baseWidth * 2.0f), juce::roundToInt (baseHeight * 2.0f));
        setSize (juce::roundToInt (baseWidth * scale), juce::roundToInt (baseHeight * scale));
        startTimerHz (30);
    }

    ~GittoEditor() override
    {
        stopTimer();
        setLookAndFeel (nullptr);
    }

    // Replaces the control sections. Plugins with a selectable sub-set of controls
    // (the EQ's selected band) call this again when the selection changes.
    void setLayout (LayoutSpec newLayout)
    {
        controls.clear();
        meters.clear();
        sections.clear();
        layout = std::move (newLayout);

        int widest = 0;
        for (auto& row : layout.rows)
        {
            int w = 0;
            for (auto& s : row)
                w += sectionWidth (s);
            w += kGap * juce::jmax (0, (int) row.size() - 1);
            widest = juce::jmax (widest, w);
        }
        if (baseWidth == 0)
        {
            baseWidth = juce::jmax (560, widest + 2 * kMargin);
            baseHeight = kHeader + (display != nullptr ? layout.displayHeight + kGap : 0)
                         + (int) layout.rows.size() * (rowHeight() + kGap) + kMargin - kGap + 4;
        }

        for (auto& m : layout.meters)
        {
            auto meter = std::make_unique<Meter> (m, lnf.accent);
            content.addAndMakeVisible (*meter);
            meters.push_back (std::move (meter));
        }

        for (auto& row : layout.rows)
            for (auto& s : row)
            {
                Section sec;
                sec.title = s.title;
                for (auto& c : s.controls)
                {
                    auto ctl = std::make_unique<Control> (proc, c, lnf);
                    content.addAndMakeVisible (*ctl);
                    sec.controlIndices.push_back ((int) controls.size());
                    controls.push_back (std::move (ctl));
                }
                sections.push_back (std::move (sec));
            }

        layoutContent();
        sendLookAndFeelChange(); // make sure every new control picks up the Gitto FX styling
        content.repaint();
    }

    void setControlEnabled (const juce::String& paramId, bool enabled)
    {
        for (auto& c : controls)
            if (c->spec.paramId == paramId)
                c->setControlEnabled (enabled);
    }

    std::function<void()> onTimer;
    GittoProcessor& proc;

    void resized() override
    {
        if (baseWidth <= 0)
            return;
        const float scale = (float) getWidth() / (float) baseWidth;
        content.setTransform (juce::AffineTransform::scale (scale));
        content.setBounds (0, 0, baseWidth, baseHeight);
        proc.uiScale = scale;
    }

    void paint (juce::Graphics& g) override { g.fillAll (colours::background); }

private:
    //==============================================================================
    struct Canvas : juce::Component
    {
        std::function<void (juce::Graphics&)> onPaint;
        void paint (juce::Graphics& g) override
        {
            if (onPaint)
                onPaint (g);
        }
    };

    struct Control : juce::Component
    {
        Control (GittoProcessor& p, ControlSpec s, GittoLookAndFeel& lf) : spec (std::move (s))
        {
            nameLabel.setText (spec.label, juce::dontSendNotification);
            nameLabel.setJustificationType (juce::Justification::centred);
            nameLabel.setFont (font (11.0f, true));
            nameLabel.setColour (juce::Label::textColourId, colours::dim);
            nameLabel.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (nameLabel);

            switch (spec.type)
            {
                case ControlSpec::Type::Knob:
                    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
                    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 78, 16);
                    slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
                    slider.getProperties().set ("bipolar", spec.bipolar);
                    slider.setColour (juce::Slider::textBoxTextColourId, colours::text);
                    addAndMakeVisible (slider);
                    sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (p.apvts, spec.paramId, slider);
                    if (auto* rp = p.apvts.getParameter (spec.paramId))
                        slider.setDoubleClickReturnValue (true, rp->convertFrom0to1 (rp->getDefaultValue()));
                    break;
                case ControlSpec::Type::Choice:
                    if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (p.apvts.getParameter (spec.paramId)))
                        box.addItemList (cp->choices, 1);
                    addAndMakeVisible (box);
                    boxAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.apvts, spec.paramId, box);
                    break;
                case ControlSpec::Type::Toggle:
                    button.setClickingTogglesState (true);
                    button.onStateChange = [this] { button.setButtonText (button.getToggleState() ? "ON" : "OFF"); };
                    addAndMakeVisible (button);
                    buttonAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, spec.paramId, button);
                    button.setButtonText (button.getToggleState() ? "ON" : "OFF");
                    break;
            }
            juce::ignoreUnused (lf);
        }

        void setControlEnabled (bool e)
        {
            slider.setEnabled (e);
            box.setEnabled (e);
            button.setEnabled (e);
            nameLabel.setAlpha (e ? 1.0f : 0.4f);
            slider.setAlpha (e ? 1.0f : 0.45f);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            nameLabel.setBounds (r.removeFromTop (16));
            switch (spec.type)
            {
                case ControlSpec::Type::Knob:   slider.setBounds (r.reduced (2, 0)); break;
                case ControlSpec::Type::Choice: box.setBounds (r.withSizeKeepingCentre (r.getWidth() - 10, 28)); break;
                case ControlSpec::Type::Toggle: button.setBounds (r.withSizeKeepingCentre (54, 28)); break;
            }
        }

        ControlSpec spec;
        juce::Label nameLabel;
        juce::Slider slider;
        juce::ComboBox box;
        juce::TextButton button;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> boxAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachment;
    };

    struct Section
    {
        juce::String title;
        std::vector<int> controlIndices;
        juce::Rectangle<int> bounds;
    };

    static int controlWidth (const ControlSpec& c)
    {
        return c.type == ControlSpec::Type::Knob ? kKnobW : (c.type == ControlSpec::Type::Choice ? kChoiceW : kToggleW);
    }
    static int sectionWidth (const SectionSpec& s)
    {
        int w = 2 * kSectionPad;
        for (auto& c : s.controls)
            w += controlWidth (c);
        return w;
    }
    static int rowHeight() { return kSectionTitleH + kControlH + kSectionPad; }

    void layoutContent()
    {
        auto area = juce::Rectangle<int> (0, 0, baseWidth, baseHeight);
        auto header = area.removeFromTop (kHeader);
        presetBox.setBounds (header.removeFromRight (210).reduced (kMargin, 11));
        area.reduce (kMargin, 0);

        if (display != nullptr)
        {
            auto top = area.removeFromTop (layout.displayHeight);
            for (int i = (int) meters.size() - 1; i >= 0; --i)
            {
                meters[(size_t) i]->setBounds (top.removeFromRight (kMeterW + 6));
            }
            if (! meters.empty())
                top.removeFromRight (kGap - 2);
            displayBounds = top;
            display->setBounds (top.reduced (1));
            area.removeFromTop (kGap);
        }

        size_t sectionIndex = 0;
        for (auto& row : layout.rows)
        {
            auto rowArea = area.removeFromTop (rowHeight());
            area.removeFromTop (kGap);
            int natural = 0;
            for (auto& s : row)
                natural += sectionWidth (s);
            const int available = rowArea.getWidth() - kGap * juce::jmax (0, (int) row.size() - 1);
            const float stretch = natural > 0 ? (float) available / (float) natural : 1.0f;

            int x = rowArea.getX();
            for (size_t i = 0; i < row.size(); ++i)
            {
                auto& sec = sections[sectionIndex++];
                const bool last = i + 1 == row.size();
                const int w = last ? rowArea.getRight() - x : juce::roundToInt ((float) sectionWidth (row[i]) * stretch);
                sec.bounds = { x, rowArea.getY(), w, rowArea.getHeight() };
                x += w + kGap;

                auto inner = sec.bounds.reduced (kSectionPad, 0);
                inner.removeFromTop (kSectionTitleH);
                inner.removeFromBottom (kSectionPad);
                int natInner = 0;
                for (auto& c : row[i].controls)
                    natInner += controlWidth (c);
                const float cs = natInner > 0 ? (float) inner.getWidth() / (float) natInner : 1.0f;
                float cx = (float) inner.getX();
                for (size_t k = 0; k < row[i].controls.size(); ++k)
                {
                    const float cw = (float) controlWidth (row[i].controls[k]) * cs;
                    controls[(size_t) sec.controlIndices[k]]->setBounds (juce::roundToInt (cx), inner.getY(), juce::roundToInt (cw), inner.getHeight());
                    cx += cw;
                }
            }
        }
    }

    void paintContent (juce::Graphics& g)
    {
        g.fillAll (colours::background);

        // Header: wordmark, product name, maker.
        auto header = juce::Rectangle<int> (0, 0, baseWidth, kHeader).reduced (kMargin, 0);
        g.setColour (lnf.accent);
        g.fillRoundedRectangle ((float) header.getX(), 15.0f, 4.0f, 20.0f, 2.0f);
        g.setColour (colours::text);
        g.setFont (font (19.0f, true));
        const juce::String brand ("GITTO FX");
        const int brandW = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), brand);
        g.drawText (brand, header.getX() + 12, 0, brandW + 4, kHeader, juce::Justification::centredLeft);
        g.setColour (lnf.accent);
        g.setFont (font (19.0f));
        g.drawText (proc.getProductName().toUpperCase(), header.getX() + 12 + brandW + 10, 0, 260, kHeader, juce::Justification::centredLeft);
        g.setColour (colours::dim);
        g.setFont (font (10.5f));
        g.drawText ("JG PRODUCTIONS", presetBox.getX() - 150, 0, 140, kHeader, juce::Justification::centredRight);

        if (display != nullptr)
        {
            g.setColour (colours::panel);
            g.fillRoundedRectangle (displayBounds.toFloat(), 7.0f);
            g.setColour (colours::panelEdge);
            g.drawRoundedRectangle (displayBounds.toFloat().reduced (0.5f), 7.0f, 1.0f);
        }

        for (auto& sec : sections)
        {
            g.setColour (colours::panel);
            g.fillRoundedRectangle (sec.bounds.toFloat(), 7.0f);
            g.setColour (colours::panelEdge);
            g.drawRoundedRectangle (sec.bounds.toFloat().reduced (0.5f), 7.0f, 1.0f);
            g.setColour (lnf.accent.withAlpha (0.9f));
            g.setFont (font (10.0f, true));
            g.drawText (sec.title.toUpperCase(), sec.bounds.withHeight (kSectionTitleH).reduced (kSectionPad + 2, 0), juce::Justification::centredLeft);
        }
    }

    void timerCallback() override
    {
        for (auto& m : meters)
            m->update();
        if (display != nullptr)
            display->repaint();
        if (onTimer)
            onTimer();
    }

    GittoLookAndFeel lnf;
    Canvas content;
    std::unique_ptr<juce::Component> display;
    juce::ComboBox presetBox;
    std::vector<Preset> presets;
    LayoutSpec layout;
    std::vector<std::unique_ptr<Control>> controls;
    std::vector<std::unique_ptr<Meter>> meters;
    std::vector<Section> sections;
    juce::Rectangle<int> displayBounds;
    int baseWidth = 0, baseHeight = 0;
};

//==============================================================================
// Small drawing helpers shared by the plugin displays.
inline float freqToX (float hz, float width, float lo = 20.0f, float hi = 20000.0f)
{
    return width * std::log (hz / lo) / std::log (hi / lo);
}
inline float xToFreq (float x, float width, float lo = 20.0f, float hi = 20000.0f)
{
    return lo * std::pow (hi / lo, x / width);
}

// A dB figure with one decimal that never prints "-0.0".
inline juce::String dbText (float db, bool showPlus = false)
{
    if (std::abs (db) < 0.05f)
        return "0.0 dB";
    return (showPlus && db > 0.0f ? "+" : "") + juce::String (db, 1) + " dB";
}

inline void drawFrequencyGrid (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setFont (font (9.5f));
    for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = r.getX() + freqToX (f, r.getWidth());
        g.setColour (colours::grid);
        g.fillRect (x, r.getY(), 1.0f, r.getHeight());
        g.setColour (colours::dim.withAlpha (0.7f));
        g.drawText (f >= 1000.0f ? juce::String (juce::roundToInt (f / 1000.0f)) + "k" : juce::String (juce::roundToInt (f)),
                    juce::Rectangle<float> (x + 3.0f, r.getBottom() - 13.0f, 30.0f, 12.0f), juce::Justification::centredLeft);
    }
}

//==============================================================================
// A needle meter for gain reduction, used by the single-purpose compressors.
class VuDisplay : public juce::Component
{
public:
    VuDisplay (std::function<float()> reductionDb, juce::Colour accentColour, juce::String captionText, float fullScaleDb = 20.0f)
        : getReduction (std::move (reductionDb)), accent (accentColour), caption (std::move (captionText)), range (fullScaleDb)
    {
    }

    std::function<juce::String()> infoText; // optional line under the caption

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (8.0f);
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);

        // Ballistics: quick to rise, slower to fall back, like a real meter.
        const float target = juce::jlimit (0.0f, range, -getReduction());
        shown += (target - shown) * (target > shown ? 0.55f : 0.18f);

        const float w = area.getWidth(), h = area.getHeight();
        const juce::Point<float> pivot (area.getCentreX(), area.getBottom() + h * 0.55f);
        const float radius = h * 1.32f;
        const float span = juce::jmin (0.62f, std::asin (juce::jmin (0.98f, (w * 0.5f - 24.0f) / radius)));
        auto angleFor = [&] (float db) { return -span + 2.0f * span * (db / range); };
        auto pointAt = [&] (float angle, float r) { return juce::Point<float> (pivot.x + r * std::sin (angle), pivot.y - r * std::cos (angle)); };

        juce::Path arc;
        arc.addCentredArc (pivot.x, pivot.y, radius, radius, 0.0f, -span, span, true);
        g.setColour (colours::panelEdge);
        g.strokePath (arc, juce::PathStrokeType (2.0f));
        juce::Path hot;
        hot.addCentredArc (pivot.x, pivot.y, radius, radius, 0.0f, angleFor (range * 0.5f), span, true);
        g.setColour (accent.withAlpha (0.8f));
        g.strokePath (hot, juce::PathStrokeType (2.0f));

        g.setFont (font (10.0f, true));
        const int steps = range > 12.0f ? 5 : 4;
        for (int i = 0; i <= steps * 2; ++i)
        {
            const float db = range * (float) i / (float) (steps * 2);
            const float a = angleFor (db);
            const bool major = (i % 2) == 0;
            g.setColour (major ? colours::dim : colours::panelEdge);
            g.drawLine (juce::Line<float> (pointAt (a, radius), pointAt (a, radius + (major ? 9.0f : 5.0f))), major ? 1.5f : 1.0f);
            if (major)
            {
                const auto pt = pointAt (a, radius + 20.0f);
                g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (pt.x - 14.0f, pt.y - 7.0f, 28.0f, 14.0f), juce::Justification::centred);
            }
        }

        const float needle = angleFor (shown);
        g.setColour (colours::text);
        g.drawLine (juce::Line<float> (pointAt (needle, radius * 0.62f), pointAt (needle, radius + 6.0f)), 2.0f);

        g.setColour (colours::dim);
        g.setFont (font (9.5f, true));
        g.drawText (caption.toUpperCase(), area.reduced (10.0f, 8.0f), juce::Justification::topLeft);
        if (infoText)
        {
            g.setFont (font (10.5f));
            g.drawText (infoText(), area.reduced (10.0f, 8.0f), juce::Justification::topRight);
        }
        g.setColour (colours::text);
        g.setFont (font (18.0f, true));
        g.drawText (dbText (-std::abs (shown)), area.reduced (10.0f, 6.0f), juce::Justification::bottomRight);
    }

private:
    std::function<float()> getReduction;
    juce::Colour accent;
    juce::String caption;
    float range, shown = 0.0f;
};

//==============================================================================
// Four frequency bands with draggable crossover points and a meter per band.
// Used by the multiband plugins; clicking a band selects it.
class BandSplitDisplay : public juce::Component
{
public:
    BandSplitDisplay (GittoProcessor& p, juce::StringArray crossoverParamIds)
        : proc (p), crossoverIds (std::move (crossoverParamIds))
    {
    }

    std::function<void (int)> onSelect;
    std::function<float (int)> bandValue;         // -1..1: bar drawn down or up from the centre line
    std::function<juce::String (int)> bandText;   // short readout per band
    std::function<bool (int)> bandEnabled;
    int selected = 0;

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (8.0f);
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 4.0f);
        const auto r = area.reduced (2.0f, 2.0f);
        const juce::Colour accent = proc.getAccent();

        float edges[5] = { r.getX(), 0.0f, 0.0f, 0.0f, r.getRight() };
        for (int i = 0; i < 3; ++i)
            edges[i + 1] = r.getX() + freqToX (juce::jlimit (20.0f, 20000.0f, proc.value (crossoverIds[i])), r.getWidth());

        g.setColour (colours::panelEdge);
        g.fillRect (r.getX(), r.getCentreY(), r.getWidth(), 1.0f);
        drawFrequencyGrid (g, r.withTrimmedTop (r.getHeight() - 22.0f));

        for (int b = 0; b < 4; ++b)
        {
            const auto band = juce::Rectangle<float> (edges[b], r.getY(), edges[b + 1] - edges[b], r.getHeight());
            const bool on = bandEnabled ? bandEnabled (b) : true;
            g.setColour (accent.withAlpha (b == selected ? 0.16f : (b % 2 ? 0.05f : 0.08f)));
            g.fillRect (band);
            if (b == selected)
            {
                g.setColour (accent.withAlpha (0.7f));
                g.fillRect (band.withHeight (2.0f));
            }

            const float v = bandValue ? juce::jlimit (-1.0f, 1.0f, bandValue (b)) : 0.0f;
            const float mid = r.getCentreY();
            const float barW = juce::jmin (46.0f, band.getWidth() * 0.5f);
            const float barH = std::abs (v) * (r.getHeight() * 0.5f - 22.0f);
            g.setColour ((v < 0.0f ? juce::Colour (0xffff8a4c) : accent).withAlpha (on ? 0.9f : 0.25f));
            g.fillRoundedRectangle (band.getCentreX() - barW * 0.5f, v < 0.0f ? mid : mid - barH, barW, juce::jmax (1.0f, barH), 2.0f);

            g.setColour (on ? (b == selected ? colours::text : colours::dim) : colours::panelEdge);
            g.setFont (font (11.0f, true));
            g.drawText ("BAND " + juce::String (b + 1), band.reduced (4.0f, 8.0f), juce::Justification::centredTop);
            if (bandText)
            {
                g.setFont (font (10.5f));
                g.drawText (bandText (b), band.reduced (4.0f, 28.0f), juce::Justification::centredBottom);
            }
        }

        for (int i = 1; i <= 3; ++i)
        {
            g.setColour (colours::text.withAlpha (0.75f));
            g.fillRect (edges[i] - 0.75f, r.getY(), 1.5f, r.getHeight());
            g.fillRoundedRectangle (edges[i] - 4.0f, r.getCentreY() - 9.0f, 8.0f, 18.0f, 3.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto r = getLocalBounds().toFloat().reduced (10.0f);
        dragging = -1;
        for (int i = 0; i < 3; ++i)
        {
            const float x = r.getX() + freqToX (juce::jlimit (20.0f, 20000.0f, proc.value (crossoverIds[i])), r.getWidth());
            if (std::abs (e.position.x - x) < 7.0f)
                dragging = i;
        }
        if (dragging >= 0)
        {
            if (auto* p = proc.apvts.getParameter (crossoverIds[dragging]))
                p->beginChangeGesture();
            return;
        }
        int band = 0;
        for (int i = 0; i < 3; ++i)
            if (e.position.x > r.getX() + freqToX (juce::jlimit (20.0f, 20000.0f, proc.value (crossoverIds[i])), r.getWidth()))
                band = i + 1;
        if (band != selected)
        {
            selected = band;
            if (onSelect)
                onSelect (band);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;
        const auto r = getLocalBounds().toFloat().reduced (10.0f);
        if (auto* p = proc.apvts.getParameter (crossoverIds[dragging]))
            p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (20.0f, 20000.0f, xToFreq (e.position.x - r.getX(), r.getWidth()))));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0)
            if (auto* p = proc.apvts.getParameter (crossoverIds[dragging]))
                p->endChangeGesture();
        dragging = -1;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto r = getLocalBounds().toFloat().reduced (10.0f);
        bool near = false;
        for (int i = 0; i < 3; ++i)
            near = near || std::abs (e.position.x - (r.getX() + freqToX (juce::jlimit (20.0f, 20000.0f, proc.value (crossoverIds[i])), r.getWidth()))) < 7.0f;
        setMouseCursor (near ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

private:
    GittoProcessor& proc;
    juce::StringArray crossoverIds;
    int dragging = -1;
};

//==============================================================================
// Draws an input-to-output curve in a square box (used by the saturation plugins).
inline void drawTransferCurve (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour accent,
                               const std::function<float (float)>& curve, float inputPeak)
{
    g.setColour (colours::background);
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (colours::grid);
    g.fillRect (box.getCentreX(), box.getY(), 1.0f, box.getHeight());
    g.fillRect (box.getX(), box.getCentreY(), box.getWidth(), 1.0f);
    g.setColour (colours::panelEdge);
    g.drawLine (box.getX(), box.getBottom(), box.getRight(), box.getY(), 1.0f);

    auto toX = [&] (float v) { return box.getCentreX() + v * box.getWidth() * 0.5f; };
    auto toY = [&] (float v) { return box.getCentreY() - juce::jlimit (-1.05f, 1.05f, v) * box.getHeight() * 0.48f; };
    juce::Path path;
    for (int i = 0; i <= 160; ++i)
    {
        const float x = -1.0f + 2.0f * (float) i / 160.0f;
        const float y = curve (x);
        if (i == 0) path.startNewSubPath (toX (x), toY (y));
        else path.lineTo (toX (x), toY (y));
    }
    g.setColour (accent);
    g.strokePath (path, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Where the signal is currently reaching on the curve.
    const float pk = juce::jlimit (0.0f, 1.0f, inputPeak);
    if (pk > 0.001f)
    {
        g.setColour (colours::text);
        for (float sgn : { -1.0f, 1.0f })
            g.fillEllipse (toX (sgn * pk) - 3.5f, toY (curve (sgn * pk)) - 3.5f, 7.0f, 7.0f);
    }
    g.setColour (colours::dim);
    g.setFont (font (9.5f));
    g.drawText ("IN", box.reduced (4.0f).removeFromBottom (11.0f), juce::Justification::centredRight);
    g.drawText ("OUT", box.reduced (4.0f).removeFromTop (11.0f), juce::Justification::centredLeft);
}
} // namespace gittofx
