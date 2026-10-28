// Headless test of the plug-in processor: MIDI timing, host sync, editor commands, state save/restore.
#include "../plugin/PluginProcessor.h"
#include <cstdio>

static int fails = 0;
static void check (bool ok, const juce::String& what)
{
    std::printf ("%s %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++fails;
}

struct FakeHost : juce::AudioPlayHead
{
    bool playing = false; double ppq = 0, bpm = 120;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p; p.setIsPlaying (playing); p.setPpqPosition (ppq); p.setBpm (bpm); return p;
    }
};

static int firstNonZero (const juce::AudioBuffer<float>& b)
{
    for (int i = 0; i < b.getNumSamples(); ++i) if (std::abs (b.getSample (0, i)) > 1e-4f) return i;
    return -1;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const double sr = 48000; const int bs = 512;

    DrumKitProcessor p;
    FakeHost host; p.setPlayHead (&host);
    p.prepareToPlay (sr, bs);
    juce::AudioBuffer<float> buf (2, bs);
    juce::MidiBuffer midi;
    auto run = [&] { buf.clear(); midi.clear(); p.processBlock (buf, midi); };

    // MIDI note 36 at sample 300 → kick starts exactly there, both channels
    buf.clear(); midi.clear();
    midi.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) 120), 300);
    p.processBlock (buf, midi);
    const int at = firstNonZero (buf);
    check (at >= 300 && at < 304, "MIDI kick is sample-accurate (starts at " + juce::String (at) + ", expect 300)");
    check (std::abs (buf.getSample (1, 310) - buf.getSample (0, 310)) < 1e-6f, "stereo output");
    check (p.snapshot().hits[DK_KICK].load() == 1, "kick counted once");
    check (p.snapshot().metronome.load() == 0, "metronome off by default inside a DAW");
    for (int i = 0; i < 400; ++i) run();

    // editor commands: quarter-note snares, then follow the host from bar 1
    for (int s = 0; s < DK_STEPS; s += 4) p.send (DrumKitProcessor::Cmd::ToggleStep, DK_SNARE, s);
    run();
    check (p.snapshot().steps[DK_SNARE].load() == 0x11111111u, "steps from the editor reach the engine");
    host.playing = true; host.ppq = 0; host.bpm = 150;
    const auto before = p.snapshot().hits[DK_SNARE].load();
    const int blocks = (int) (sr * 60.0 / 150.0 * 8 / bs);   // 8 beats
    for (int i = 0; i < blocks; ++i) { run(); host.ppq += bs / sr * host.bpm / 60.0; }
    const auto played = p.snapshot().hits[DK_SNARE].load() - before;
    check (played == 8, "8 snares in 8 host beats: " + juce::String ((int) played));
    check (p.snapshot().bpm.load() == 150, "tempo follows the host: " + juce::String (p.snapshot().bpm.load()));
    host.playing = false; run();
    check (p.snapshot().playing.load() == 0, "stops with the host");

    // state round trip into a fresh instance (different sample rate)
    p.send (DrumKitProcessor::Cmd::ToggleStep, DK_RIDE, 3); run();
    if (auto* v = p.params.getParameter ("volume")) v->setValueNotifyingHost (v->convertTo0to1 (0.5f));
    juce::MemoryBlock state; p.getStateInformation (state);
    DrumKitProcessor q;
    q.setStateInformation (state.getData(), (int) state.getSize());
    q.prepareToPlay (44100, 256);
    juce::AudioBuffer<float> b2 (2, 256); juce::MidiBuffer m2; q.processBlock (b2, m2);
    check (q.snapshot().steps[DK_SNARE].load() == 0x11111111u && q.snapshot().steps[DK_RIDE].load() == (1u << 3), "pattern restored from host state");
    check (std::abs (q.params.getRawParameterValue ("volume")->load() - 0.5f) < 1e-3f, "volume restored");

    // restore while running
    DrumKitProcessor r; r.prepareToPlay (sr, bs);
    juce::AudioBuffer<float> b3 (2, bs); juce::MidiBuffer m3; r.processBlock (b3, m3);
    r.setStateInformation (state.getData(), (int) state.getSize()); r.processBlock (b3, m3);
    check (r.snapshot().steps[DK_RIDE].load() == (1u << 3), "state applied on the next audio block");

    std::printf (fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
