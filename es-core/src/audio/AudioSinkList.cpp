//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  AudioSinkList.cpp
//
//  Accumulates the replies of a sink list request.
//

#include "audio/AudioSinkList.h"

#include <utility>

std::uint64_t AudioSinkList::start()
{
    ++mGeneration;
    mActive = true;
    mOutputs.clear();

    return mGeneration;
}

void AudioSinkList::collect(const std::uint64_t generation, const AudioOutput& output)
{
    if (!mActive || generation != mGeneration)
        return;

    mOutputs.push_back(output);
}

bool AudioSinkList::take(const std::uint64_t generation, std::vector<AudioOutput>& outputs)
{
    if (!mActive || generation != mGeneration)
        return false;

    mActive = false;
    outputs = std::move(mOutputs);
    mOutputs.clear();

    return true;
}

void AudioSinkList::discard(const std::uint64_t generation)
{
    if (generation != mGeneration)
        return;

    mActive = false;
    mOutputs.clear();
}
