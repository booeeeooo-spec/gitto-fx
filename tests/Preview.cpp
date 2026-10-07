// Developer tool: renders each plugin's interface to a PNG and runs audio files
// through the real plugin code, so the suite can be checked without a DAW.
//
//   GittoPreview shots <output folder> [input.wav] [bpm]
//   GittoPreview render <plugin> <preset index> <input.wav> <output.wav> [bpm=N] [id=value ...]
//   GittoPreview presets
#include "../shared/Gitto.h"

namespace gittofx
{
juce::AudioProcessor* createGittoEq();
juce::AudioProcessor* createGittoCompressor();
juce::AudioProcessor* createGittoLimiter();
juce::AudioProcessor* createGittoReverb();
juce::AudioProcessor* createGittoDelay();
juce::AudioProcessor* createGittoChorus();
juce::AudioProcessor* createGittoMultiband();
juce::AudioProcessor* createGittoSmooth();
juce::AudioProcessor* createGittoOptoComp();
juce::AudioProcessor* createGittoFetComp();
juce::AudioProcessor* createGittoSpace();
juce::AudioProcessor* createGittoBusComp();
juce::AudioProcessor* createGittoTape();
juce::AudioProcessor* createGittoPlate();
juce::AudioProcessor* createGittoTune();
juce::AudioProcessor* createGittoSaturator();
juce::AudioProcessor* createGittoDeEsser();
juce::AudioProcessor* createGittoDistortion();
juce::AudioProcessor* createGittoShaper();
juce::AudioProcessor* createGittoChannelStrip();
}

namespace
{
struct Entry { const char* name; juce::AudioProcessor* (*create)(); };
const Entry kPlugins[] = {
    { "EQ", gittofx::createGittoEq }, { "Compressor", gittofx::createGittoCompressor }, { "Limiter", gittofx::createGittoLimiter },
    { "Reverb", gittofx::createGittoReverb }, { "Delay", gittofx::createGittoDelay }, { "Chorus", gittofx::createGittoChorus },
    { "Multiband", gittofx::createGittoMultiband }, { "Smooth", gittofx::createGittoSmooth }, { "OptoComp", gittofx::createGittoOptoComp },
    { "FetComp", gittofx::createGittoFetComp }, { "Space", gittofx::createGittoSpace }, { "BusComp", gittofx::createGittoBusComp },
    { "Tape", gittofx::createGittoTape }, { "Plate", gittofx::createGittoPlate }, { "Tune", gittofx::createGittoTune },
    { "Saturator", gittofx::createGittoSaturator }, { "DeEsser", gittofx::createGittoDeEsser }, { "Distortion", gittofx::createGittoDistortion },
    { "Shaper", gittofx::createGittoShaper }, { "ChannelStrip", gittofx::createGittoChannelStrip },
};

bool loadWav (const juce::File& file, juce::AudioBuffer<float>& buffer, double& sampleRate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
        return false;
    buffer.setSize (2, (int) reader->lengthInSamples);
    reader->read (&buffer, 0, (int) reader->lengthInSamples, 0, true, true);
    if (reader->numChannels == 1)
        buffer.copyFrom (1, 0, buffer, 0, 0, buffer.getNumSamples());
    sampleRate = reader->sampleRate;
    return true;
}

// A stand-in for the host's transport, so tempo-synced plugins render in time.
struct Transport : juce::AudioPlayHead
{
    double bpm = 120.0, sampleRate = 48000.0;
    juce::int64 samplePos = 0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setTimeInSamples (samplePos);
        info.setTimeInSeconds ((double) samplePos / sampleRate);
        info.setPpqPosition ((double) samplePos / sampleRate * bpm / 60.0);
        info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
        info.setIsPlaying (true);
        return info;
    }
};
Transport transport;

void run (juce::AudioProcessor& proc, juce::AudioBuffer<float>& audio, double sampleRate, int startSample = 0, int numSamples = -1)
{
    const int block = 512;
    juce::MidiBuffer midi;
    const int end = numSamples < 0 ? audio.getNumSamples() : juce::jmin (audio.getNumSamples(), startSample + numSamples);
    transport.sampleRate = sampleRate;
    proc.setPlayHead (&transport);
    for (int pos = startSample; pos < end; pos += block)
    {
        const int n = juce::jmin (block, end - pos);
        juce::AudioBuffer<float> view (audio.getArrayOfWritePointers(), 2, pos, n);
        transport.samplePos = pos;
        proc.processBlock (view, midi);
    }
    proc.setPlayHead (nullptr);
}

std::unique_ptr<juce::AudioProcessor> make (const Entry& e, double sampleRate)
{
    std::unique_ptr<juce::AudioProcessor> proc (e.create());
    // Main stereo in and out only; any sidechain input stays off.
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (juce::AudioChannelSet::stereo());
    layout.outputBuses.add (juce::AudioChannelSet::stereo());
    for (int i = 1; i < proc->getBusCount (true); ++i)
        layout.inputBuses.add (juce::AudioChannelSet::disabled());
    proc->setBusesLayout (layout);
    proc->setRateAndBufferSizeDetails (sampleRate, 512);
    proc->prepareToPlay (sampleRate, 512);
    return proc;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::StringArray args (argv + 1, argc - 1);
    const juce::String command = args[0];

    if (command == "presets")
    {
        for (auto& e : kPlugins)
        {
            auto proc = make (e, 48000.0);
            auto* gp = dynamic_cast<gittofx::GittoProcessor*> (proc.get());
            const auto presets = gp->getPresets();
            for (int i = 0; i < (int) presets.size(); ++i)
                std::cout << e.name << "\t" << i << "\t" << presets[(size_t) i].name << "\n";
        }
        return 0;
    }

    if (command == "shots" && args.size() >= 2)
    {
        const juce::File outDir (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]));
        outDir.createDirectory();
        juce::AudioBuffer<float> source;
        double sr = 48000.0;
        if (args.size() >= 4)
            transport.bpm = args[3].getDoubleValue();
        const bool haveAudio = args.size() >= 3 && loadWav (juce::File::getCurrentWorkingDirectory().getChildFile (args[2]), source, sr);

        for (auto& e : kPlugins)
        {
            auto proc = make (e, sr);
            auto* gp = dynamic_cast<gittofx::GittoProcessor*> (proc.get());
            const auto presets = gp->getPresets();
            // Pick a preset that shows each plugin doing some work on the demo material.
            const int shotPreset = juce::String (e.name) == "Limiter" ? 2 : 1;
            if ((int) presets.size() > shotPreset)
                gp->applyPreset (presets[(size_t) shotPreset]);

            std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());
            editor->setVisible (true);

            // Feed audio in short bursts with the message loop running in between, so the
            // meters and displays are caught mid-performance.
            if (haveAudio)
            {
                juce::AudioBuffer<float> audio;
                audio.makeCopyOf (source);
                const int chunk = (int) (sr * 0.033);
                const int total = juce::jmin (audio.getNumSamples(), (int) (sr * 6.1));
                for (int pos = 0; pos + chunk < total; pos += chunk)
                {
                    run (*proc, audio, sr, pos, chunk);
                    juce::MessageManager::getInstance()->runDispatchLoopUntil (34);
                    // There is no window here, so paint a throwaway frame to advance the
                    // displays exactly as a host's 30 Hz repaint would.
                    editor->createComponentSnapshot (editor->getLocalBounds(), true, 0.1f);
                }
            }
            else
            {
                juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
            }

            const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
            const auto file = outDir.getChildFile (juce::String ("GittoFX_") + e.name + ".png");
            file.deleteFile();
            juce::FileOutputStream stream (file);
            juce::PNGImageFormat png;
            png.writeImageToStream (image, stream);
            std::cout << e.name << ": " << editor->getWidth() << " x " << editor->getHeight() << " -> " << file.getFullPathName() << "\n";
        }
        return 0;
    }

    if (command == "render" && args.size() >= 5)
    {
        const Entry* entry = nullptr;
        for (auto& e : kPlugins)
            if (args[1].equalsIgnoreCase (e.name))
                entry = &e;
        if (entry == nullptr)
        {
            std::cerr << "Unknown plugin " << args[1] << "\n";
            return 1;
        }
        juce::AudioBuffer<float> audio;
        double sr = 48000.0;
        if (! loadWav (juce::File::getCurrentWorkingDirectory().getChildFile (args[3]), audio, sr))
        {
            std::cerr << "Cannot read " << args[3] << "\n";
            return 1;
        }
        auto proc = make (*entry, sr);
        auto* gp = dynamic_cast<gittofx::GittoProcessor*> (proc.get());
        const auto presets = gp->getPresets();
        const int index = args[2].getIntValue();
        if (index >= 0 && index < (int) presets.size())
            gp->applyPreset (presets[(size_t) index]);
        // Extra parameter overrides: id=value pairs after the output file.
        for (int i = 5; i < args.size(); ++i)
        {
            const auto id = args[i].upToFirstOccurrenceOf ("=", false, false);
            if (id == "bpm")
                transport.bpm = args[i].fromFirstOccurrenceOf ("=", false, false).getDoubleValue();
            else if (auto* p = gp->apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (args[i].fromFirstOccurrenceOf ("=", false, false).getFloatValue()));
            else
            {
                std::cout << "Unknown parameter " << id << "\n";
                return 1;
            }
        }
        proc->prepareToPlay (sr, 512); // start with the chosen settings already in place

        // Leave room for tails, and line the output up by removing the plugin's latency.
        const int latency = proc->getLatencySamples();
        const int tail = (int) (sr * juce::jmin (6.0, proc->getTailLengthSeconds())) + latency;
        juce::AudioBuffer<float> padded (2, audio.getNumSamples() + tail);
        padded.clear();
        padded.copyFrom (0, 0, audio, 0, 0, audio.getNumSamples());
        padded.copyFrom (1, 0, audio, 1, 0, audio.getNumSamples());
        run (*proc, padded, sr);

        const auto outFile = juce::File::getCurrentWorkingDirectory().getChildFile (args[4]);
        outFile.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> stream (outFile.createOutputStream());
        std::unique_ptr<juce::OutputStream> out (stream.release());
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24));
        if (writer == nullptr)
            return 1;
        writer->writeFromAudioSampleBuffer (padded, latency, padded.getNumSamples() - latency);
        std::cout << entry->name << " preset " << index << " -> " << outFile.getFileName() << " (latency " << latency << " samples)\n";
        return 0;
    }

    std::cerr << "Usage: GittoPreview shots <dir> [in.wav] [bpm] | render <plugin> <preset> <in.wav> <out.wav> [bpm=N] [id=value ...] | presets\n";
    return 1;
}
