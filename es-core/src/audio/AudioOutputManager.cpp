//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  AudioOutputManager.cpp
//
//  Selects the audio output manager implementation and provides the null implementation that is
//  used on every platform without a backend.
//

#include "audio/AudioOutputManager.h"

#if defined(AUDIO_OUTPUT_PULSE)
// Defined by AudioOutputManagerPulse.cpp.
std::unique_ptr<AudioOutputManager> createPulseAudioOutputManager();
#endif

namespace
{
    // Reports the sound server as unavailable and ignores all actions. This is what every platform
    // without a backend, and every build without AUDIO_OUTPUT_MANAGER, gets.
    class NullAudioOutputManager : public AudioOutputManager
    {
    public:
        NullAudioOutputManager()
            : mSnapshot {std::make_shared<AudioSnapshot>()}
        {
        }

        bool isAvailable() const override { return false; }
        std::shared_ptr<const AudioSnapshot> getSnapshot() const override { return mSnapshot; }
        std::uint64_t getSequence() const override { return mSnapshot->sequence; }

        void setDefault(const std::string&) override { }
        void refresh() override { }

    private:
        std::shared_ptr<const AudioSnapshot> mSnapshot;
    };

    // The backend is chosen here, at build time.
    std::unique_ptr<AudioOutputManager> createAudioOutputManager()
    {
#if defined(AUDIO_OUTPUT_PULSE)
        return createPulseAudioOutputManager();
#else
        return std::make_unique<NullAudioOutputManager>();
#endif
    }

} // namespace

AudioOutputManager& AudioOutputManager::getInstance()
{
    static const std::unique_ptr<AudioOutputManager> instance {createAudioOutputManager()};
    return *instance;
}
