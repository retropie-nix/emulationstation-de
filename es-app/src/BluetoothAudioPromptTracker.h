//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothAudioPromptTracker.h
//
//  Decides when a Bluetooth audio sink may be offered as the audio output.
//
//  A sink is not offered as soon as it is listed: the sound server announces a reconnecting sink
//  before WirePlumber has restored it as the configured default, so a device that was accepted once
//  would be offered again on every reconnect. The sink has to stay present, connected and not the
//  default for a grace period first.
//
//  The clock is a parameter of update(), so the decision can be tested without timers, a sound
//  server or Bluetooth hardware.
//

#ifndef ES_APP_BLUETOOTH_AUDIO_PROMPT_TRACKER_H
#define ES_APP_BLUETOOTH_AUDIO_PROMPT_TRACKER_H

#include "audio/AudioOutputManager.h"
#include "bluetooth/BluetoothManager.h"

#include <cstdint>
#include <map>
#include <string>

class BluetoothAudioPromptTracker
{
public:
    struct Offer {
        // Empty when there is nothing to offer.
        std::string outputId;
        std::string deviceName;
    };

    // How long a sink has to be present and not the default before it may be offered.
    static constexpr std::uint32_t sGracePeriod {3000};

    // Feeds one observation of both managers. Returns the output to offer now, if any. An output
    // that is offered is remembered, so it is not offered again while it stays connected.
    Offer update(const AudioSnapshot& audio,
                 const BluetoothSnapshot& bluetooth,
                 std::uint32_t now);

private:
    struct Sink {
        // When the grace period of this sink started.
        std::uint32_t firstSeen {0};
        // False while the grace period has not started or was reset.
        bool waiting {false};
        // True once the sink was offered for this connection.
        bool offered {false};
    };

    // Keyed by sink identifier and pruned when a sink disappears, which is what forgets both the
    // timer and the fact that the sink was offered.
    std::map<std::string, Sink> mSinks;

    static bool isSameAddress(const std::string& left, const std::string& right);
    static const BluetoothDevice* findAudioDevice(const BluetoothSnapshot& bluetooth,
                                                  const std::string& address);
};

#endif // ES_APP_BLUETOOTH_AUDIO_PROMPT_TRACKER_H
