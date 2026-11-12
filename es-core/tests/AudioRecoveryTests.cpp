//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  AudioRecoveryTests.cpp
//
//  Test cases for the audio device recovery decision. The clock is a parameter, so the triggers,
//  the retry delays and the point where the recovery gives up are covered without SDL or a sound
//  server.
//

#include "TestHarness.h"

#include "AudioRecovery.h"

namespace
{
    // Marks the state where no game is being launched.
    constexpr bool sNotLaunching {false};
    constexpr bool sLaunching {true};

} // namespace

TEST_CASE(removedDeviceIsReopenedImmediately)
{
    AudioRecovery recovery;

    EXPECT(!recovery.shouldReopen(1000, sNotLaunching));

    recovery.deviceRemoved(1000);

    EXPECT(recovery.isPending());
    EXPECT(recovery.shouldReopen(1000, sNotLaunching));
}

TEST_CASE(failedReopensBackOffAndGiveUp)
{
    AudioRecovery recovery;
    recovery.deviceRemoved(0);

    // The first attempt is due at once and fails.
    EXPECT(recovery.shouldReopen(0, sNotLaunching));
    recovery.reopenFinished(false, 0);

    // 0.5 s after the first failure, 1 s after the second, 2 s after the third, then the cap.
    EXPECT(!recovery.shouldReopen(499, sNotLaunching));
    EXPECT(recovery.shouldReopen(500, sNotLaunching));
    recovery.reopenFinished(false, 500);

    EXPECT(!recovery.shouldReopen(1499, sNotLaunching));
    EXPECT(recovery.shouldReopen(1500, sNotLaunching));
    recovery.reopenFinished(false, 1500);

    EXPECT(!recovery.shouldReopen(3499, sNotLaunching));
    EXPECT(recovery.shouldReopen(3500, sNotLaunching));
    recovery.reopenFinished(false, 3500);

    EXPECT(!recovery.shouldReopen(7499, sNotLaunching));
    EXPECT(recovery.shouldReopen(7500, sNotLaunching));
    recovery.reopenFinished(false, 7500);

    // The fifth failure still schedules the capped delay.
    EXPECT(recovery.shouldReopen(11500, sNotLaunching));
    recovery.reopenFinished(false, 11500);

    // The sixth failure abandons the recovery.
    EXPECT(!recovery.isPending());
    EXPECT(!recovery.shouldReopen(20000, sNotLaunching));
}

TEST_CASE(successStopsTheRecovery)
{
    AudioRecovery recovery;
    recovery.deviceRemoved(0);

    EXPECT(recovery.shouldReopen(0, sNotLaunching));
    recovery.reopenFinished(true, 0);

    EXPECT(!recovery.isPending());
    EXPECT(!recovery.shouldReopen(10000, sNotLaunching));
}

TEST_CASE(gameLaunchSuspendsButDoesNotDelay)
{
    AudioRecovery recovery;
    recovery.deviceRemoved(0);

    // The attempt becomes due during a launch, and is made as soon as the launch ends.
    EXPECT(!recovery.shouldReopen(5000, sLaunching));
    EXPECT(recovery.shouldReopen(5000, sNotLaunching));
}

TEST_CASE(serverComingBackTriggersRecovery)
{
    AudioRecovery recovery;

    // The startup connection is not a recovery, it only tells the tracker that the server has
    // been available once.
    recovery.serverAvailability(false, 0);
    EXPECT(!recovery.isPending());

    recovery.serverAvailability(true, 1000);
    EXPECT(!recovery.isPending());

    // Another observation while it stays available does not queue a reopen either.
    recovery.serverAvailability(true, 1500);
    EXPECT(!recovery.isPending());

    // Going away and coming back does.
    recovery.serverAvailability(false, 3000);
    recovery.serverAvailability(true, 4000);
    EXPECT(recovery.isPending());
    EXPECT(recovery.shouldReopen(4000, sNotLaunching));
}

TEST_CASE(availableAtStartupIsNotARecovery)
{
    AudioRecovery recovery;

    recovery.serverAvailability(true, 0);
    EXPECT(!recovery.isPending());
    EXPECT(!recovery.shouldReopen(1000, sNotLaunching));

    // The first loss and return after that is a recovery.
    recovery.serverAvailability(false, 2000);
    recovery.serverAvailability(true, 3000);
    EXPECT(recovery.isPending());
    EXPECT(recovery.shouldReopen(3000, sNotLaunching));
}

TEST_CASE(selectedOutputIsReopenedOnlyWhenConfirmed)
{
    AudioRecovery recovery;

    recovery.requestDefault("bluez_output.headset", 0);

    // Another output is still the default, so the stream would be reopened on the wrong device.
    recovery.observeDefault("alsa_output.hdmi", 100);
    EXPECT(!recovery.isPending());

    recovery.observeDefault("bluez_output.headset", 200);
    EXPECT(recovery.isPending());
    EXPECT(recovery.shouldReopen(200, sNotLaunching));
}

TEST_CASE(unconfirmedSelectionIsDropped)
{
    AudioRecovery recovery;

    recovery.requestDefault("bluez_output.headset", 0);
    recovery.observeDefault("alsa_output.hdmi", AudioRecovery::sDefaultTimeout - 1);
    EXPECT(!recovery.isPending());

    // The request expires at the timeout, so a default that changes on its own later must not
    // reopen the device.
    recovery.observeDefault("alsa_output.hdmi", AudioRecovery::sDefaultTimeout);
    recovery.observeDefault("bluez_output.headset", AudioRecovery::sDefaultTimeout + 100);
    EXPECT(!recovery.isPending());
}

TEST_CASE(newTriggerRestartsTheBackoff)
{
    AudioRecovery recovery;
    recovery.deviceRemoved(0);

    EXPECT(recovery.shouldReopen(0, sNotLaunching));
    recovery.reopenFinished(false, 0);
    EXPECT(!recovery.shouldReopen(0, sNotLaunching));

    // A second, independent trigger does not have to wait for the pending delay.
    recovery.deviceRemoved(10);
    EXPECT(recovery.shouldReopen(10, sNotLaunching));
}
