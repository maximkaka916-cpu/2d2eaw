#include "Common/Evvscy.h"

/*  EvvscyGhost
    1) Эхо-"шиммер" (ping-pong + питч-сдвиг) слышно ТОЛЬКО в паузах между фразами.
    2) Перед каждой фразой плагин "заглядывает вперёд" (латентность 2 с), берёт её начало,
       прогоняет через ревербератор и проигрывает задом наперёд - обратный свелл-"вдох".
*/

namespace
{
using namespace evv;

//==============================================================================
struct PitchShifter   // два окна с кроссфейдом, без FFT
{
    std::vector<float> buf;
    int w = 0, mask = 0, W = 1024;
    float phase = 0.0f;

    void prepare (int windowSamples)
    {
        W = juce::jmax (64, windowSamples);
        const int size = juce::nextPowerOfTwo (W * 2 + 8);
        buf.assign ((size_t) size, 0.0f);
        mask = size - 1;
        w = 0;
        phase = 0.0f;
    }

    float read (float d) const
    {
        float pos = (float) w - d;
        if (pos < 0.0f) pos += (float) (mask + 1);
        const int i0 = (int) pos;
        const float fr = pos - (float) i0;
        const float a = buf[(size_t) (i0 & mask)];
        const float b = buf[(size_t) ((i0 + 1) & mask)];
        return a + fr * (b - a);
    }

    float process (float x, float ratio)
    {
        buf[(size_t) w] = x;

        phase += (1.0f - ratio) / (float) W;
        phase -= std::floor (phase);
        float p2 = phase + 0.5f;
        p2 -= std::floor (p2);

        const float g1 = 0.5f - 0.5f * std::cos (kTwoPi * phase);
        const float g2 = 0.5f - 0.5f * std::cos (kTwoPi * p2);
        const float y = g1 * read (phase * (float) W) + g2 * read (p2 * (float) W);

        w = (w + 1) & mask;
        return y;
    }
};

//==============================================================================
struct PhraseDetector
{
    float env = 0.0f, atk = 0.0f, rel = 0.0f, thrOn = 0.0f, thrOff = 0.0f;
    int silent = 0, hold = 0;
    bool active = false;

    void configure (float sr, float gapDb)
    {
        atk = std::exp (-1.0f / (0.003f * sr));
        rel = std::exp (-1.0f / (0.060f * sr));
        hold = (int) (0.2f * sr);
        thrOn = juce::Decibels::decibelsToGain (gapDb);
        thrOff = juce::Decibels::decibelsToGain (gapDb - 6.0f);
    }

    void reset() { env = 0.0f; silent = 0; active = false; }

    /** true в момент начала новой фразы (после паузы). */
    bool process (float x)
    {
        const float a = std::abs (x);
        const float c = a > env ? atk : rel;
        env = c * env + (1.0f - c) * a;

        bool onset = false;

        if (! active)
        {
            if (env > thrOn) { active = true; silent = 0; onset = true; }
        }
        else
        {
            if (env < thrOff) { if (++silent >= hold) active = false; }
            else silent = 0;
        }

        return onset;
    }
};

//==============================================================================
class GhostEngine
{
public:
    struct Params
    {
        float swellMs = 350.0f, swellAmt = 0.7f, tailSec = 3.0f, pitchSt = 12.0f;
        float gapDb = -40.0f, width = 1.0f, toneHz = 8000.0f, mix = 0.5f;
    };

    void prepare (double sr)
    {
        sampleRate = sr;
        latency = (int) std::lround (sr * 2.0);

        const int histLen = juce::nextPowerOfTwo (latency + (int) (sr * 0.5) + 8);
        histMask = (int64_t) histLen - 1;
        for (auto& h : hist) h.assign ((size_t) histLen, 0.0f);
        flags.assign ((size_t) histLen, 0);

        const int dlen = juce::nextPowerOfTwo ((int) (sr * 0.6) + 8);
        dMask = dlen - 1;
        for (auto& d : dl) d.assign ((size_t) dlen, 0.0f);
        d0 = (int) (0.31 * sr);
        d1 = (int) (0.47 * sr);

        shifter.prepare ((int) (0.04 * sr));

        const int maxJob = (int) (1.1 * sr) + 64;
        for (auto& b : job.buf) b.assign ((size_t) maxJob, 0.0f);

        reverb.setSampleRate (sr);
        juce::Reverb::Parameters rp;
        rp.roomSize = 0.85f; rp.damping = 0.35f; rp.wetLevel = 1.0f;
        rp.dryLevel = 0.0f; rp.width = 1.0f; rp.freezeMode = 0.0f;
        reverb.setParameters (rp);

        reset();
    }

    void reset()
    {
        now = 0; dpos = 0; lp0 = 0.0f; pauseGain = 0.0f; swellEnv = 0.0f;
        for (auto& h : hist) std::fill (h.begin(), h.end(), 0.0f);
        std::fill (flags.begin(), flags.end(), (uint8_t) 0);
        for (auto& d : dl) std::fill (d.begin(), d.end(), 0.0f);
        detector.reset();
        job.state = Job::Idle;
        reverb.reset();
    }

    void setParams (const Params& np) { p = np; }
    int getLatency() const { return latency; }

    void process (float* left, float* right, int n)
    {
        const float sr = (float) sampleRate;
        detector.configure (sr, p.gapDb);

        const int Sn = (int) (p.swellMs * 0.001f * sr);
        const int total = Sn + Sn / 2;
        const int lookahead = (int) (0.010f * sr);
        const int preroll = (int) (0.020f * sr);
        const float ratio = std::pow (2.0f, p.pitchSt / 12.0f);
        const bool shift = std::abs (p.pitchSt) > 0.01f;
        const float loopSec = (float) (d0 + d1) / sr;
        const float fb = juce::jmin (0.9f, std::pow (10.0f, -3.0f * loopSec / juce::jmax (0.3f, p.tailSec)));
        const float toneA = std::exp (-kTwoPi * p.toneHz / sr);
        const float pgAtk = std::exp (-1.0f / (0.002f * sr));
        const float pgRel = std::exp (-1.0f / (0.050f * sr));
        const float fadeLen = 0.004f * sr;

        for (int i = 0; i < n; ++i)
        {
            const float inL = left[i], inR = right[i];

            // история входа и флаг "идёт фраза"
            const size_t wi = (size_t) (now & histMask);
            hist[0][wi] = inL;
            hist[1][wi] = inR;

            const bool onset = detector.process (0.5f * (inL + inR));
            flags[wi] = detector.active ? 1 : 0;

            // планируем свелл
            if (onset && job.state == Job::Idle)
            {
                job.t0 = now - preroll;
                job.captureEnd = job.t0 + Sn;
                job.state = Job::Capturing;
            }

            if (job.state == Job::Capturing && now >= job.captureEnd)
                startRender (Sn, total);

            // задержанный сухой сигнал (то, что слышит слушатель)
            const size_t ri = (size_t) ((now - latency) & histMask);
            const float dryL = hist[0][ri], dryR = hist[1][ri];

            // пауза в "выходном" времени (с небольшим упреждением)
            const size_t fi = (size_t) ((now - latency + lookahead) & histMask);
            const float target = flags[fi] == 0 ? 1.0f : 0.0f;
            const float cg = target < pauseGain ? pgAtk : pgRel;
            pauseGain = cg * pauseGain + (1.0f - cg) * target;

            // обратный свелл
            float swL = 0.0f, swR = 0.0f;
            if (job.state == Job::Ready)
            {
                const int64_t pp = now - job.playStart;
                if (pp >= job.total)
                {
                    job.state = Job::Idle;
                }
                else if (pp >= 0)
                {
                    const int idx = job.total - 1 - (int) pp;
                    const float fade = juce::jmin (1.0f, (float) pp / fadeLen);
                    swL = job.buf[0][(size_t) idx] * job.gain * fade;
                    swR = job.buf[1][(size_t) idx] * job.gain * fade;
                }
            }
            swellEnv = juce::jmax (std::abs (swL), swellEnv * 0.9995f);

            // пинг-понг шиммер
            const float t0v = dl[0][(size_t) ((dpos - d0) & dMask)];
            const float t1v = dl[1][(size_t) ((dpos - d1) & dMask)];
            const float sh = shift ? shifter.process (t0v, ratio) : t0v;
            lp0 = (1.0f - toneA) * sh + toneA * lp0;

            dl[1][(size_t) dpos] = lp0;
            dl[0][(size_t) dpos] = 0.5f * (dryL + dryR) + fb * t1v;
            dpos = (dpos + 1) & dMask;

            float wetL = lp0 * 0.8f, wetR = t1v * 0.8f;
            const float m = 0.5f * (wetL + wetR), s = 0.5f * (wetL - wetR) * p.width;
            wetL = m + s;
            wetR = m - s;

            left[i]  = dryL + p.mix * (pauseGain * wetL + p.swellAmt * swL);
            right[i] = dryR + p.mix * (pauseGain * wetR + p.swellAmt * swR);

            ++now;
        }

        // дорендер свелла небольшими порциями
        if (job.state == Job::Rendering)
        {
            const int nn = juce::jmin (job.total - job.renderPos, 8192);
            float* l = job.buf[0].data() + job.renderPos;
            float* r = job.buf[1].data() + job.renderPos;
            reverb.processStereo (l, r, nn);

            for (int k = 0; k < nn; ++k)
                job.dstPeak = juce::jmax (job.dstPeak, std::abs (l[k]), std::abs (r[k]));

            job.renderPos += nn;
            if (job.renderPos >= job.total)
            {
                job.gain = job.dstPeak > 1.0e-6f ? juce::jmin (8.0f, job.srcPeak / job.dstPeak) : 0.0f;
                job.state = Job::Ready;
            }
        }

        uiPause.store (pauseGain);
        uiSwell.store (juce::jmin (1.0f, swellEnv * 3.0f));
        uiLevel.store (detector.env);
    }

    std::atomic<float> uiPause { 0.0f }, uiSwell { 0.0f }, uiLevel { 0.0f };

private:
    struct Job
    {
        enum State { Idle, Capturing, Rendering, Ready };
        State state = Idle;
        int64_t t0 = 0, captureEnd = 0, playStart = 0;
        int total = 0, renderPos = 0;
        float srcPeak = 0.0f, dstPeak = 0.0f, gain = 0.0f;
        std::array<std::vector<float>, 2> buf;
    };

    void startRender (int Sn, int total)
    {
        job.total = juce::jmin (total, (int) job.buf[0].size());
        job.renderPos = 0;
        job.srcPeak = 0.0f;
        job.dstPeak = 0.0f;

        for (int k = 0; k < job.total; ++k)
        {
            float l = 0.0f, r = 0.0f;
            if (k < Sn)
            {
                const size_t idx = (size_t) ((job.t0 + k) & histMask);
                l = hist[0][idx];
                r = hist[1][idx];
            }
            job.buf[0][(size_t) k] = l;
            job.buf[1][(size_t) k] = r;
            job.srcPeak = juce::jmax (job.srcPeak, std::abs (l), std::abs (r));
        }

        reverb.reset();
        job.playStart = job.t0 + latency - job.total;   // свелл заканчивается ровно в начале фразы
        job.state = Job::Rendering;
    }

    Params p;
    double sampleRate = 44100.0;
    int latency = 88200;

    std::array<std::vector<float>, 2> hist;
    std::vector<uint8_t> flags;
    int64_t histMask = 0, now = 0;

    std::array<std::vector<float>, 2> dl;
    int dMask = 0, dpos = 0, d0 = 0, d1 = 0;
    float lp0 = 0.0f, pauseGain = 0.0f, swellEnv = 0.0f;

    PitchShifter shifter;
    PhraseDetector detector;
    juce::Reverb reverb;
    Job job;
};

//==============================================================================
class GhostProcessor : public ProcessorBase
{
public:
    GhostProcessor() : ProcessorBase ("EvvscyGhost", makeLayout()) {}

    void prepareToPlay (double sr, int) override
    {
        engine.prepare (sr);
        setLatencySamples (engine.getLatency());
    }

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        if (b.getNumChannels() < 2) return;

        GhostEngine::Params p;
        p.swellMs  = get ("swell");
        p.swellAmt = get ("swellamt") / 100.0f;
        p.tailSec  = get ("tail");
        p.pitchSt  = get ("pitch");
        p.gapDb    = get ("gap");
        p.width    = get ("width") / 100.0f;
        p.toneHz   = get ("tone");
        p.mix      = get ("mix") / 100.0f;

        engine.setParams (p);
        engine.process (b.getWritePointer (0), b.getWritePointer (1), b.getNumSamples());
    }

    juce::AudioProcessorEditor* createEditor() override;

    GhostEngine engine;

private:
    static Layout makeLayout()
    {
        Layout l;
        addFloat (l, "swell",    "Swell Length (ms)", 100.0f, 700.0f, 350.0f, 1.0f);
        addFloat (l, "swellamt", "Swell Amount (%)",  0.0f, 100.0f, 70.0f, 0.1f);
        addFloat (l, "tail",     "Ghost Tail (s)",    0.5f, 10.0f, 3.0f, 0.01f);
        addFloat (l, "pitch",    "Pitch (st)",        -12.0f, 12.0f, 12.0f, 1.0f);
        addFloat (l, "gap",      "Gap Threshold (dB)", -60.0f, -20.0f, -40.0f, 0.1f);
        addFloat (l, "width",    "Width (%)",         0.0f, 200.0f, 100.0f, 0.1f);
        addFloat (l, "tone",     "Tone (Hz)",         1000.0f, 20000.0f, 8000.0f, 1.0f, 0.4f);
        addFloat (l, "mix",      "Mix (%)",           0.0f, 100.0f, 50.0f, 0.1f);
        return l;
    }
};

//==============================================================================
class GhostEditor : public EditorBase
{
public:
    explicit GhostEditor (GhostProcessor& p)
        : EditorBase (p, p.apvts, makeTheme(), "EvvscyGhost", "~ ghost echo that lives in your pauses ~",
                      { { "swell", "Swell Len", " ms" }, { "swellamt", "Swell", " %" },
                        { "tail", "Ghost Tail", " s" },  { "pitch", "Pitch", " st" },
                        { "gap", "Gap Thresh", " dB" },  { "width", "Width", " %" },
                        { "tone", "Tone", " Hz" },       { "mix", "Mix", " %" } }),
          proc (p)
    {
    }

private:
    static Theme makeTheme()
    {
        Theme t;
        t.bgTop = juce::Colour (0xff1b1840);    t.bgMid = juce::Colour (0xff3a2a6e);  t.bgBottom = juce::Colour (0xff5a3f94);
        t.accentA = juce::Colour (0xff9ad7ff);  t.accentB = juce::Colour (0xffd9a8ff);
        t.text = juce::Colour (0xffeee6ff);     t.card = juce::Colour (0x40ffffff);
        t.knobTop = juce::Colour (0xfff7f1ff);  t.knobBottom = juce::Colour (0xffcfc2ff);
        t.ink = juce::Colour (0xff3b2b66);
        t.shape = Theme::Sparkle; t.dark = true;
        return t;
    }

    void tick() override
    {
        pauseShown += (proc.engine.uiPause.load() - pauseShown) * 0.3f;
        swellShown += (proc.engine.uiSwell.load() - swellShown) * 0.3f;
        levelShown += (proc.engine.uiLevel.load() - levelShown) * 0.3f;
    }

    void drawGhost (juce::Graphics& g, float cx, float cy, float r, float alpha,
                    juce::Colour body, bool eyesOpen)
    {
        juce::Path p;
        const float x0 = cx - r, x1 = cx + r, bottom = cy + r * 1.05f;
        p.startNewSubPath (x0, cy);
        p.cubicTo (x0, cy - r * 1.333f, x1, cy - r * 1.333f, x1, cy);
        p.lineTo (x1, bottom);

        const float seg = (x1 - x0) / 4.0f;
        for (int i = 0; i < 4; ++i)
        {
            const float ex = x1 - seg * (float) (i + 1);
            const float qx = x1 - seg * ((float) i + 0.5f);
            const float amp = ((i % 2 == 0) ? 1.0f : -1.0f) * r * 0.28f + std::sin (phase * 4.0f + (float) i) * r * 0.06f;
            p.quadraticTo (qx, bottom + amp, ex, bottom);
        }
        p.closeSubPath();

        g.setColour (body.withAlpha (alpha));
        g.fillPath (p);
        g.setColour (theme.accentA.withAlpha (alpha));
        g.strokePath (p, juce::PathStrokeType (2.0f));

        const float ey = cy - r * 0.05f;
        for (float side : { -1.0f, 1.0f })
        {
            const float ex = cx + side * r * 0.36f;
            g.setColour (theme.ink.withAlpha (alpha));

            if (eyesOpen)
            {
                g.fillEllipse (ex - r * 0.1f, ey - r * 0.15f, r * 0.2f, r * 0.3f);
                g.setColour (juce::Colours::white.withAlpha (alpha));
                g.fillEllipse (ex - r * 0.04f, ey - r * 0.1f, r * 0.08f, r * 0.08f);
            }
            else
            {
                juce::Path arc;
                arc.addCentredArc (ex, ey + r * 0.03f, r * 0.1f, r * 0.1f, 0.0f, kHalfPi, 3.0f * kHalfPi, true);
                g.strokePath (arc, juce::PathStrokeType (r * 0.06f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            g.setColour (juce::Colour (0xffff8fc4).withAlpha (alpha * 0.55f));
            g.fillEllipse (cx + side * r * 0.62f - r * 0.12f, cy + r * 0.2f - r * 0.07f, r * 0.24f, r * 0.14f);
        }

        g.setColour (theme.ink.withAlpha (alpha));
        g.fillEllipse (cx - r * 0.05f, cy + r * 0.2f, r * 0.1f, r * (eyesOpen ? 0.14f : 0.07f));
    }

    void paintHero (juce::Graphics& g, juce::Rectangle<float> a) override
    {
        const auto c = a.getCentre();

        // сжимающиеся кольца = обратный свелл
        if (swellShown > 0.02f)
        {
            for (int k = 0; k < 3; ++k)
            {
                const float t = std::fmod (phase * 0.9f + (float) k / 3.0f, 1.0f);
                const float rad = (1.0f - t) * 78.0f + 8.0f;
                g.setColour (theme.accentB.withAlpha (juce::jlimit (0.0f, 1.0f, swellShown) * t * 0.9f));
                g.drawEllipse (c.x - rad, c.y - rad * 0.8f, rad * 2.0f, rad * 1.6f, 2.5f);
            }
        }

        const float bob = std::sin (phase * 2.0f) * 4.0f;
        const float scale = 0.72f + 0.28f * pauseShown;
        const float r = 36.0f * scale;
        const float cx = c.x - 36.0f;
        const float cy = c.y - 4.0f + bob;

        // эхо-шлейф в паузе
        if (pauseShown > 0.15f)
        {
            for (int k = 3; k >= 1; --k)
            {
                const float f = (float) k;
                drawGhost (g, cx + f * 38.0f, cy - f * 5.0f + std::sin (phase * 2.0f + f) * 3.0f,
                           r * (1.0f - 0.17f * f), pauseShown * (0.5f / f),
                           theme.accentA.interpolatedWith (theme.accentB, f / 3.0f), true);
            }
        }

        drawGhost (g, cx, cy, r, 0.3f + 0.7f * pauseShown, juce::Colours::white, pauseShown > 0.4f);

        // звёздочки
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.fillPath (sparklePath (a.getX() + 22.0f, a.getY() + 24.0f, 6.0f + std::sin (phase * 3.0f) * 2.0f));
        g.fillPath (sparklePath (a.getRight() - 24.0f, a.getBottom() - 22.0f, 5.0f + std::sin (phase * 2.3f) * 2.0f));
    }

    void paintInfo (juce::Graphics& g, juce::Rectangle<float> a) override
    {
        g.setFont (boldFont (13.0f));

        auto row = [&] (float y, const char* name, float v)
        {
            g.setColour (theme.text);
            g.drawText (name, (int) a.getX(), (int) (a.getY() + y), 76, 16, juce::Justification::centredLeft);
            drawBar (g, { a.getX() + 78.0f, a.getY() + y + 2.0f, a.getWidth() - 78.0f, 12.0f }, v,
                     theme.accentA, theme.accentB, true);
        };

        row (0.0f, "voice", juce::jmin (1.0f, levelShown * 5.0f));
        row (21.0f, "ghost air", pauseShown);

        g.setColour (theme.accentB);
        const char* msg = swellShown > 0.02f ? "swell is rising..."
                        : pauseShown > 0.5f  ? "ghost is floating in the pause"
                                             : "ghost hides while you sing";
        g.drawText (msg, (int) a.getX(), (int) (a.getY() + 46.0f), 336, 16, juce::Justification::centredLeft);
    }

    GhostProcessor& proc;
    float pauseShown = 0.0f, swellShown = 0.0f, levelShown = 0.0f;
};

juce::AudioProcessorEditor* GhostProcessor::createEditor() { return new GhostEditor (*this); }

} // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new GhostProcessor(); }
