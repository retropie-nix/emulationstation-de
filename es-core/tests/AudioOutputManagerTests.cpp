//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  AudioOutputManagerTests.cpp
//
//  Test cases for the AudioOutputManager singleton. They also keep the selected backend in the
//  link: until the UI uses the manager nothing else references it, so without these the libpulse
//  implementation would be compiled but never linked.
//

#include "TestHarness.h"

#include "audio/AudioOutputManager.h"

TEST_CASE(audioOutputManagerPublishesASnapshot)
{
    AudioOutputManager& manager {AudioOutputManager::getInstance()};

    // The snapshot is never null, so that the UI can build the output list before the worker has
    // reached the sound server.
    const std::shared_ptr<const AudioSnapshot> snapshot {manager.getSnapshot()};
    EXPECT(snapshot != nullptr);

    // The sequence number is a cheap way to detect changes and never goes backwards.
    EXPECT(manager.getSequence() >= snapshot->sequence);
}
