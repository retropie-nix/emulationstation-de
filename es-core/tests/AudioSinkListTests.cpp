//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  AudioSinkListTests.cpp
//
//  Test cases for the accumulation of sink list replies. A sink list that is superseded by a newer
//  request must not mix its entries into the newer one, which is what happens when a Bluetooth sink
//  is added and then changed while the first reply is still in flight.
//

#include "TestHarness.h"

#include "audio/AudioSinkList.h"

namespace
{
    AudioOutput makeOutput(const std::string& id, const std::string& description = "")
    {
        AudioOutput output;
        output.id = id;
        output.description = description.empty() ? id : description;

        return output;
    }

} // namespace

TEST_CASE(interleavedSinkListsDoNotMix)
{
    AudioSinkList list;

    // A first request is in flight and already reported one sink.
    const std::uint64_t first {list.start()};
    list.collect(first, makeOutput("bluez_output.headset"));

    // A second request starts before the first one finishes, as happens when a sink is added and
    // then changed.
    const std::uint64_t second {list.start()};
    list.collect(second, makeOutput("alsa_output.hdmi"));

    // A late entry of the superseded request must not reach the current list.
    list.collect(first, makeOutput("bluez_output.headset.2"));

    std::vector<AudioOutput> outputs;
    EXPECT(!list.take(first, outputs));
    EXPECT(outputs.empty());

    list.collect(second, makeOutput("alsa_output.spdif"));

    EXPECT(list.take(second, outputs));
    EXPECT(outputs.size() == 2);
    if (outputs.size() == 2) {
        EXPECT(outputs.at(0).id == "alsa_output.hdmi");
        EXPECT(outputs.at(1).id == "alsa_output.spdif");
    }
}

TEST_CASE(sinkListEntriesDoNotRepeat)
{
    AudioSinkList list;

    // The same sequence as a connected headset produces it: the list is requested for the new sink
    // and requested again for the change that follows it.
    const std::uint64_t first {list.start()};
    list.collect(first, makeOutput("alsa_output.hdmi"));
    list.collect(first, makeOutput("bluez_output.headset"));

    const std::uint64_t second {list.start()};
    list.collect(second, makeOutput("alsa_output.hdmi"));
    list.collect(second, makeOutput("bluez_output.headset"));

    std::vector<AudioOutput> outputs;
    EXPECT(list.take(second, outputs));
    EXPECT(outputs.size() == 2);
}

TEST_CASE(finishedSinkListsAreHandedOverOnce)
{
    AudioSinkList list;

    const std::uint64_t generation {list.start()};
    list.collect(generation, makeOutput("alsa_output.hdmi"));

    std::vector<AudioOutput> outputs;
    EXPECT(list.take(generation, outputs));
    EXPECT(outputs.size() == 1);

    // A second end marker of the same reply must not hand the list over again.
    outputs.clear();
    EXPECT(!list.take(generation, outputs));
    EXPECT(outputs.empty());
}

TEST_CASE(discardedSinkListsAreForgotten)
{
    AudioSinkList list;

    const std::uint64_t generation {list.start()};
    list.collect(generation, makeOutput("alsa_output.hdmi"));

    // The request could not be issued, so nothing may be published from it.
    list.discard(generation);

    std::vector<AudioOutput> outputs;
    EXPECT(!list.take(generation, outputs));
    EXPECT(outputs.empty());

    // The next request starts from an empty list again.
    const std::uint64_t next {list.start()};
    list.collect(next, makeOutput("alsa_output.spdif"));
    EXPECT(list.take(next, outputs));
    EXPECT(outputs.size() == 1);
}
