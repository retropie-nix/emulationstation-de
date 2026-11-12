//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothMessageParserTests.cpp
//
//  Test cases for the parsing of the BlueZ messages. They build, seal and rewind a message instead
//  of talking to a bus, so the parsing can be checked without Bluetooth hardware.
//

#include "TestHarness.h"

// The parser only exists in builds with the BlueZ backend.
#if defined(BLUETOOTH_MANAGER_BLUEZ)

#include "bluetooth/BluetoothMessageParser.h"

#include <systemd/sd-bus.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
    constexpr const char* adapterPath {"/org/bluez/hci0"};
    constexpr const char* devicePath {"/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"};
    constexpr const char* adapterAddress {"10:20:30:40:50:60"};

    // The helpers below append one a{sv} entry each. Every value is wrapped in a variant, which is
    // what the parser has to unwrap.
    void appendStringProperty(sd_bus_message* message, const char* name, const char* value)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "s");
        sd_bus_message_append(message, "s", value);
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
    }

    void appendBooleanProperty(sd_bus_message* message, const char* name, const bool value)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "b");
        sd_bus_message_append(message, "b", value ? 1 : 0);
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
    }

    void appendStringListProperty(sd_bus_message* message,
                                  const char* name,
                                  const std::vector<std::string>& values)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "as");
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "s");
        for (const std::string& value : values)
            sd_bus_message_append(message, "s", value.c_str());
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
    }

    void appendInt16Property(sd_bus_message* message, const char* name, const int16_t value)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "n");
        sd_bus_message_append(message, "n", value);
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
    }

    void appendByteProperty(sd_bus_message* message, const char* name, const uint8_t value)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "y");
        sd_bus_message_append(message, "y", value);
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
    }

    // A value type that the parser does not reduce, to check that it is skipped without disturbing
    // the rest of the dictionary.
    void appendDoubleProperty(sd_bus_message* message, const char* name, const double value)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "d");
        sd_bus_message_append(message, "d", value);
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
    }

    void openObject(sd_bus_message* message, const char* path)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "oa{sa{sv}}");
        sd_bus_message_append(message, "o", path);
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sa{sv}}");
    }

    void closeObject(sd_bus_message* message)
    {
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
    }

    void openInterface(sd_bus_message* message, const char* name)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
    }

    void closeInterface(sd_bus_message* message)
    {
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
    }

    // Builds, seals and rewinds the reply of GetManagedObjects for one adapter with one device.
    std::shared_ptr<sd_bus_message> buildManagedObjectsReply(sd_bus* bus)
    {
        sd_bus_message* message {nullptr};
        if (sd_bus_message_new_method_call(bus,
                                          &message,
                                          "org.bluez",
                                          "/",
                                          "org.freedesktop.DBus.ObjectManager",
                                          "GetManagedObjects") < 0)
            return nullptr;

        std::shared_ptr<sd_bus_message> holder {message, sd_bus_message_unref};

        if (sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{oa{sa{sv}}}") < 0)
            return nullptr;

        openObject(message, adapterPath);
        openInterface(message, "org.bluez.Adapter1");
        appendBooleanProperty(message, "Powered", true);
        appendStringProperty(message, "Address", adapterAddress);
        appendBooleanProperty(message, "Discovering", false);
        appendDoubleProperty(message, "Unsupported", 1.5);
        closeInterface(message);
        closeObject(message);

        openObject(message, devicePath);
        openInterface(message, "org.bluez.Device1");
        appendStringProperty(message, "Alias", "Pad");
        appendInt16Property(message, "RSSI", -42);
        appendStringListProperty(message,
                                 "UUIDs",
                                 {"00001124-0000-1000-8000-00805f9b34fb",
                                  "00001812-0000-1000-8000-00805f9b34fb"});
        closeInterface(message);

        openInterface(message, "org.bluez.Battery1");
        appendByteProperty(message, "Percentage", 65);
        closeInterface(message);
        closeObject(message);

        if (sd_bus_message_close_container(message) < 0)
            return nullptr;

        if (sd_bus_message_seal(message, 1, 0) < 0)
            return nullptr;

        if (sd_bus_message_rewind(message, true) < 0)
            return nullptr;

        return holder;
    }

} // namespace

TEST_CASE(managedObjectsAreParsed)
{
    sd_bus* bus {nullptr};
    EXPECT(sd_bus_open_user(&bus) >= 0);
    if (bus == nullptr)
        return;

    const std::shared_ptr<sd_bus_message> message {buildManagedObjectsReply(bus)};
    sd_bus_unref(bus);

    EXPECT(message != nullptr);
    if (message == nullptr)
        return;

    BluetoothObjectMap objects;
    EXPECT(BluetoothMessageParser::readManagedObjects(message.get(), objects));
    EXPECT(objects.size() == 2);

    // The adapter properties have to arrive, including the boolean, the string and the values of
    // the narrower integer types.
    const auto adapter {objects.find(adapterPath)};
    EXPECT(adapter != objects.end());
    if (adapter != objects.end()) {
        const auto interface {adapter->second.find("org.bluez.Adapter1")};
        EXPECT(interface != adapter->second.end());
        if (interface != adapter->second.end()) {
            const BluetoothProperties& properties {interface->second};

            const auto powered {properties.find("Powered")};
            EXPECT(powered != properties.end());
            if (powered != properties.end()) {
                const auto value {std::get_if<bool>(&powered->second)};
                EXPECT(value != nullptr && *value);
            }

            const auto address {properties.find("Address")};
            EXPECT(address != properties.end());
            if (address != properties.end()) {
                const auto value {std::get_if<std::string>(&address->second)};
                EXPECT(value != nullptr && *value == adapterAddress);
            }

            const auto discovering {properties.find("Discovering")};
            EXPECT(discovering != properties.end());
            if (discovering != properties.end()) {
                const auto value {std::get_if<bool>(&discovering->second)};
                EXPECT(value != nullptr && !*value);
            }

            // The value type that the parser does not reduce is dropped, and dropping it does not
            // stop the rest of the dictionary from being read.
            EXPECT(properties.find("Unsupported") == properties.end());
        }
    }

    const auto device {objects.find(devicePath)};
    EXPECT(device != objects.end());
    if (device != objects.end()) {
        const auto interface {device->second.find("org.bluez.Device1")};
        EXPECT(interface != device->second.end());
        if (interface != device->second.end()) {
            const BluetoothProperties& properties {interface->second};

            const auto alias {properties.find("Alias")};
            EXPECT(alias != properties.end());
            if (alias != properties.end()) {
                const auto value {std::get_if<std::string>(&alias->second)};
                EXPECT(value != nullptr && *value == "Pad");
            }

            const auto rssi {properties.find("RSSI")};
            EXPECT(rssi != properties.end());
            if (rssi != properties.end()) {
                const auto value {std::get_if<std::int64_t>(&rssi->second)};
                EXPECT(value != nullptr && *value == -42);
            }

            const auto uuids {properties.find("UUIDs")};
            EXPECT(uuids != properties.end());
            if (uuids != properties.end()) {
                const auto value {std::get_if<std::vector<std::string>>(&uuids->second)};
                EXPECT(value != nullptr && value->size() == 2);
                if (value != nullptr && value->size() == 2) {
                    EXPECT(value->at(0) == "00001124-0000-1000-8000-00805f9b34fb");
                    EXPECT(value->at(1) == "00001812-0000-1000-8000-00805f9b34fb");
                }
            }
        }

        const auto battery {device->second.find("org.bluez.Battery1")};
        EXPECT(battery != device->second.end());
        if (battery != device->second.end()) {
            const auto percentage {battery->second.find("Percentage")};
            EXPECT(percentage != battery->second.end());
            if (percentage != battery->second.end()) {
                const auto value {std::get_if<std::uint64_t>(&percentage->second)};
                EXPECT(value != nullptr && *value == 65);
            }
        }
    }
}

TEST_CASE(managedObjectsBecomeASnapshot)
{
    sd_bus* bus {nullptr};
    EXPECT(sd_bus_open_user(&bus) >= 0);
    if (bus == nullptr)
        return;

    const std::shared_ptr<sd_bus_message> message {buildManagedObjectsReply(bus)};
    sd_bus_unref(bus);

    EXPECT(message != nullptr);
    if (message == nullptr)
        return;

    BluetoothObjectMap objects;
    EXPECT(BluetoothMessageParser::readManagedObjects(message.get(), objects));

    BluetoothState state;
    state.setManagedObjects(objects);

    const BluetoothSnapshot snapshot {state.toSnapshot(1)};
    EXPECT(snapshot.available);
    EXPECT(snapshot.powered);
    EXPECT(snapshot.adapterPath == adapterPath);
    EXPECT(snapshot.adapterAddress == adapterAddress);

    EXPECT(snapshot.devices.size() == 1);
    if (snapshot.devices.size() == 1) {
        const BluetoothDevice& device {snapshot.devices.front()};
        EXPECT(device.name == "Pad");
        EXPECT(device.rssi == -42);
        EXPECT(device.battery == 65);
    }
}

#endif // BLUETOOTH_MANAGER_BLUEZ
