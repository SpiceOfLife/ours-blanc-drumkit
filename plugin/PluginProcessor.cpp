#include "PluginProcessor.h"
#ifndef DK_NO_EDITOR
 #include "PluginEditor.h"
#endif

static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout l;
    l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "volume", 1 }, "Volume",
                                                        juce::NormalisableRange<float> (0.0f, 1.4f, 0.01f), 1.0f));
    return l;
}

DrumKitProcessor::DrumKitProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "DrumKit", makeLayout())
{
    volume = params.getRawParameterValue ("volume");
    savedState.reserve (DK_STATE_MAX);
    pendingLoad.reserve (DK_STATE_MAX);
}

DrumKitProcessor::~DrumKitProcessor() { dk_destroy (engine); }

bool DrumKitProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void DrumKitProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    right.assign ((size_t) juce::jmax (samplesPerBlock, 4096), 0.0f);
    if (engine != nullptr && std::abs (dk_sample_rate (engine) - sampleRate) < 0.5)
        return;

    // keep the pattern across sample-rate changes (the drums are re-synthesized at the new rate)
    std::vector<uint8_t> keep;
    {
        const juce::SpinLock::ScopedLockType lock (stateLock);
        if (hasPendingLoad) { keep = pendingLoad; hasPendingLoad = false; }
        else if (engine != nullptr) { keep.resize (DK_STATE_MAX); keep.resize (dk_save (engine, keep.data(), keep.size())); }
        else keep = savedState;
    }
    dk_destroy (engine);
    engine = dk_create (sampleRate);
    if (engine == nullptr) return;
    if (! keep.empty()) dk_load (engine, keep.data(), keep.size());
    else if (! isStandalone()) dk_set_metronome (engine, 0);   // in a DAW the host already has a click
    savedRevision = 0xffffffffu;
    syncStateFromAudio();
    publishSnapshot();
}

void DrumKitProcessor::send (Cmd c, int a, int b)
{
    const auto scope = fifo.write (1);
    if (scope.blockSize1 > 0) queue[(size_t) scope.startIndex1] = { c, a, b };
    else if (scope.blockSize2 > 0) queue[(size_t) scope.startIndex2] = { c, a, b };
}

void DrumKitProcessor::apply (const Message& m)
{
    switch (m.cmd)
    {
        case Cmd::Hit:          dk_hit (engine, m.a, m.b); break;
        case Cmd::TogglePlay:   dk_toggle_play (engine); break;
        case Cmd::ToggleRecord: dk_toggle_record (engine); break;
        case Cmd::Clear:        dk_clear (engine); break;
        case Cmd::SetBpm:       dk_set_bpm (engine, m.a); break;
        case Cmd::SetMetronome: dk_set_metronome (engine, m.a); break;
        case Cmd::SetQuantize:  dk_set_quantize (engine, m.a); break;
        case Cmd::ToggleStep:   dk_toggle_step (engine, m.a, m.b); break;
    }
}

void DrumKitProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (engine == nullptr) { buffer.clear(); return; }

    {   // a pattern restored by the host (setStateInformation) while we were running
        const juce::SpinLock::ScopedTryLockType lock (stateLock);
        if (lock.isLocked() && hasPendingLoad)
        {
            dk_load (engine, pendingLoad.data(), pendingLoad.size());
            hasPendingLoad = false;
        }
    }

    {   // commands from the editor
        const auto scope = fifo.read (fifo.getNumReady());
        for (int i = 0; i < scope.blockSize1; ++i) apply (queue[(size_t) (scope.startIndex1 + i)]);
        for (int i = 0; i < scope.blockSize2; ++i) apply (queue[(size_t) (scope.startIndex2 + i)]);
    }

    // follow the DAW's tempo and position; when it stops, the internal transport takes over
    bool hostPlaying = false;
    double ppq = 0.0, bpm = 0.0;
    if (! isStandalone())
        if (auto* head = getPlayHead())
            if (const auto pos = head->getPosition())
            {
                bpm = pos->getBpm().orFallback (0.0);
                if (const auto p = pos->getPpqPosition()) { ppq = *p; hostPlaying = pos->getIsPlaying(); }
            }
    dk_host_sync (engine, hostPlaying ? 1 : 0, ppq, bpm);
    dk_set_volume (engine, volume->load());

    // render, splitting the block at each MIDI note so hits are sample-accurate
    float* l = buffer.getWritePointer (0);
    int done = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (done, n, meta.samplePosition);
        if (at > done) { dk_render (engine, l + done, nullptr, at - done); done = at; }
        const auto msg = meta.getMessage();
        if (msg.isNoteOn())
        {
            const int inst = dk_gm_note_to_inst (msg.getNoteNumber());
            if (inst >= 0) dk_hit (engine, inst, msg.getVelocity());
        }
    }
    if (n > done) dk_render (engine, l + done, nullptr, n - done);
    for (int ch = 1; ch < buffer.getNumChannels(); ++ch) buffer.copyFrom (ch, 0, l, n);
    midi.clear();

    syncStateFromAudio();
    publishSnapshot();
    snap.hostSync.store (hostPlaying || (bpm > 0.0) ? 1 : 0);
}

void DrumKitProcessor::syncStateFromAudio()
{
    if (dk_revision (engine) == savedRevision) return;
    const juce::SpinLock::ScopedTryLockType lock (stateLock);
    if (! lock.isLocked()) return;   // try again next block
    savedState.resize (DK_STATE_MAX);  // within the reserved capacity: no allocation
    savedState.resize (dk_save (engine, savedState.data(), savedState.size()));
    savedRevision = dk_revision (engine);
}

void DrumKitProcessor::publishSnapshot()
{
    for (int i = 0; i < DK_NI; ++i)
    {
        snap.steps[(size_t) i].store (dk_step_mask (engine, i));
        snap.hits[(size_t) i].store (dk_hit_count (engine, i));
    }
    snap.playing.store (dk_playing (engine));
    snap.recording.store (dk_recording (engine));
    snap.bpm.store (dk_bpm (engine));
    snap.metronome.store (dk_metronome (engine));
    snap.quantize.store (dk_quantize (engine));
    snap.events.store (dk_event_count (engine));
    snap.position.store ((float) dk_position (engine));
    snap.level.store (dk_level (engine));
}

// ---------------------------------------------------------------- state: parameters + pattern
void DrumKitProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = params.copyState();
    {
        const juce::SpinLock::ScopedLockType lock (stateLock);
        const auto& bytes = hasPendingLoad ? pendingLoad : savedState;
        state.setProperty ("pattern", juce::Base64::toBase64 (bytes.data(), bytes.size()), nullptr);
    }
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void DrumKitProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (params.state.getType())) return;
    auto tree = juce::ValueTree::fromXml (*xml);
    juce::MemoryOutputStream decoded;
    const bool ok = juce::Base64::convertFromBase64 (decoded, tree.getProperty ("pattern").toString());
    tree.removeProperty ("pattern", nullptr);
    params.replaceState (tree);
    if (! ok || decoded.getDataSize() == 0) return;

    const auto* p = static_cast<const uint8_t*> (decoded.getData());
    const juce::SpinLock::ScopedLockType lock (stateLock);
    pendingLoad.assign (p, p + juce::jmin ((size_t) DK_STATE_MAX, decoded.getDataSize()));
    savedState = pendingLoad;
    hasPendingLoad = true;
}

juce::AudioProcessorEditor* DrumKitProcessor::createEditor()
{
#ifdef DK_NO_EDITOR
    return nullptr;
#else
    return new DrumKitEditor (*this);
#endif
}

#ifndef DK_NO_EDITOR
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new DrumKitProcessor(); }
#endif
