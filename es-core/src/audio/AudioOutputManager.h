//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  AudioOutputManager.h
//
//  Audio output selection interface.
//
//  The implementation is selected at build time and hides the sound server behind an asynchronous
//  API: actions return immediately and the resulting state is published as an immutable snapshot
//  that the UI reads once per frame.
//

#ifndef ES_CORE_AUDIO_OUTPUT_MANAGER_H
#define ES_CORE_AUDIO_OUTPUT_MANAGER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AudioOutput {
    // The name of the sink in the sound server, which is the identity of the output.
    std::string id;
    // The human readable description the sound server reports for it.
    std::string description;
    bool isDefault {false};
};

struct AudioSnapshot {
    // True when the sound server could be reached. The outputs are meaningless when it is false.
    bool available {false};
    std::vector<AudioOutput> outputs;
    // The identifier of the output that is currently the default, empty when there is none.
    std::string defaultId;
    // The last failure, in a form that can be shown to the user. Empty when there is none.
    std::string lastError;
    // Increases every time a snapshot is published.
    std::uint64_t sequence {0};
};

class AudioOutputManager
{
public:
    virtual ~AudioOutputManager() = default;

    static AudioOutputManager& getInstance();

    AudioOutputManager(const AudioOutputManager&) = delete;
    AudioOutputManager& operator=(const AudioOutputManager&) = delete;

    // False in builds without a backend and, with a backend, while the sound server is not there.
    virtual bool isAvailable() const = 0;

    // The current state. The pointer stays valid when a newer snapshot is published, so the UI can
    // hold on to it while it draws a frame.
    virtual std::shared_ptr<const AudioSnapshot> getSnapshot() const = 0;

    // The sequence number of the current snapshot, a cheap way to detect that it changed.
    virtual std::uint64_t getSequence() const = 0;

    // The actions below return immediately. Their outcome shows up in a later snapshot: the
    // output list and the default output, or AudioSnapshot::lastError when it went wrong.
    virtual void setDefault(const std::string& outputId) = 0;

    // Re-reads the outputs, for the case where a change of the sound server was missed.
    virtual void refresh() = 0;

protected:
    AudioOutputManager() = default;
};

#endif // ES_CORE_AUDIO_OUTPUT_MANAGER_H
