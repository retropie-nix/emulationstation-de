//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  AudioRecovery.cpp
//
//  Decides when the audio device has to be reopened.
//

#include "AudioRecovery.h"

void AudioRecovery::deviceRemoved(const std::uint32_t now)
{
    arm(now);
}

void AudioRecovery::serverAvailability(const bool available, const std::uint32_t now)
{
    if (!mAvailabilityKnown) {
        mAvailabilityKnown = true;
        mWasAvailable = available;
        mEverAvailable = available;
        return;
    }

    // The initial connection is not a recovery: the device was opened after the server was
    // already there, or the launcher waited for it, so reopening here would only restart audio
    // on every launch.
    if (!mWasAvailable && available && mEverAvailable)
        arm(now);

    mWasAvailable = available;

    if (available)
        mEverAvailable = true;
}

void AudioRecovery::requestDefault(const std::string& outputId, const std::uint32_t now)
{
    if (outputId.empty())
        return;

    mRequestedDefault = outputId;
    mRequestedAt = now;
}

void AudioRecovery::observeDefault(const std::string& defaultId, const std::uint32_t now)
{
    if (mRequestedDefault.empty())
        return;

    if (defaultId == mRequestedDefault) {
        mRequestedDefault.clear();
        arm(now);
        return;
    }

    // The default stays on another output, so the reopen would open the wrong device.
    if (static_cast<std::uint32_t>(now - mRequestedAt) >= sDefaultTimeout)
        mRequestedDefault.clear();
}

bool AudioRecovery::shouldReopen(const std::uint32_t now, const bool gameLaunching)
{
    if (!mPending || gameLaunching)
        return false;

    // Signed arithmetic so that a wrapped tick counter still compares correctly.
    return static_cast<std::int32_t>(now - mNextAttempt) >= 0;
}

void AudioRecovery::reopenFinished(const bool success, const std::uint32_t now)
{
    if (success) {
        mPending = false;
        mAttempts = 0;
        return;
    }

    ++mAttempts;

    if (mAttempts >= sMaxAttempts) {
        mPending = false;
        mAttempts = 0;
        return;
    }

    mNextAttempt = now + retryDelay(mAttempts);
}

void AudioRecovery::arm(const std::uint32_t now)
{
    mPending = true;
    mAttempts = 0;
    mNextAttempt = now;
}

std::uint32_t AudioRecovery::retryDelay(const int failedAttempts)
{
    std::uint32_t delay {sFirstRetryDelay};

    for (int i {1}; i < failedAttempts && delay < sMaxRetryDelay; ++i)
        delay *= 2;

    return delay > sMaxRetryDelay ? sMaxRetryDelay : delay;
}
