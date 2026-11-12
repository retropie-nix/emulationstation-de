//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothStateTests.cpp
//
//  Test cases for the reduction of the BlueZ objects into a BluetoothSnapshot.
//

#include "TestHarness.h"

#include "bluetooth/BluetoothState.h"

namespace
{
    constexpr const char* adapterPath {"/org/bluez/hci0"};
    constexpr const char* devicePath {"/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"};
    constexpr const char* secondDevicePath {"/org/bluez/hci0/dev_11_22_33_44_55_66"};
    constexpr const char* thirdDevicePath {"/org/bluez/hci0/dev_77_88_99_AA_BB_CC"};

    // The hands-free profile, which is what makes a device a headset rather than a speaker.
    constexpr const char* handsFreeUuid {"0000111e-0000-1000-8000-00805f9b34fb"};

    BluetoothProperties adapterProperties(const bool discovering = false)
    {
        return {
            {"Address", std::string {"10:20:30:40:50:60"}},
            {"Name", std::string {"amalthea"}},
            {"Alias", std::string {"amalthea"}},
            {"Powered", true},
            {"Discovering", discovering},
        };
    }

    BluetoothProperties deviceProperties(const std::string& name,
                                         const std::string& icon = "",
                                         const int rssi = 0,
                                         const bool paired = false)
    {
        BluetoothProperties properties {
            {"Address", std::string {"AA:BB:CC:DD:EE:FF"}},
            {"Connected", false},
            {"Paired", paired},
            {"Trusted", false},
        };

        if (!name.empty()) {
            properties["Alias"] = name;
            properties["Name"] = name;
        }
        if (!icon.empty())
            properties["Icon"] = icon;
        if (rssi != 0)
            properties["RSSI"] = static_cast<std::int64_t>(rssi);

        return properties;
    }

    BluetoothObjectMap singleAdapter(const bool discovering = false)
    {
        return {
            {adapterPath, {{"org.bluez.Adapter1", adapterProperties(discovering)}}},
        };
    }

    BluetoothInterfaces deviceInterfaces(const BluetoothProperties& properties,
                                         const BluetoothProperties& battery = {})
    {
        BluetoothInterfaces interfaces {{"org.bluez.Device1", properties}};
        if (!battery.empty())
            interfaces["org.bluez.Battery1"] = battery;

        // Interfaces that are not reduced are dropped while parsing, and the state has to ignore
        // them as well.
        interfaces["org.bluez.MediaControl1"] = {{"Connected", true}};

        return interfaces;
    }

} // namespace

TEST_CASE(adapterStateComesFromTheManagedObjects)
{
    BluetoothState state;
    state.setManagedObjects(singleAdapter());

    const BluetoothSnapshot snapshot {state.toSnapshot(7)};

    EXPECT(snapshot.available);
    EXPECT(snapshot.powered);
    EXPECT(!snapshot.discovering);
    EXPECT(snapshot.adapterPath == adapterPath);
    EXPECT(snapshot.adapterName == "amalthea");
    EXPECT(snapshot.adapterAddress == "10:20:30:40:50:60");
    EXPECT(snapshot.sequence == 7);
    EXPECT(snapshot.devices.empty());
}

TEST_CASE(noAdapterMeansUnavailable)
{
    BluetoothState state;
    const BluetoothSnapshot empty {state.toSnapshot(1)};
    EXPECT(!empty.available);
    EXPECT(!empty.powered);

    // A device without its adapter is not enough to call the service available.
    state.applyInterfacesAdded(devicePath, deviceInterfaces(deviceProperties("Pad")));
    EXPECT(!state.toSnapshot(2).available);
}

TEST_CASE(devicesAreReducedWithBatteryAndAddress)
{
    BluetoothState state;
    state.setManagedObjects(singleAdapter());
    state.applyInterfacesAdded(
        devicePath,
        deviceInterfaces(deviceProperties("WH-1000XM4", "audio-headset", -42, true),
                         {{"Percentage", static_cast<std::uint64_t>(65)}}));

    const BluetoothSnapshot snapshot {state.toSnapshot(3)};

    EXPECT(snapshot.devices.size() == 1);
    if (snapshot.devices.size() != 1)
        return;

    const BluetoothDevice& device {snapshot.devices.front()};
    EXPECT(device.path == devicePath);
    EXPECT(device.name == "WH-1000XM4");
    EXPECT(device.address == "AA:BB:CC:DD:EE:FF");
    EXPECT(device.icon == "audio-headset");
    EXPECT(device.paired);
    EXPECT(!device.connected);
    EXPECT(device.battery == 65);
    EXPECT(device.rssi == -42);
    EXPECT(device.kind == BluetoothDeviceKind::headset);
}

TEST_CASE(interfacesAddedAndRemovedAreApplied)
{
    BluetoothState state;
    state.setManagedObjects(singleAdapter());
    EXPECT(state.toSnapshot(1).devices.empty());

    state.applyInterfacesAdded(devicePath, deviceInterfaces(deviceProperties("Pad")));
    EXPECT(state.toSnapshot(2).devices.size() == 1);

    state.applyInterfacesAdded(secondDevicePath, deviceInterfaces(deviceProperties("Mouse")));
    EXPECT(state.toSnapshot(3).devices.size() == 2);

    state.applyInterfacesRemoved(secondDevicePath, {"org.bluez.Device1"});
    EXPECT(state.toSnapshot(4).devices.size() == 1);

    // Removing an interface the object does not have changes nothing.
    state.applyInterfacesRemoved(secondDevicePath, {"org.bluez.Device1"});
    EXPECT(state.toSnapshot(5).devices.size() == 1);

    // Removing the adapter is how a power-off or an unplugged dongle shows up.
    state.applyInterfacesRemoved(adapterPath, {"org.bluez.Adapter1"});
    const BluetoothSnapshot snapshot {state.toSnapshot(6)};
    EXPECT(!snapshot.available);
    EXPECT(snapshot.adapterPath.empty());
}

TEST_CASE(propertiesChangedUpdatesAndInvalidates)
{
    BluetoothState state;
    state.setManagedObjects(singleAdapter());
    state.applyInterfacesAdded(devicePath,
                               deviceInterfaces(deviceProperties("Pad", "input-gaming")));

    state.applyPropertiesChanged(devicePath,
                                 "org.bluez.Device1",
                                 {{"Connected", true}, {"RSSI", std::int64_t {-33}}});

    BluetoothSnapshot snapshot {state.toSnapshot(1)};
    EXPECT(snapshot.devices.size() == 1);
    if (snapshot.devices.size() != 1)
        return;
    EXPECT(snapshot.devices.front().connected);
    EXPECT(snapshot.devices.front().rssi == -33);

    // An invalidated property is removed, so the name falls back to the address, which the
    // snapshot does not use as a name.
    state.applyPropertiesChanged(devicePath, "org.bluez.Device1", {}, {"Alias", "Name"});
    snapshot = state.toSnapshot(2);
    EXPECT(snapshot.devices.size() == 1);
    if (snapshot.devices.size() != 1)
        return;
    EXPECT(snapshot.devices.front().name.empty());
}

TEST_CASE(propertiesChangedForUnknownObjectsIsIgnored)
{
    BluetoothState state;
    state.setManagedObjects(singleAdapter());

    state.applyPropertiesChanged("/org/bluez/hci0/dev_unknown",
                                 "org.bluez.Device1",
                                 {{"Paired", true}});
    state.applyPropertiesChanged(devicePath, "org.bluez.Device1", {{"Paired", true}});
    state.applyInterfacesAdded(devicePath, deviceInterfaces(deviceProperties("Pad")));
    state.applyPropertiesChanged(devicePath, "org.bluez.MediaControl1", {{"Connected", true}});

    EXPECT(state.toSnapshot(1).devices.size() == 1);
}

TEST_CASE(discoveryHidesUnnamedAdvertisers)
{
    BluetoothState state;
    state.setManagedObjects(singleAdapter(true));

    // An unnamed device that is not paired and has no device class is a Low Energy advertiser.
    state.applyInterfacesAdded(devicePath, deviceInterfaces(deviceProperties("")));

    // Paired devices are always shown, even without a name.
    BluetoothProperties paired {deviceProperties("")};
    paired["Paired"] = true;
    state.applyInterfacesAdded(secondDevicePath, deviceInterfaces(paired));

    // Classic devices report a device class.
    BluetoothProperties classic {deviceProperties("")};
    classic["Class"] = static_cast<std::uint64_t>(0x002508);
    state.applyInterfacesAdded(thirdDevicePath, deviceInterfaces(classic));

    EXPECT(state.toSnapshot(1).devices.size() == 2);

    // The filter only applies while scanning.
    BluetoothState idle;
    idle.setManagedObjects(singleAdapter(false));
    idle.applyInterfacesAdded(devicePath, deviceInterfaces(deviceProperties("")));
    EXPECT(idle.toSnapshot(2).devices.size() == 1);
}

TEST_CASE(devicesAreOrderedBySignalStrength)
{
    BluetoothState state;
    state.setManagedObjects(singleAdapter());

    state.applyInterfacesAdded(devicePath, deviceInterfaces(deviceProperties("weak", "", -80)));
    state.applyInterfacesAdded(secondDevicePath, deviceInterfaces(deviceProperties("unknown")));
    state.applyInterfacesAdded(thirdDevicePath,
                               deviceInterfaces(deviceProperties("strong", "", -35)));

    const BluetoothSnapshot snapshot {state.toSnapshot(1)};

    EXPECT(snapshot.devices.size() == 3);
    if (snapshot.devices.size() != 3)
        return;

    EXPECT(snapshot.devices.at(0).name == "strong");
    EXPECT(snapshot.devices.at(1).name == "weak");
    // A device without an RSSI sorts after the ones that report one.
    EXPECT(snapshot.devices.at(2).name == "unknown");
}

TEST_CASE(devicesAreOrderedByConnectionState)
{
    BluetoothState state;
    state.setManagedObjects(singleAdapter(false));

    // The stranger has the strongest signal, the paired device a weak one and the connected device
    // reports no signal at all, so an order by signal strength alone would list them the other way
    // around.
    state.applyInterfacesAdded(devicePath, deviceInterfaces(deviceProperties("stranger", "", -20)));
    state.applyInterfacesAdded(secondDevicePath,
                               deviceInterfaces(deviceProperties("paired", "", -60, true)));

    BluetoothProperties connected {deviceProperties("connected", "", 0, true)};
    connected["Connected"] = true;
    state.applyInterfacesAdded(thirdDevicePath, deviceInterfaces(connected));

    const BluetoothSnapshot snapshot {state.toSnapshot(1)};

    EXPECT(snapshot.devices.size() == 3);
    if (snapshot.devices.size() != 3)
        return;

    EXPECT(snapshot.devices.at(0).name == "connected");
    EXPECT(snapshot.devices.at(1).name == "paired");
    EXPECT(snapshot.devices.at(2).name == "stranger");
}

TEST_CASE(devicesAreClassified)
{
    BluetoothDevice device;

    device.icon = "audio-card";
    EXPECT(BluetoothState::classify(device) == BluetoothDeviceKind::speaker);

    device.icon = "audio-headphones";
    EXPECT(BluetoothState::classify(device) == BluetoothDeviceKind::headset);

    // Without an icon the service UUIDs decide, compared case insensitively.
    device.icon = "";
    device.uuids = {handsFreeUuid};
    EXPECT(BluetoothState::classify(device) == BluetoothDeviceKind::headset);

    device.uuids = {"0000110B-0000-1000-8000-00805F9B34FB"};
    EXPECT(BluetoothState::classify(device) == BluetoothDeviceKind::speaker);

    device.uuids = {};
    device.icon = "input-gaming";
    EXPECT(BluetoothState::classify(device) == BluetoothDeviceKind::controller);

    device.icon = "input-keyboard";
    EXPECT(BluetoothState::classify(device) == BluetoothDeviceKind::keyboard);

    device.icon = "input-mouse";
    EXPECT(BluetoothState::classify(device) == BluetoothDeviceKind::mouse);

    // An LE gamepad has no icon, but reports its appearance.
    device.icon = "";
    device.appearance = 0x03c4;
    EXPECT(BluetoothState::classify(device) == BluetoothDeviceKind::controller);

    device.appearance = 0x03c1;
    EXPECT(BluetoothState::classify(device) == BluetoothDeviceKind::keyboard);

    device.appearance = 0;
    EXPECT(BluetoothState::classify(device) == BluetoothDeviceKind::other);
}

TEST_CASE(errorsAreMappedToUserMessages)
{
    // org.bluez.Error.Failed is the generic name, the reason is only in the message.
    EXPECT(BluetoothState::describeError("org.bluez.Error.Failed",
                                         "br-connection-profile-unavailable") ==
           "br-connection-profile-unavailable");
    EXPECT(BluetoothState::describeError("org.bluez.Error.Failed", "") ==
           "The Bluetooth operation failed");

    EXPECT(BluetoothState::describeError("org.bluez.Error.AlreadyExists", "") ==
           "The device is already known");
    EXPECT(BluetoothState::describeError("org.bluez.Error.NotReady", "") ==
           "The Bluetooth adapter is not ready");

    // A known name keeps its explanation and adds the reason.
    EXPECT(BluetoothState::describeError("org.bluez.Error.AuthenticationFailed",
                                         "Pin code rejected") ==
           "The authentication failed: Pin code rejected");

    // An unknown name falls back on the message, and then on the name itself.
    EXPECT(BluetoothState::describeError("org.example.Error.Custom", "Something specific") ==
           "Something specific");
    EXPECT(BluetoothState::describeError("org.example.Error.Custom", "") ==
           "org.example.Error.Custom");
}

TEST_CASE(pendingOperationsAndErrorsReachTheSnapshot)
{
    BluetoothState state;
    state.setManagedObjects(singleAdapter());
    state.applyInterfacesAdded(devicePath, deviceInterfaces(deviceProperties("Pad")));

    state.setPending(devicePath, BluetoothOp::pairing);
    state.setLastError("The authentication failed");

    BluetoothSnapshot snapshot {state.toSnapshot(4)};
    EXPECT(snapshot.devices.size() == 1);
    if (snapshot.devices.size() != 1)
        return;
    EXPECT(snapshot.devices.front().pending == BluetoothOp::pairing);
    EXPECT(snapshot.lastError == "The authentication failed");

    state.clearPending(devicePath);
    state.setLastError("");
    snapshot = state.toSnapshot(5);
    EXPECT(snapshot.devices.size() == 1);
    if (snapshot.devices.size() != 1)
        return;
    EXPECT(snapshot.devices.front().pending == BluetoothOp::none);
    EXPECT(snapshot.lastError.empty());
}

TEST_CASE(clearForgetsEveryObject)
{
    BluetoothState state;
    state.setManagedObjects(singleAdapter());
    state.applyInterfacesAdded(devicePath, deviceInterfaces(deviceProperties("Pad")));
    state.setPending(devicePath, BluetoothOp::connecting);

    state.clear();

    const BluetoothSnapshot snapshot {state.toSnapshot(1)};
    EXPECT(!snapshot.available);
    EXPECT(snapshot.devices.empty());
}
