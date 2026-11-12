//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  AudioRecovery.h
//
//  Decides when the audio device has to be reopened.
//
//  ES-DE opens SDL audio once, and SDL's PipeWire backend does not survive a sound server restart,
//  so the device has to be reopened when it is lost or when the output changes underneath it. The
//  decision is kept free of SDL so it can be covered by the test executable; AudioManager performs
//  the actual deinit and init.
//

#ifndef ES_CORE_AUDIO_RECOVERY_H
#define ES_CORE_AUDIO_RECOVERY_H

#include <cstdint>
#include <string>

class AudioRecovery
{
public:
    // The output device that was in use disappeared.
    void deviceRemoved(std::uint32_t now);

    // The sound server came back after having been unreachable. The first observation only
    // records the state, so the startup connection is not treated as a recovery.
    void serverAvailability(bool available, std::uint32_t now);

    // The user selected another output. The reopen waits until the sound server reports it as the
    // default, because the selection is asynchronous.
    void requestDefault(const std::string& outputId, std::uint32_t now);
    void observeDefault(const std::string& defaultId, std::uint32_t now);

    // True when an attempt is due. A game launch suspends attempts without consuming the delay.
    bool shouldReopen(std::uint32_t now, bool gameLaunching);

    // Reports the outcome. A failure retries with backoff and gives up after a few attempts.
    void reopenFinished(bool success, std::uint32_t now);

    bool isPending() const { return mPending; }

    // The delays between failed attempts: 0.5, 1 and 2 seconds, then 4 seconds capped.
    static constexpr std::uint32_t sFirstRetryDelay {500};
    static constexpr std::uint32_t sMaxRetryDelay {4000};
    // Attempts before the recovery is abandoned; a new trigger starts it again.
    static constexpr int sMaxAttempts {6};
    // How long a selected output may take to become the default before the request is dropped.
    static constexpr std::uint32_t sDefaultTimeout {5000};

private:
    void arm(std::uint32_t now);
    static std::uint32_t retryDelay(int failedAttempts);

    bool mPending {false};
    int mAttempts {0};
    std::uint32_t mNextAttempt {0};
    bool mAvailabilityKnown {false};
    bool mWasAvailable {false};
    bool mEverAvailable {false};
    std::string mRequestedDefault;
    std::uint32_t mRequestedAt {0};
};

#endif // ES_CORE_AUDIO_RECOVERY_H
