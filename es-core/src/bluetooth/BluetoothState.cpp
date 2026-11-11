//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothState.cpp
//
//  Platform independent reduction of the BlueZ D-Bus objects into a BluetoothSnapshot.
//

#include "bluetooth/BluetoothState.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace
{
    std::string toLowerAscii(const std::string& text)
    {
        std::string lowered {text};
        std::transform(lowered.begin(),
                       lowered.end(),
                       lowered.begin(),
                       [](const unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        return lowered;
    }

    bool equalsIgnoreCase(const std::string& left, const std::string& right)
    {
        return left.size() == right.size() && toLowerAscii(left) == toLowerAscii(right);
    }

    const BluetoothPropertyValue* findProperty(const BluetoothProperties& properties,
                                               const char* name)
    {
        const auto property {properties.find(name)};
        return property == properties.end() ? nullptr : &property->second;
    }

    bool getBoolean(const BluetoothProperties& properties, const char* name, const bool fallback)
    {
        const BluetoothPropertyValue* value {findProperty(properties, name)};
        if (value == nullptr)
            return fallback;

        const auto boolean {std::get_if<bool>(value)};
        return boolean == nullptr ? fallback : *boolean;
    }

    std::uint64_t getUnsigned(const BluetoothProperties& properties,
                              const char* name,
                              const std::uint64_t fallback)
    {
        const BluetoothPropertyValue* value {findProperty(properties, name)};
        if (value == nullptr)
            return fallback;

        if (const auto number {std::get_if<std::uint64_t>(value)}; number != nullptr)
            return *number;
        if (const auto number {std::get_if<std::int64_t>(value)}; number != nullptr)
            return *number < 0 ? fallback : static_cast<std::uint64_t>(*number);

        return fallback;
    }

    std::int64_t getSigned(const BluetoothProperties& properties,
                           const char* name,
                           const std::int64_t fallback)
    {
        const BluetoothPropertyValue* value {findProperty(properties, name)};
        if (value == nullptr)
            return fallback;

        if (const auto number {std::get_if<std::int64_t>(value)}; number != nullptr)
            return *number;
        if (const auto number {std::get_if<std::uint64_t>(value)}; number != nullptr)
            return *number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ?
                       fallback :
                       static_cast<std::int64_t>(*number);

        return fallback;
    }

    std::string getString(const BluetoothProperties& properties, const char* name)
    {
        const BluetoothPropertyValue* value {findProperty(properties, name)};
        if (value == nullptr)
            return std::string {};

        const auto text {std::get_if<std::string>(value)};
        return text == nullptr ? std::string {} : *text;
    }

    std::vector<std::string> getStrings(const BluetoothProperties& properties, const char* name)
    {
        const BluetoothPropertyValue* value {findProperty(properties, name)};
        if (value == nullptr)
            return std::vector<std::string> {};

        const auto strings {std::get_if<std::vector<std::string>>(value)};
        return strings == nullptr ? std::vector<std::string> {} : *strings;
    }

    bool hasUuid(const BluetoothDevice& device, const char* uuid)
    {
        return std::any_of(device.uuids.begin(),
                           device.uuids.end(),
                           [uuid](const std::string& candidate) {
                               return equalsIgnoreCase(candidate, uuid);
                           });
    }

    // The adapter of an appliance is the only one, so the first adapter object wins.
    const BluetoothProperties* findAdapterProperties(const BluetoothObjectMap& objects)
    {
        for (const auto& object : objects) {
            const auto adapter {object.second.find(BluetoothState::adapterInterface)};
            if (adapter != object.second.end())
                return &adapter->second;
        }
        return nullptr;
    }

    bool sortBySignalStrength(const BluetoothDevice& left, const BluetoothDevice& right)
    {
        // The strongest signal first. A device without an RSSI (0) sorts after the ones that
        // report one, which is correct because RSSI values are negative.
        const int leftRssi {
            left.rssi == 0 ? std::numeric_limits<int>::min() : left.rssi};
        const int rightRssi {
            right.rssi == 0 ? std::numeric_limits<int>::min() : right.rssi};

        if (leftRssi != rightRssi)
            return leftRssi > rightRssi;
        if (left.name != right.name)
            return left.name < right.name;
        return left.path < right.path;
    }

} // namespace

bool BluetoothState::isKeptInterface(const std::string& interfaceName)
{
    return interfaceName == adapterInterface || interfaceName == deviceInterface ||
           interfaceName == batteryInterface;
}

void BluetoothState::setManagedObjects(const BluetoothObjectMap& objects)
{
    mObjects.clear();

    for (const auto& object : objects) {
        BluetoothInterfaces kept;
        for (const auto& interface : object.second) {
            if (isKeptInterface(interface.first))
                kept[interface.first] = interface.second;
        }
        if (!kept.empty())
            mObjects[object.first] = std::move(kept);
    }
}

void BluetoothState::applyInterfacesAdded(const std::string& path,
                                          const BluetoothInterfaces& interfaces)
{
    BluetoothInterfaces kept;
    for (const auto& interface : interfaces) {
        if (isKeptInterface(interface.first))
            kept[interface.first] = interface.second;
    }
    if (kept.empty())
        return;

    for (const auto& interface : kept)
        mObjects[path][interface.first] = interface.second;
}

void BluetoothState::applyInterfacesRemoved(const std::string& path,
                                            const std::vector<std::string>& interfaces)
{
    const auto object {mObjects.find(path)};
    if (object == mObjects.end())
        return;

    for (const std::string& interface : interfaces)
        object->second.erase(interface);

    if (object->second.empty())
        mObjects.erase(object);
}

void BluetoothState::applyPropertiesChanged(const std::string& path,
                                            const std::string& interfaceName,
                                            const BluetoothProperties& changed,
                                            const std::vector<std::string>& invalidated)
{
    if (!isKeptInterface(interfaceName))
        return;

    const auto object {mObjects.find(path)};
    if (object == mObjects.end())
        return;

    const auto interface {object->second.find(interfaceName)};
    if (interface == object->second.end())
        return;

    for (const auto& property : changed)
        interface->second[property.first] = property.second;

    for (const std::string& property : invalidated)
        interface->second.erase(property);
}

void BluetoothState::clear()
{
    mObjects.clear();
    mPending.clear();
}

void BluetoothState::setPending(const std::string& devicePath, const BluetoothOp op)
{
    if (op == BluetoothOp::none)
        mPending.erase(devicePath);
    else
        mPending[devicePath] = op;
}

void BluetoothState::clearPending(const std::string& devicePath)
{
    mPending.erase(devicePath);
}

void BluetoothState::setLastError(const std::string& error)
{
    mLastError = error;
}

BluetoothSnapshot BluetoothState::toSnapshot(const std::uint64_t sequence) const
{
    BluetoothSnapshot snapshot;
    snapshot.sequence = sequence;
    snapshot.lastError = mLastError;

    if (const BluetoothProperties* adapter {findAdapterProperties(mObjects)};
        adapter != nullptr) {
        snapshot.available = true;
        snapshot.powered = getBoolean(*adapter, "Powered", false);
        snapshot.discovering = getBoolean(*adapter, "Discovering", false);
        snapshot.adapterAddress = getString(*adapter, "Address");
        snapshot.adapterName = getString(*adapter, "Alias");
        if (snapshot.adapterName.empty())
            snapshot.adapterName = getString(*adapter, "Name");
    }

    for (const auto& object : mObjects) {
        if (snapshot.adapterPath.empty() &&
            object.second.find(adapterInterface) != object.second.end())
            snapshot.adapterPath = object.first;

        const auto device {object.second.find(deviceInterface)};
        if (device == object.second.end())
            continue;

        BluetoothDevice entry;
        entry.path = object.first;
        entry.address = getString(device->second, "Address");
        entry.name = getString(device->second, "Alias");
        if (entry.name.empty())
            entry.name = getString(device->second, "Name");
        entry.icon = getString(device->second, "Icon");
        entry.uuids = getStrings(device->second, "UUIDs");
        entry.appearance = static_cast<std::uint32_t>(getUnsigned(device->second, "Appearance", 0));
        entry.deviceClass = static_cast<std::uint32_t>(getUnsigned(device->second, "Class", 0));
        entry.paired = getBoolean(device->second, "Paired", false);
        entry.connected = getBoolean(device->second, "Connected", false);
        entry.trusted = getBoolean(device->second, "Trusted", false);
        entry.rssi = static_cast<int>(getSigned(device->second, "RSSI", 0));

        const auto battery {object.second.find(batteryInterface)};
        if (battery != object.second.end())
            entry.battery = static_cast<int>(getUnsigned(battery->second, "Percentage", 0));

        const auto pending {mPending.find(entry.path)};
        entry.pending = pending == mPending.end() ? BluetoothOp::none : pending->second;

        entry.kind = classify(entry);

        // While scanning, an unnamed device that is not paired and does not report a device class
        // is an advertiser: it has nothing to show and would fill the list.
        if (snapshot.discovering && entry.name.empty() && !entry.paired && entry.deviceClass == 0)
            continue;

        snapshot.devices.push_back(std::move(entry));
    }

    std::sort(snapshot.devices.begin(), snapshot.devices.end(), sortBySignalStrength);

    return snapshot;
}

BluetoothDeviceKind BluetoothState::classify(const BluetoothDevice& device)
{
    // The icon is what BlueZ itself derived from the device class and the service UUIDs, so it is
    // the best single signal. Service UUIDs are used next, as they are advertised by a device that
    // was never connected, and the appearance last, as it is the only thing that identifies an LE
    // gamepad.
    const std::string icon {toLowerAscii(device.icon)};

    if (icon == "audio-headset" || icon == "audio-headphones")
        return BluetoothDeviceKind::headset;
    if (icon == "audio-card")
        return BluetoothDeviceKind::speaker;

    // A headset reports a hands-free or headset profile, a speaker usually only A2DP.
    if (hasUuid(device, "00001108-0000-1000-8000-00805f9b34fb") ||
        hasUuid(device, "0000111e-0000-1000-8000-00805f9b34fb"))
        return BluetoothDeviceKind::headset;
    if (hasUuid(device, "0000110a-0000-1000-8000-00805f9b34fb") ||
        hasUuid(device, "0000110b-0000-1000-8000-00805f9b34fb"))
        return BluetoothDeviceKind::speaker;

    if (icon == "input-keyboard")
        return BluetoothDeviceKind::keyboard;
    if (icon == "input-mouse")
        return BluetoothDeviceKind::mouse;
    if (icon == "input-gaming")
        return BluetoothDeviceKind::controller;

    // The subcategory of the appearance value: keyboards, mice, joysticks and gamepads.
    switch (device.appearance & 0x03ff) {
        case 0x03c1:
            return BluetoothDeviceKind::keyboard;
        case 0x03c2:
            return BluetoothDeviceKind::mouse;
        case 0x03c3:
        case 0x03c4:
            return BluetoothDeviceKind::controller;
        default:
            break;
    }

    return BluetoothDeviceKind::other;
}

std::string BluetoothState::describeError(const std::string& errorName, const std::string& message)
{
    static const std::map<std::string, std::string> descriptions {
        {"org.bluez.Error.Failed", "The Bluetooth operation failed"},
        {"org.bluez.Error.InProgress", "The Bluetooth operation is already in progress"},
        {"org.bluez.Error.AlreadyExists", "The device is already known"},
        {"org.bluez.Error.DoesNotExist", "The device is no longer known"},
        {"org.bluez.Error.NotSupported", "The operation is not supported by the device"},
        {"org.bluez.Error.NotReady", "The Bluetooth adapter is not ready"},
        {"org.bluez.Error.NotAuthorized", "The operation is not authorized"},
        {"org.bluez.Error.NotPermitted", "The operation is not permitted"},
        {"org.bluez.Error.InvalidArguments", "The operation was not accepted"},
        {"org.bluez.Error.AuthenticationFailed", "The authentication failed"},
        {"org.bluez.Error.AuthenticationTimeout", "The authentication timed out"},
        {"org.bluez.Error.AuthenticationRejected", "The authentication was rejected"},
        {"org.bluez.Error.AuthenticationCanceled", "The authentication was canceled"},
        {"org.bluez.Error.ConnectionAttemptFailed", "The connection attempt failed"},
        {"org.bluez.Error.NoSuchAdapter", "There is no Bluetooth adapter"},
        {"org.bluez.Error.Blocked", "The Bluetooth adapter is blocked"},
        {"org.freedesktop.DBus.Error.ServiceUnknown", "The Bluetooth service is not running"},
        {"org.freedesktop.DBus.Error.NoReply", "The Bluetooth service did not reply"},
        {"org.freedesktop.DBus.Error.TimedOut", "The Bluetooth service did not reply in time"},
        {"org.freedesktop.DBus.Error.AccessDenied", "Access to the Bluetooth service was denied"},
    };

    const auto description {descriptions.find(errorName)};

    // BlueZ reports the reason for a failure only in the message string, so prefer it.
    if (errorName == "org.bluez.Error.Failed") {
        if (!message.empty())
            return message;
        return description != descriptions.end() ? description->second :
                                                   "The Bluetooth operation failed";
    }

    if (description != descriptions.end()) {
        if (message.empty())
            return description->second;
        return description->second + ": " + message;
    }

    if (!message.empty())
        return message;

    return errorName;
}
