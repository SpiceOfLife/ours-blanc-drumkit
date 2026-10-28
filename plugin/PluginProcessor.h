#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <vector>
#include "dk_engine.h"

// The C engine runs only on the audio thread. The editor talks to it through a lock-free
// command FIFO and reads back a snapshot of atomics written after every block.
class DrumKitProcessor : public juce::AudioProcessor
{
public:
    DrumKitProcessor();
    ~DrumKitProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- editor → audio
    enum class Cmd : uint8_t { Hit, TogglePlay, ToggleRecord, Clear, SetBpm, SetMetronome, SetQuantize, ToggleStep };
    void send (Cmd c, int a = 0, int b = 0);

    // ---- audio → editor
    struct Snapshot
    {
        std::array<std::atomic<uint32_t>, DK_NI> steps {}, hits {};
        std::atomic<int> playing { 0 }, recording { 0 }, bpm { 100 }, metronome { 1 }, quantize { 1 }, events { 0 }, hostSync { 0 };
        std::atomic<float> position { 0 }, level { 0 };
    };
    const Snapshot& snapshot() const { return snap; }
    bool isStandalone() const { return wrapperType == wrapperType_Standalone; }

    juce::AudioProcessorValueTreeState params;

private:
    struct Message { Cmd cmd; int a, b; };
    juce::AbstractFifo fifo { 256 };
    std::array<Message, 256> queue {};

    dk_engine* engine = nullptr;
    std::atomic<float>* volume = nullptr;
    Snapshot snap;
    std::vector<float> right;

    // state shared with the message thread (guarded; the audio thread only ever try-locks)
    juce::SpinLock stateLock;
    std::vector<uint8_t> savedState, pendingLoad;
    bool hasPendingLoad = false;
    uint32_t savedRevision = 0xffffffffu;

    void apply (const Message&);
    void syncStateFromAudio();
    void publishSnapshot();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumKitProcessor)
};
