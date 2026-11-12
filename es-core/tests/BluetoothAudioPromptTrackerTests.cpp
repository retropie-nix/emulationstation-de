//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothAudioPromptTrackerTests.cpp
//
//  Test cases for the decision when a Bluetooth audio sink may be offered. The clock is passed in,
//  so the grace period is checked without timers, a sound server or Bluetooth hardware.
//
//  The tracker belongs to es-app, but it only depends on the two manager interfaces, so it is
//  compiled into this executable through the target below.
//

#include "TestHarness.h"

#include "BluetoothAudioPromptTracker.h"

namespace
{
    constexpr const char* headsetAddress {"AA:BB:CC:DD:EE:FF"};
    constexpr const char* headsetSink {"bluez_output.AA_BB_CC_DD_EE_FF.1"};
    constexpr const char* hdmiSink {"alsa_output.pci-0000_01_00.0.hdmi-stereo"};
    constexpr const char* headsetName {"WH-1000XM4"};

    constexpr BluetoothAudioMode sAsk {BluetoothAudioMode::ask};
    constexpr BluetoothAudioMode sAuto {BluetoothAudioMode::autoSwitch};
    constexpr BluetoothAudioMode sOff {BluetoothAudioMode::off};

    AudioOutput makeOutput(const std::string& id,
                           const std::string& description = "",
                           const std::string& bluetoothAddress = "")
    {
        AudioOutput output;
        output.id = id;
        output.description = description.empty() ? id : description;
        output.bluetoothAddress = bluetoothAddress;

        return output;
    }

    AudioSnapshot makeAudio(const bool headsetPresent, const bool headsetIsDefault)
    {
        AudioSnapshot audio;
        audio.available = true;
        audio.outputs.push_back(makeOutput(hdmiSink, "HDMI"));

        if (headsetPresent)
            audio.outputs.push_back(makeOutput(headsetSink, "Headset", headsetAddress));

        audio.defaultId = headsetIsDefault ? headsetSink : hdmiSink;

        return audio;
    }

    BluetoothSnapshot makeBluetooth(const bool connected,
                                    const BluetoothDeviceKind kind = BluetoothDeviceKind::headset)
    {
        BluetoothSnapshot bluetooth;
        bluetooth.available = true;

        BluetoothDevice device;
        device.path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF";
        device.address = headsetAddress;
        device.name = headsetName;
        device.kind = kind;
        device.connected = connected;
        bluetooth.devices.push_back(device);

        return bluetooth;
    }

} // namespace

TEST_CASE(bluetoothSinkIsOfferedAfterTheGracePeriod)
{
    BluetoothAudioPromptTracker tracker;
    const AudioSnapshot audio {makeAudio(true, false)};
    const BluetoothSnapshot bluetooth {makeBluetooth(true)};

    // The sink is announced before the sound server has restored it as the default, so nothing is
    // offered while the grace period runs.
    EXPECT(tracker.update(audio, bluetooth, sAsk, 0).action == BluetoothAudioOffer::none);
    EXPECT(tracker.update(audio, bluetooth, sAsk,
                          BluetoothAudioPromptTracker::sGracePeriod - 1)
               .action == BluetoothAudioOffer::none);

    const BluetoothAudioPromptTracker::Offer offer {
        tracker.update(audio, bluetooth, sAsk, BluetoothAudioPromptTracker::sGracePeriod)};
    EXPECT(offer.action == BluetoothAudioOffer::prompt);
    EXPECT(offer.outputId == headsetSink);
    EXPECT(offer.deviceName == headsetName);

    // It is not offered again for the same connection.
    EXPECT(tracker.update(audio, bluetooth, sAsk,
                          BluetoothAudioPromptTracker::sGracePeriod + 1)
               .action == BluetoothAudioOffer::none);
    EXPECT(tracker.update(audio, bluetooth, sAsk, 60000).action == BluetoothAudioOffer::none);
}

TEST_CASE(autoModeSwitchesWithoutAsking)
{
    BluetoothAudioPromptTracker tracker;
    const AudioSnapshot audio {makeAudio(true, false)};
    const BluetoothSnapshot bluetooth {makeBluetooth(true)};

    EXPECT(tracker.update(audio, bluetooth, sAuto, 0).action == BluetoothAudioOffer::none);

    const BluetoothAudioPromptTracker::Offer offer {
        tracker.update(audio, bluetooth, sAuto, BluetoothAudioPromptTracker::sGracePeriod)};
    EXPECT(offer.action == BluetoothAudioOffer::switchOutput);
    EXPECT(offer.outputId == headsetSink);

    // The automatic switch is remembered like a prompt, so it happens once per connection.
    EXPECT(tracker.update(audio, bluetooth, sAuto,
                          BluetoothAudioPromptTracker::sGracePeriod + 1)
               .action == BluetoothAudioOffer::none);
    EXPECT(tracker.update(audio, bluetooth, sAsk, 60000).action == BluetoothAudioOffer::none);
}

TEST_CASE(offModeNeverOffersAnything)
{
    BluetoothAudioPromptTracker tracker;
    const AudioSnapshot audio {makeAudio(true, false)};
    const BluetoothSnapshot bluetooth {makeBluetooth(true)};

    EXPECT(tracker.update(audio, bluetooth, sOff, 0).action == BluetoothAudioOffer::none);
    EXPECT(tracker.update(audio, bluetooth, sOff, 10000).action == BluetoothAudioOffer::none);
    EXPECT(tracker.update(audio, bluetooth, sOff, 60000).action == BluetoothAudioOffer::none);

    // Nothing was remembered, so turning asking back on offers the still-connected device once
    // its (long elapsed) grace period is checked again.
    const BluetoothAudioPromptTracker::Offer offer {
        tracker.update(audio, bluetooth, sAsk, 60001)};
    EXPECT(offer.action == BluetoothAudioOffer::prompt);
    EXPECT(offer.outputId == headsetSink);
}

TEST_CASE(settingStringsMapToTheThreeModes)
{
    EXPECT(BluetoothAudioPromptTracker::modeFromString("ask") == BluetoothAudioMode::ask);
    EXPECT(BluetoothAudioPromptTracker::modeFromString("auto") ==
           BluetoothAudioMode::autoSwitch);
    EXPECT(BluetoothAudioPromptTracker::modeFromString("off") == BluetoothAudioMode::off);

    // An empty or manually edited value falls back to asking.
    EXPECT(BluetoothAudioPromptTracker::modeFromString("") == BluetoothAudioMode::ask);
    EXPECT(BluetoothAudioPromptTracker::modeFromString("something") == BluetoothAudioMode::ask);
}

TEST_CASE(defaultSinkIsNotOffered)
{
    BluetoothAudioPromptTracker tracker;

    // The device is already the default, which is what a reconnecting accepted device looks like.
    EXPECT(tracker.update(makeAudio(true, true), makeBluetooth(true), sAsk, 0).action ==
           BluetoothAudioOffer::none);
    EXPECT(tracker.update(makeAudio(true, true), makeBluetooth(true), sAsk, 10000).action ==
           BluetoothAudioOffer::none);

    // When it stops being the default later, the grace period starts over.
    EXPECT(tracker.update(makeAudio(true, false), makeBluetooth(true), sAsk, 20000).action ==
           BluetoothAudioOffer::none);
    EXPECT(tracker.update(makeAudio(true, false), makeBluetooth(true), sAsk, 22000).action ==
           BluetoothAudioOffer::none);
    EXPECT(tracker.update(makeAudio(true, false), makeBluetooth(true), sAsk, 23000).outputId ==
           headsetSink);
}

TEST_CASE(disconnectedDeviceIsNotOffered)
{
    BluetoothAudioPromptTracker tracker;

    EXPECT(tracker.update(makeAudio(true, false), makeBluetooth(false), sAsk, 0).action ==
           BluetoothAudioOffer::none);
    EXPECT(tracker.update(makeAudio(true, false), makeBluetooth(false), sAsk, 10000).action ==
           BluetoothAudioOffer::none);

    // Once the device reports connected the grace period starts.
    EXPECT(tracker.update(makeAudio(true, false), makeBluetooth(true), sAsk, 20000).action ==
           BluetoothAudioOffer::none);
    EXPECT(tracker.update(makeAudio(true, false), makeBluetooth(true), sAsk,
                          20000 + BluetoothAudioPromptTracker::sGracePeriod)
               .outputId == headsetSink);
}

TEST_CASE(disappearingSinkForgetsTheOffer)
{
    BluetoothAudioPromptTracker tracker;

    EXPECT(tracker.update(makeAudio(true, false), makeBluetooth(true), sAsk, 0).action ==
           BluetoothAudioOffer::none);
    EXPECT(tracker.update(makeAudio(true, false), makeBluetooth(true), sAsk,
                          BluetoothAudioPromptTracker::sGracePeriod)
               .outputId == headsetSink);

    // The sink is gone, so nothing is offered and the offer is forgotten.
    EXPECT(tracker.update(makeAudio(false, false), makeBluetooth(false), sAsk, 5000).action ==
           BluetoothAudioOffer::none);

    // The reconnection starts a new grace period instead of offering it right away.
    EXPECT(tracker.update(makeAudio(true, false), makeBluetooth(true), sAsk, 10000).action ==
           BluetoothAudioOffer::none);
    EXPECT(tracker.update(makeAudio(true, false), makeBluetooth(true), sAsk, 12000).action ==
           BluetoothAudioOffer::none);
    EXPECT(tracker.update(makeAudio(true, false), makeBluetooth(true), sAsk,
                          10000 + BluetoothAudioPromptTracker::sGracePeriod)
               .outputId == headsetSink);
}

TEST_CASE(otherSinksAreNotOffered)
{
    BluetoothAudioPromptTracker tracker;

    // A sink that does not belong to a Bluetooth device is never offered.
    EXPECT(tracker.update(makeAudio(false, false), makeBluetooth(false), sAsk, 0).action ==
           BluetoothAudioOffer::none);

    // Neither is the sink of a device that is not an audio device.
    EXPECT(tracker.update(makeAudio(true, false),
                          makeBluetooth(true, BluetoothDeviceKind::keyboard), sAsk, 0)
               .action == BluetoothAudioOffer::none);
    EXPECT(tracker.update(makeAudio(true, false),
                          makeBluetooth(true, BluetoothDeviceKind::keyboard), sAsk, 20000)
               .action == BluetoothAudioOffer::none);
}

TEST_CASE(twoBluetoothSinksAreOfferedOneAtATime)
{
    BluetoothAudioPromptTracker tracker;

    AudioSnapshot audio {makeAudio(true, false)};
    audio.outputs.push_back(
        makeOutput("bluez_output.11_22_33_44_55_66.1", "Speaker", "11:22:33:44:55:66"));

    BluetoothSnapshot bluetooth {makeBluetooth(true)};
    BluetoothDevice speaker;
    speaker.path = "/org/bluez/hci0/dev_11_22_33_44_55_66";
    speaker.address = "11:22:33:44:55:66";
    speaker.name = "Speaker";
    speaker.kind = BluetoothDeviceKind::speaker;
    speaker.connected = true;
    bluetooth.devices.push_back(speaker);

    // Only one is offered per observation, and the other follows on the next one even though its
    // grace period also elapsed.
    EXPECT(tracker.update(audio, bluetooth, sAsk, 0).action == BluetoothAudioOffer::none);

    const BluetoothAudioPromptTracker::Offer first {
        tracker.update(audio, bluetooth, sAsk, BluetoothAudioPromptTracker::sGracePeriod)};
    EXPECT(first.outputId == headsetSink);

    const BluetoothAudioPromptTracker::Offer second {
        tracker.update(audio, bluetooth, sAsk, BluetoothAudioPromptTracker::sGracePeriod + 1)};
    EXPECT(second.outputId == "bluez_output.11_22_33_44_55_66.1");
    EXPECT(second.deviceName == "Speaker");
}
