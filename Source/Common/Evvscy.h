#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

/**  Общий код для всех плагинов evvscy: тема, ручки-мордочки, анимированный фон,
     база редактора (шапка + сетка ручек), база процессора и темповые часы. */
namespace evv
{
constexpr float kPi     = juce::MathConstants<float>::pi;
constexpr float kTwoPi  = juce::MathConstants<float>::twoPi;
constexpr float kHalfPi = juce::MathConstants<float>::halfPi;

//==============================================================================
struct Theme
{
    enum Shape { Sparkle, Heart, Petal, Star };

    juce::Colour bgTop, bgMid, bgBottom, accentA, accentB, text, card, knobTop, knobBottom, ink;
    Shape shape = Sparkle;
    bool dark = false;
};

inline juce::Font boldFont (float h)  { return juce::Font (juce::FontOptions (h, juce::Font::bold)); }
inline juce::Font plainFont (float h) { return juce::Font (juce::FontOptions (h)); }

inline juce::Path heartPath (float cx, float cy, float size)
{
    juce::Path p;
    p.startNewSubPath (0.0f, 0.9f);
    p.cubicTo (-1.3f, 0.0f, -1.0f, -1.0f, 0.0f, -0.35f);
    p.cubicTo (1.0f, -1.0f, 1.3f, 0.0f, 0.0f, 0.9f);
    p.closeSubPath();
    p.applyTransform (juce::AffineTransform::scale (size).translated (cx, cy));
    return p;
}

inline juce::Path sparklePath (float cx, float cy, float size)
{
    juce::Path p;
    p.addStar ({ cx, cy }, 4, size * 0.22f, size, 0.0f);
    return p;
}

inline void drawBar (juce::Graphics& g, juce::Rectangle<float> r, float v,
                     juce::Colour a, juce::Colour b, bool dark)
{
    g.setColour (dark ? juce::Colours::white.withAlpha (0.12f) : juce::Colours::white.withAlpha (0.85f));
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);

    v = juce::jlimit (0.0f, 1.0f, v);
    if (v > 0.01f)
    {
        auto f = r.withWidth (juce::jmax (r.getHeight(), r.getWidth() * v));
        g.setGradientFill (juce::ColourGradient (a, f.getX(), 0.0f, b, f.getRight(), 0.0f, false));
        g.fillRoundedRectangle (f, r.getHeight() * 0.5f);
    }
}

//==============================================================================
//  Параметры
using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

inline void addFloat (Layout& l, const char* id, const char* name,
                      float lo, float hi, float def, float step = 0.01f, float skew = 1.0f)
{
    l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name,
                                                        juce::NormalisableRange<float> (lo, hi, step, skew), def));
}

inline void addChoice (Layout& l, const char* id, const char* name, const juce::StringArray& items, int def)
{
    l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, items, def));
}

inline void addBool (Layout& l, const char* id, const char* name, bool def)
{
    l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def));
}

//==============================================================================
/** Фаза LFO: либо свободная (Гц), либо привязанная к темпу и позиции хоста. */
struct LfoClock
{
    double cycles = 0.0, inc = 0.0;

    void beginBlock (juce::AudioPlayHead* ph, bool sync, double divBeats, double freeHz, double sr)
    {
        double bpm = 120.0, ppq = 0.0;
        bool locked = false;

        if (ph != nullptr)
        {
            if (auto pos = ph->getPosition())
            {
                auto b = pos->getBpm();
                if (b.hasValue()) bpm = *b;

                auto q = pos->getPpqPosition();
                if (pos->getIsPlaying() && q.hasValue()) { locked = true; ppq = *q; }
            }
        }

        if (sync)
        {
            inc = bpm / 60.0 / sr / divBeats;
            if (locked) cycles = ppq / divBeats;
        }
        else
        {
            inc = freeHz / sr;
        }
    }

    float phaseFor (double mult, double offset) const
    {
        const double v = cycles * mult + offset;
        return (float) (v - std::floor (v));
    }

    void advance() { cycles += inc; }
};

//==============================================================================
class KawaiiLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit KawaiiLookAndFeel (const Theme& t) : theme (t)
    {
        setColour (juce::Slider::textBoxTextColourId, theme.text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Label::textColourId, theme.text);
        setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                           float pos, float startA, float endA, juce::Slider&) override
    {
        auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
        const float radius = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
        const auto c = b.getCentre();
        const float angle = startA + pos * (endA - startA);
        const float arcR = radius - 3.0f;

        juce::Path track;
        track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, startA, endA, true);
        g.setColour (theme.dark ? juce::Colours::white.withAlpha (0.15f) : juce::Colours::white.withAlpha (0.85f));
        g.strokePath (track, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path val;
        val.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, startA, angle, true);
        g.setGradientFill (juce::ColourGradient (theme.accentA, c.x - arcR, c.y, theme.accentB, c.x + arcR, c.y, false));
        g.strokePath (val, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const float bodyR = radius - 11.0f;
        g.setColour (juce::Colours::black.withAlpha (theme.dark ? 0.3f : 0.08f));
        g.fillEllipse (c.x - bodyR, c.y - bodyR + 3.0f, bodyR * 2.0f, bodyR * 2.0f);
        g.setGradientFill (juce::ColourGradient (theme.knobTop, c.x, c.y - bodyR, theme.knobBottom, c.x, c.y + bodyR, false));
        g.fillEllipse (c.x - bodyR, c.y - bodyR, bodyR * 2.0f, bodyR * 2.0f);
        g.setColour (theme.accentA.withAlpha (0.6f));
        g.drawEllipse (c.x - bodyR, c.y - bodyR, bodyR * 2.0f, bodyR * 2.0f, 2.0f);

        // мордочка
        for (float side : { -1.0f, 1.0f })
        {
            const float ex = c.x + side * bodyR * 0.42f;
            const float ey = c.y - bodyR * 0.1f;

            g.setColour (theme.ink);
            g.fillEllipse (ex - bodyR * 0.09f, ey - bodyR * 0.12f, bodyR * 0.18f, bodyR * 0.24f);
            g.setColour (juce::Colours::white);
            g.fillEllipse (ex - bodyR * 0.04f, ey - bodyR * 0.09f, bodyR * 0.07f, bodyR * 0.07f);

            g.setColour (theme.accentA.withAlpha (0.4f));
            g.fillEllipse (c.x + side * bodyR * 0.62f - bodyR * 0.15f, c.y + bodyR * 0.22f - bodyR * 0.085f,
                           bodyR * 0.3f, bodyR * 0.17f);
        }

        juce::Path mouth;
        mouth.addCentredArc (c.x, c.y + bodyR * 0.18f, bodyR * 0.1f, bodyR * 0.1f, 0.0f, kHalfPi, 3.0f * kHalfPi, true);
        g.setColour (theme.ink);
        g.strokePath (mouth, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // указатель-сердечко
        const float px = c.x + arcR * std::sin (angle);
        const float py = c.y - arcR * std::cos (angle);
        g.setColour (juce::Colours::white);
        g.fillEllipse (px - 6.0f, py - 6.0f, 12.0f, 12.0f);
        g.setColour (theme.accentA);
        g.fillPath (heartPath (px, py, 4.2f));
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool) override
    {
        auto r = button.getLocalBounds().toFloat().reduced (2.0f);
        const bool on = button.getToggleState();
        const float corner = r.getHeight() * 0.5f;

        g.setColour (on ? theme.accentA : (theme.dark ? juce::Colours::white.withAlpha (0.12f)
                                                      : juce::Colours::white.withAlpha (0.85f)));
        g.fillRoundedRectangle (r, corner);
        g.setColour (theme.accentA.withAlpha (highlighted ? 1.0f : 0.8f));
        g.drawRoundedRectangle (r, corner, 2.0f);

        g.setColour (on ? juce::Colours::white : theme.accentA);
        g.fillPath (heartPath (r.getX() + corner, r.getCentreY(), 5.0f));

        g.setColour (on ? (theme.dark ? theme.bgTop : juce::Colours::white) : theme.text);
        g.setFont (boldFont (14.0f));
        g.drawText (button.getButtonText() + (on ? ": ON" : ": off"),
                    r.withTrimmedLeft (corner + 8.0f), juce::Justification::centred);
    }

private:
    Theme theme;
};

//==============================================================================
struct ParamSpec
{
    const char* id;
    const char* name;
    const char* suffix;
    bool toggle = false;
};

/** Шапка с анимированной героиней + сетка ручек. Наследник рисует героиню и инфо-панель. */
class EditorBase : public juce::AudioProcessorEditor,
                   protected juce::Timer
{
public:
    EditorBase (juce::AudioProcessor& p, juce::AudioProcessorValueTreeState& apvts, const Theme& t,
                const juce::String& titleText, const juce::String& subtitleText,
                const std::vector<ParamSpec>& specs, int columns = 4)
        : juce::AudioProcessorEditor (p), theme (t), laf (t),
          title (titleText), subtitle (subtitleText), cols (columns)
    {
        setLookAndFeel (&laf);

        for (const auto& s : specs)
        {
            auto cell = std::make_unique<Cell>();

            if (s.toggle)
            {
                cell->button = std::make_unique<juce::ToggleButton> (s.name);
                addAndMakeVisible (*cell->button);
                cell->buttonAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                    apvts, s.id, *cell->button);
            }
            else
            {
                cell->slider = std::make_unique<juce::Slider>();
                cell->slider->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
                cell->slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 84, 18);
                cell->slider->setTextValueSuffix (s.suffix);
                cell->slider->setRotaryParameters (kPi * 1.2f, kPi * 2.8f, true);
                addAndMakeVisible (*cell->slider);

                cell->label = std::make_unique<juce::Label>();
                cell->label->setText (s.name, juce::dontSendNotification);
                cell->label->setJustificationType (juce::Justification::centred);
                cell->label->setFont (boldFont (14.0f));
                addAndMakeVisible (*cell->label);

                cell->sliderAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                    apvts, s.id, *cell->slider);
            }

            cells.push_back (std::move (cell));
        }

        rows = ((int) cells.size() + cols - 1) / cols;
        totalH = headerH + rows * cellH + footerH;

        // частицы на фоне
        uint32_t seed = 987654u;
        auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (float) ((seed >> 8) & 0xFFFF) / 65535.0f; };
        for (auto& sp : sparks)
        {
            sp.x = rnd() * (float) winW;
            sp.y = rnd() * (float) totalH;
            sp.size = 4.0f + rnd() * 7.0f;
            sp.speed = 0.15f + rnd() * 0.5f;
            sp.seed = rnd();
        }

        setSize (winW, totalH);
        startTimerHz (30);
    }

    ~EditorBase() override
    {
        stopTimer();
        setLookAndFeel (nullptr);
    }

    void resized() override
    {
        const int cellW = (winW - 40) / cols;

        for (size_t i = 0; i < cells.size(); ++i)
        {
            juce::Rectangle<int> cell (20 + (int) (i % (size_t) cols) * cellW,
                                       headerH + (int) (i / (size_t) cols) * cellH, cellW, cellH);
            auto& c = *cells[i];

            if (c.button != nullptr)
            {
                c.button->setBounds (cell.withSizeKeepingCentre (130, 38));
            }
            else
            {
                c.label->setBounds (cell.removeFromTop (22));
                c.slider->setBounds (cell);
            }
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();

        // фон
        juce::ColourGradient bg (theme.bgTop, 0.0f, 0.0f, theme.bgBottom, 0.0f, b.getBottom(), false);
        bg.addColour (0.5, theme.bgMid);
        g.setGradientFill (bg);
        g.fillAll();

        g.setGradientFill (juce::ColourGradient (theme.accentB.withAlpha (0.28f), 150.0f, 90.0f,
                                                 theme.accentB.withAlpha (0.0f), 150.0f, 340.0f, true));
        g.fillAll();

        for (const auto& sp : sparks)
            drawSpark (g, sp);

        // карточка шапки
        const auto card = juce::Rectangle<float> (12.0f, 12.0f, 636.0f, 154.0f);
        g.setColour (theme.card);
        g.fillRoundedRectangle (card, 28.0f);
        g.setColour (theme.accentA.withAlpha (0.7f));
        g.drawRoundedRectangle (card, 28.0f, 3.0f);

        // героиня
        const auto hero = juce::Rectangle<float> (24.0f, 20.0f, 262.0f, 138.0f);
        g.saveState();
        g.reduceClipRegion (hero.toNearestInt());
        paintHero (g, hero);
        g.restoreState();

        // название
        g.setFont (boldFont (32.0f));
        g.setColour (theme.dark ? juce::Colours::black.withAlpha (0.4f) : juce::Colours::white);
        g.drawText (title, 302, 26, 230, 40, juce::Justification::centredLeft);
        g.setColour (theme.accentA);
        g.drawText (title, 300, 24, 230, 40, juce::Justification::centredLeft);

        g.setFont (plainFont (14.0f));
        g.setColour (theme.accentB);
        g.drawText (subtitle, 300, 64, 336, 20, juce::Justification::centredLeft);

        // инфо-панель
        paintInfo (g, juce::Rectangle<float> (300.0f, 90.0f, 336.0f, 66.0f));

        // бейдж evvscy
        const juce::Rectangle<float> badge (538.0f, 24.0f, 98.0f, 28.0f);
        g.setGradientFill (juce::ColourGradient (theme.accentA, badge.getX(), 0.0f, theme.accentB, badge.getRight(), 0.0f, false));
        g.fillRoundedRectangle (badge, 14.0f);
        g.setColour (theme.dark ? theme.bgTop : juce::Colours::white);
        g.fillPath (sparklePath (badge.getX() + 16.0f, badge.getCentreY(), 7.0f));
        g.setFont (boldFont (16.0f));
        g.drawText ("evvscy", badge.withTrimmedLeft (22.0f), juce::Justification::centred);

        // подвал
        g.setFont (boldFont (13.0f));
        g.setColour (theme.text.withAlpha (0.85f));
        g.drawText ("made with love by evvscy", 0, totalH - footerH + 8, winW, 22, juce::Justification::centred);
        g.setColour (theme.accentA);
        g.fillPath (heartPath ((float) winW * 0.5f - 108.0f, (float) (totalH - footerH + 19), 5.0f));
        g.fillPath (heartPath ((float) winW * 0.5f + 108.0f, (float) (totalH - footerH + 19), 5.0f));
    }

protected:
    virtual void tick() {}
    virtual void paintHero (juce::Graphics&, juce::Rectangle<float>) {}
    virtual void paintInfo (juce::Graphics&, juce::Rectangle<float>) {}

    Theme theme;
    float phase = 0.0f;
    int frame = 0;

private:
    struct Cell
    {
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::ToggleButton> button;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAtt;
    };

    struct Spark { float x = 0, y = 0, size = 5, speed = 0.3f, seed = 0; };

    void timerCallback() override
    {
        phase += 0.033f;
        ++frame;

        for (auto& sp : sparks)
        {
            sp.y -= sp.speed;
            sp.x += std::sin (phase * 0.7f + sp.seed * 6.0f) * 0.25f;
            if (sp.y < -12.0f) { sp.y = (float) totalH + 12.0f; }
        }

        tick();
        repaint();
    }

    void drawSpark (juce::Graphics& g, const Spark& sp) const
    {
        const float tw = 0.5f + 0.5f * std::sin (phase * 2.0f + sp.seed * 10.0f);
        g.setColour (theme.accentA.interpolatedWith (juce::Colours::white, 0.5f).withAlpha (0.2f + 0.5f * tw));

        switch (theme.shape)
        {
            case Theme::Sparkle:
                g.fillPath (sparklePath (sp.x, sp.y, sp.size * (0.8f + 0.3f * tw)));
                break;
            case Theme::Heart:
                g.fillPath (heartPath (sp.x, sp.y, sp.size * 0.7f));
                break;
            case Theme::Petal:
            {
                juce::Path pt;
                pt.addEllipse (-sp.size * 0.7f, -sp.size * 0.35f, sp.size * 1.4f, sp.size * 0.7f);
                pt.applyTransform (juce::AffineTransform::rotation (phase * 0.8f + sp.seed * 6.0f).translated (sp.x, sp.y));
                g.fillPath (pt);
                break;
            }
            case Theme::Star:
            {
                juce::Path st;
                st.addStar ({ sp.x, sp.y }, 5, sp.size * 0.4f, sp.size * 0.9f, phase * 0.5f + sp.seed * 6.0f);
                g.fillPath (st);
                break;
            }
        }
    }

    static constexpr int winW = 660, headerH = 176, cellH = 142, footerH = 44;

    KawaiiLookAndFeel laf;
    juce::String title, subtitle;
    int cols = 4, rows = 1, totalH = 400;
    std::vector<std::unique_ptr<Cell>> cells;
    std::array<Spark, 26> sparks;
};

//==============================================================================
/** Общая часть процессора: стерео-шины, состояние, чтение параметров. */
class ProcessorBase : public juce::AudioProcessor
{
public:
    ProcessorBase (const juce::String& n, Layout layout)
        : juce::AudioProcessor (BusesProperties()
                                    .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                    .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "PARAMS", std::move (layout)),
          pluginName (n)
    {
    }

    const juce::String getName() const override { return pluginName; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool hasEditor() const override { return true; }
    void releaseResources() override {}

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
               && l.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
    }

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        if (auto xml = apvts.copyState().createXml())
            copyXmlToBinary (*xml, dest);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
            if (xml->hasTagName (apvts.state.getType()))
                apvts.replaceState (juce::ValueTree::fromXml (*xml));
    }

    float get (const char* id) const { return apvts.getRawParameterValue (id)->load(); }

    juce::AudioProcessorValueTreeState apvts;

private:
    juce::String pluginName;
};

} // namespace evv
