//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  AudioSinkList.h
//
//  Accumulates the replies of a sink list request.
//
//  The sound server answers a sink list with one callback per sink and an end marker, and several
//  such requests can be in flight at the same time: a Bluetooth sink appears and then changes, so a
//  second list starts before the first one has finished. Every request therefore carries a
//  generation, and a reply of a superseded request is discarded instead of being mixed into the
//  list of the current one.
//
//  This is deliberately independent of the sound server library so that it can be tested without a
//  sound server.
//

#ifndef ES_CORE_AUDIO_SINK_LIST_H
#define ES_CORE_AUDIO_SINK_LIST_H

#include "audio/AudioOutputManager.h"

#include <cstdint>
#include <vector>

class AudioSinkList
{
public:
    // Starts a new request, discarding whatever was collected for the previous one. The returned
    // generation has to be passed to every callback of this reply.
    std::uint64_t start();

    // Adds one sink of the reply with the given generation.
    void collect(std::uint64_t generation, const AudioOutput& output);

    // Ends the reply. Returns false, and does not touch `outputs`, when a newer request has
    // superseded this one or when it was already ended.
    bool take(std::uint64_t generation, std::vector<AudioOutput>& outputs);

    // Ends the reply without handing anything over, for a request that failed.
    void discard(std::uint64_t generation);

private:
    std::uint64_t mGeneration {0};
    bool mActive {false};
    std::vector<AudioOutput> mOutputs;
};

#endif // ES_CORE_AUDIO_SINK_LIST_H
