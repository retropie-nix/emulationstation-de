//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothManagerBlueZ.cpp
//
//  BlueZ implementation of BluetoothManager.
//
//  One sd_event loop on its own thread owns one system bus connection. The loop is the only thread
//  that touches the connection: nothing calls sd_bus_call, every method call is asynchronous and
//  every action enters through a command queue that an eventfd wakes up. The UI thread only reads
//  the published snapshot.
//

#include "bluetooth/BluetoothManager.h"

#if defined(BLUETOOTH_MANAGER_BLUEZ)

#include "Log.h"
#include "bluetooth/BluetoothState.h"

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    constexpr const char* sBlueZService {"org.bluez"};
    constexpr const char* sObjectManagerPath {"/"};
    constexpr const char* sObjectManagerInterface {"org.freedesktop.DBus.ObjectManager"};
    constexpr const char* sPropertiesInterface {"org.freedesktop.DBus.Properties"};
    constexpr const char* sAdapterInterface {"org.bluez.Adapter1"};
    constexpr const char* sDeviceInterface {"org.bluez.Device1"};
    constexpr const char* sAgentInterface {"org.bluez.Agent1"};
    constexpr const char* sAgentManagerInterface {"org.bluez.AgentManager1"};
    constexpr const char* sAgentManagerPath {"/org/bluez"};
    constexpr const char* sAgentPath {"/org/es_de/BluetoothAgent"};
    // Nothing is displayed and nothing is typed on an appliance with a game controller.
    constexpr const char* sAgentCapability {"NoInputNoOutput"};
    // bluetoothd clears Pairable by itself after this many seconds.
    constexpr std::uint32_t sPairableTimeout {180};

    // Reads one D-Bus variant into the reduced property representation. Returns false when the
    // value type is not one of the supported ones, in which case the caller skips it.
    bool readPropertyValue(sd_bus_message* message, BluetoothPropertyValue& value)
    {
        char type {0};
        const char* contents {nullptr};

        if (sd_bus_message_peek_type(message, &type, &contents) < 0)
            return false;

        switch (type) {
            case SD_BUS_TYPE_BOOLEAN: {
                int boolean {0};
                if (sd_bus_message_read(message, "b", &boolean) < 0)
                    return false;
                value = boolean != 0;
                return true;
            }
            case SD_BUS_TYPE_BYTE: {
                uint8_t number {0};
                if (sd_bus_message_read(message, "y", &number) < 0)
                    return false;
                value = static_cast<std::uint64_t>(number);
                return true;
            }
            case SD_BUS_TYPE_UINT16: {
                uint16_t number {0};
                if (sd_bus_message_read(message, "q", &number) < 0)
                    return false;
                value = static_cast<std::uint64_t>(number);
                return true;
            }
            case SD_BUS_TYPE_UINT32: {
                uint32_t number {0};
                if (sd_bus_message_read(message, "u", &number) < 0)
                    return false;
                value = static_cast<std::uint64_t>(number);
                return true;
            }
            case SD_BUS_TYPE_UINT64: {
                uint64_t number {0};
                if (sd_bus_message_read(message, "t", &number) < 0)
                    return false;
                value = number;
                return true;
            }
            case SD_BUS_TYPE_INT16: {
                int16_t number {0};
                if (sd_bus_message_read(message, "n", &number) < 0)
                    return false;
                value = static_cast<std::int64_t>(number);
                return true;
            }
            case SD_BUS_TYPE_INT32: {
                int32_t number {0};
                if (sd_bus_message_read(message, "i", &number) < 0)
                    return false;
                value = static_cast<std::int64_t>(number);
                return true;
            }
            case SD_BUS_TYPE_INT64: {
                int64_t number {0};
                if (sd_bus_message_read(message, "x", &number) < 0)
                    return false;
                value = static_cast<std::int64_t>(number);
                return true;
            }
            case SD_BUS_TYPE_STRING: {
                const char* text {nullptr};
                if (sd_bus_message_read(message, "s", &text) < 0)
                    return false;
                value = std::string {text};
                return true;
            }
            case SD_BUS_TYPE_OBJECT_PATH: {
                const char* path {nullptr};
                if (sd_bus_message_read(message, "o", &path) < 0)
                    return false;
                value = std::string {path};
                return true;
            }
            case SD_BUS_TYPE_ARRAY: {
                if (contents == nullptr || std::strcmp(contents, "s") != 0)
                    return false;

                std::vector<std::string> strings;
                if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "s") < 0)
                    return false;

                const char* text {nullptr};
                while (sd_bus_message_read(message, "s", &text) > 0)
                    strings.emplace_back(text);

                if (sd_bus_message_exit_container(message) < 0)
                    return false;

                value = std::move(strings);
                return true;
            }
            default:
                return false;
        }
    }

    // Reads an a{sv} dictionary, skipping the values that cannot be reduced.
    bool readProperties(sd_bus_message* message, BluetoothProperties& properties)
    {
        if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}") < 0)
            return false;

        while (true) {
            const int result {sd_bus_message_enter_container(message,
                                                             SD_BUS_TYPE_DICT_ENTRY,
                                                             "sv")};
            if (result < 0)
                return false;
            if (result == 0)
                break;

            const char* name {nullptr};
            if (sd_bus_message_read(message, "s", &name) < 0)
                return false;

            BluetoothPropertyValue value;
            if (readPropertyValue(message, value))
                properties[name] = std::move(value);
            else if (sd_bus_message_skip(message, nullptr) < 0)
                return false;

            if (sd_bus_message_exit_container(message) < 0)
                return false;
        }

        return sd_bus_message_exit_container(message) >= 0;
    }

    // Reads an a{sa{sv}} dictionary, keeping the interfaces that BluetoothState reduces.
    bool readInterfaces(sd_bus_message* message, BluetoothInterfaces& interfaces)
    {
        if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sa{sv}}") < 0)
            return false;

        while (true) {
            const int result {sd_bus_message_enter_container(message,
                                                             SD_BUS_TYPE_DICT_ENTRY,
                                                             "sa{sv}")};
            if (result < 0)
                return false;
            if (result == 0)
                break;

            const char* name {nullptr};
            if (sd_bus_message_read(message, "s", &name) < 0)
                return false;

            BluetoothProperties properties;
            if (!readProperties(message, properties))
                return false;

            if (BluetoothState::isKeptInterface(name))
                interfaces[name] = std::move(properties);

            if (sd_bus_message_exit_container(message) < 0)
                return false;
        }

        return sd_bus_message_exit_container(message) >= 0;
    }

    // Reads the a{oa{sa{sv}}} reply of GetManagedObjects.
    bool readManagedObjects(sd_bus_message* message, BluetoothObjectMap& objects)
    {
        if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{oa{sa{sv}}}") < 0)
            return false;

        while (true) {
            const int result {
                sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "oa{sa{sv}}")};
            if (result < 0)
                return false;
            if (result == 0)
                break;

            const char* path {nullptr};
            if (sd_bus_message_read(message, "o", &path) < 0)
                return false;

            BluetoothInterfaces interfaces;
            if (!readInterfaces(message, interfaces))
                return false;

            if (!interfaces.empty())
                objects[path] = std::move(interfaces);

            if (sd_bus_message_exit_container(message) < 0)
                return false;
        }

        return sd_bus_message_exit_container(message) >= 0;
    }

    // Reads an "as".
    bool readStringArray(sd_bus_message* message, std::vector<std::string>& strings)
    {
        if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "s") < 0)
            return false;

        const char* text {nullptr};
        while (sd_bus_message_read(message, "s", &text) > 0)
            strings.emplace_back(text);

        return sd_bus_message_exit_container(message) >= 0;
    }

    // Appends one entry to an open a{sv} container.
    bool appendStringProperty(sd_bus_message* message, const char* name, const char* value)
    {
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv") < 0)
            return false;
        if (sd_bus_message_append(message, "s", name) < 0)
            return false;
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "s") < 0)
            return false;
        if (sd_bus_message_append(message, "s", value) < 0)
            return false;
        if (sd_bus_message_close_container(message) < 0)
            return false;

        return sd_bus_message_close_container(message) >= 0;
    }

    bool appendBooleanProperty(sd_bus_message* message, const char* name, const bool value)
    {
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv") < 0)
            return false;
        if (sd_bus_message_append(message, "s", name) < 0)
            return false;
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "b") < 0)
            return false;
        if (sd_bus_message_append(message, "b", value ? 1 : 0) < 0)
            return false;
        if (sd_bus_message_close_container(message) < 0)
            return false;

        return sd_bus_message_close_container(message) >= 0;
    }

    class BlueZBluetoothManager : public BluetoothManager
    {
    private:
        // The method call that is waiting for its reply. A few actions are made of several calls,
        // and the step says which one is in flight.
        enum class Step {
            powered,
            discoveryFiltered,
            discoveryStarted,
            discoveryStopped,
            discoveryStoppedForPair,
            discoveryStoppedForConnect,
            trusted,
            paired,
            connected,
            disconnected,
            removed,
            removedCleanup,
            agentRegistered,
            agentDefault,
            agentUnregistered,
            pairableSet,
            pairableTimeoutSet
        };

        // What a completion callback needs besides the reply itself.
        struct FollowUp {
            Step step {Step::powered};
            std::string devicePath;
            BlueZBluetoothManager* manager {nullptr};
        };

        // An action posted by the UI thread.
        struct Command {
            enum class Type {
                stop,
                refresh,
                setPowered,
                startDiscovery,
                stopDiscovery,
                pair,
                connect,
                disconnect,
                forget,
                setPairingEnabled
            };

            Type type {Type::refresh};
            std::string path;
            // The boolean argument of the command: the state for setPowered and
            // setPairingEnabled, unused for everything else.
            bool state {false};
        };

    public:
        BlueZBluetoothManager()
        {
            mWakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (mWakeFd < 0) {
                LOG(LogError) << "Bluetooth: could not create the wake file descriptor: "
                              << std::strerror(errno);
                publish();
                return;
            }

            publish();
            mThread = std::thread {&BlueZBluetoothManager::run, this};
        }

        ~BlueZBluetoothManager() override { stop(); }

        bool isAvailable() const override
        {
            const std::shared_ptr<const BluetoothSnapshot> snapshot {getSnapshot()};
            return snapshot != nullptr && snapshot->available;
        }

        std::shared_ptr<const BluetoothSnapshot> getSnapshot() const override
        {
            std::lock_guard<std::mutex> lock {mSnapshotMutex};
            return mSnapshot;
        }

        std::uint64_t getSequence() const override { return mSequence; }

        // The actions only post a command and wake the loop, so the UI thread never waits for the
        // bus. Their progress and their outcome show up in later snapshots.
        void setPowered(const bool powered) override
        {
            postCommand({Command::Type::setPowered, std::string {}, powered});
        }

        void startDiscovery() override
        {
            postCommand({Command::Type::startDiscovery, std::string {}, false});
        }

        void stopDiscovery() override
        {
            postCommand({Command::Type::stopDiscovery, std::string {}, false});
        }

        void pair(const std::string& devicePath) override
        {
            postCommand({Command::Type::pair, devicePath, false});
        }

        void connect(const std::string& devicePath) override
        {
            postCommand({Command::Type::connect, devicePath, false});
        }

        void disconnect(const std::string& devicePath) override
        {
            postCommand({Command::Type::disconnect, devicePath, false});
        }

        void forget(const std::string& devicePath) override
        {
            postCommand({Command::Type::forget, devicePath, false});
        }

        void setPairingEnabled(const bool enabled) override
        {
            postCommand({Command::Type::setPairingEnabled, std::string {}, enabled});
        }

    private:
        void run()
        {
            if (sd_event_default(&mEvent) < 0) {
                LOG(LogError) << "Bluetooth: could not create the event loop";
                mEvent = nullptr;
                return;
            }

            if (sd_event_add_io(mEvent, nullptr, mWakeFd, EPOLLIN, onWake, this) < 0) {
                // Without the wake source the loop could never be stopped again.
                LOG(LogError) << "Bluetooth: could not watch the wake file descriptor";
                mEvent = sd_event_unref(mEvent);
                return;
            }

            if (sd_bus_open_system(&mBus) < 0) {
                LOG(LogWarning) << "Bluetooth: could not connect to the system bus";
                mBus = nullptr;
                setError("The Bluetooth service is not available");
            }
            else {
                sd_bus_attach_event(mBus, mEvent, SD_EVENT_PRIORITY_NORMAL);
                addMatches();
                requestManagedObjects();
            }

            const int result {sd_event_loop(mEvent)};
            if (result < 0) {
                LOG(LogError) << "Bluetooth: the event loop failed: " << std::strerror(-result);
            }

            if (mAgentSlot != nullptr)
                mAgentSlot = sd_bus_slot_unref(mAgentSlot);

            if (mBus != nullptr) {
                sd_bus_flush(mBus);
                mBus = sd_bus_unref(mBus);
            }
            mEvent = sd_event_unref(mEvent);
        }

        void stop()
        {
            if (!mThread.joinable())
                return;

            {
                std::lock_guard<std::mutex> lock {mCommandMutex};
                mCommands.push_back({Command::Type::stop, std::string {}, false});
            }
            wake();
            mThread.join();

            if (mWakeFd >= 0) {
                close(mWakeFd);
                mWakeFd = -1;
            }
        }

        void wake()
        {
            if (mWakeFd < 0)
                return;

            const std::uint64_t counter {1};
            if (write(mWakeFd, &counter, sizeof(counter)) < 0 && errno != EAGAIN) {
                LOG(LogWarning) << "Bluetooth: could not wake the event loop: "
                                << std::strerror(errno);
            }
        }

        void postCommand(Command command)
        {
            {
                std::lock_guard<std::mutex> lock {mCommandMutex};
                mCommands.push_back(std::move(command));
            }
            wake();
        }

        void addMatches()
        {
            const char* matches[] = {
                "type='signal',sender='org.bluez',path='/',"
                "interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded'",
                "type='signal',sender='org.bluez',path='/',"
                "interface='org.freedesktop.DBus.ObjectManager',member='InterfacesRemoved'",
                "type='signal',sender='org.bluez',"
                "interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
                "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
            };

            for (const char* match : matches) {
                if (sd_bus_add_match(mBus, nullptr, match, onSignal, this) < 0) {
                    LOG(LogWarning) << "Bluetooth: could not subscribe to a signal: " << match;
                }
            }
        }

        void requestManagedObjects()
        {
            if (mBus == nullptr)
                return;

            // No synchronous calls: the reply is handled by the same loop that owns the connection.
            const int result {sd_bus_call_method_async(mBus,
                                                       nullptr,
                                                       sBlueZService,
                                                       sObjectManagerPath,
                                                       sObjectManagerInterface,
                                                       "GetManagedObjects",
                                                       onGetManagedObjects,
                                                       this,
                                                       nullptr)};
            if (result < 0) {
                setError(std::string {"Could not read the Bluetooth devices: "} +
                         std::strerror(-result));
            }
        }

        //------------------------------------------------------------------------------------------
        // Actions, everything below runs on the loop thread.

        void performSetPowered(const bool powered)
        {
            if (!hasAdapter())
                return;

            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus,
                                               &message,
                                               sBlueZService,
                                               mAdapterPath.c_str(),
                                               sAdapterInterface,
                                               "SetPowered") < 0) {
                setError("Could not reach the Bluetooth service");
                return;
            }

            sd_bus_message_append(message, "b", powered ? 1 : 0);
            sendCall(message, Step::powered, std::string {});
        }

        void performStartDiscovery()
        {
            if (!hasAdapter())
                return;

            // Scanning while the screen is open is a good moment for a device to pair, and every
            // scan restarts the pairable timeout.
            if (mPairingEnabled)
                setPairable(true);

            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus,
                                               &message,
                                               sBlueZService,
                                               mAdapterPath.c_str(),
                                               sAdapterInterface,
                                               "SetDiscoveryFilter") < 0) {
                setError("Could not reach the Bluetooth service");
                return;
            }

            // "auto" scans both transports, and refusing duplicate reports keeps the adapter from
            // reporting the same advertisement over and over.
            sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
            appendStringProperty(message, "Transport", "auto");
            appendBooleanProperty(message, "DuplicateData", false);
            sd_bus_message_close_container(message);

            sendCall(message, Step::discoveryFiltered, std::string {});
        }

        void performStopDiscovery()
        {
            if (!hasAdapter())
                return;

            callMethod(mAdapterPath,
                       sAdapterInterface,
                       "StopDiscovery",
                       Step::discoveryStopped,
                       std::string {});
        }

        void performPair(const std::string& devicePath)
        {
            if (!beginOperation(devicePath, BluetoothOp::pairing))
                return;

            if (mPairingEnabled)
                setPairable(true);

            // Paging fails often while the adapter is still inquiring on these combo chips.
            if (mDiscovering) {
                callMethod(mAdapterPath,
                           sAdapterInterface,
                           "StopDiscovery",
                           Step::discoveryStoppedForPair,
                           devicePath);
                return;
            }

            setTrusted(devicePath);
        }

        void performConnect(const std::string& devicePath)
        {
            if (!beginOperation(devicePath, BluetoothOp::connecting))
                return;

            if (mDiscovering) {
                callMethod(mAdapterPath,
                           sAdapterInterface,
                           "StopDiscovery",
                           Step::discoveryStoppedForConnect,
                           devicePath);
                return;
            }

            callMethod(devicePath, sDeviceInterface, "Connect", Step::connected, devicePath);
        }

        void performDisconnect(const std::string& devicePath)
        {
            if (!beginOperation(devicePath, BluetoothOp::disconnecting))
                return;

            callMethod(devicePath, sDeviceInterface, "Disconnect", Step::disconnected, devicePath);
        }

        void performForget(const std::string& devicePath)
        {
            if (!hasAdapter())
                return;
            if (!beginOperation(devicePath, BluetoothOp::removing))
                return;

            removeDevice(devicePath, true);
        }

        // RemoveDevice lives on the adapter and takes the device it removes as its argument. The
        // quiet form is used to clean up after a failed pair, whose error is already reported.
        void removeDevice(const std::string& devicePath, const bool reportFailure)
        {
            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus,
                                               &message,
                                               sBlueZService,
                                               mAdapterPath.c_str(),
                                               sAdapterInterface,
                                               "RemoveDevice") < 0) {
                if (reportFailure) {
                    finishOperation(devicePath, true, "Could not reach the Bluetooth service");
                }
                return;
            }

            sd_bus_message_append(message, "o", devicePath.c_str());

            auto* followUp {new FollowUp {reportFailure ? Step::removed : Step::removedCleanup,
                                          devicePath,
                                          this}};
            const int result {
                sd_bus_call_async(mBus, nullptr, message, onFollowUpReply, followUp, 0)};
            sd_bus_message_unref(message);

            if (result < 0) {
                delete followUp;
                if (reportFailure) {
                    finishOperation(devicePath, true, "Could not reach the Bluetooth service");
                }
            }
        }

        // Starts a Properties.Set call for one property, the caller appends the value.
        sd_bus_message* newPropertySet(const std::string& objectPath,
                                       const char* interfaceName,
                                       const char* name)
        {
            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus,
                                               &message,
                                               sBlueZService,
                                               objectPath.c_str(),
                                               sPropertiesInterface,
                                               "Set") < 0)
                return nullptr;

            sd_bus_message_append(message, "ss", interfaceName, name);
            return message;
        }

        static void appendBooleanVariant(sd_bus_message* message, const bool value)
        {
            sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "b");
            sd_bus_message_append(message, "b", value ? 1 : 0);
            sd_bus_message_close_container(message);
        }

        static void appendUnsignedVariant(sd_bus_message* message, const std::uint32_t value)
        {
            sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "u");
            sd_bus_message_append(message, "u", value);
            sd_bus_message_close_container(message);
        }

        // BlueZ only consults a pairing agent for a device it does not trust yet, and a controller
        // opens its HID channel right after pairing, before any later SetTrusted could run.
        void setTrusted(const std::string& devicePath)
        {
            sd_bus_message* message {newPropertySet(devicePath, sDeviceInterface, "Trusted")};
            if (message == nullptr) {
                finishOperation(devicePath, true, "Could not reach the Bluetooth service");
                return;
            }

            appendBooleanVariant(message, true);
            sendCall(message, Step::trusted, devicePath);
        }

        bool hasAdapter()
        {
            if (!mAdapterPath.empty())
                return true;

            setError("There is no Bluetooth adapter");
            return false;
        }

        // Marks a device as busy. A second request for the same device is dropped while the first
        // one is still in flight.
        bool beginOperation(const std::string& devicePath, const BluetoothOp op)
        {
            if (mActiveOperations.find(devicePath) != mActiveOperations.end())
                return false;

            mActiveOperations[devicePath] = op;
            mState.setPending(devicePath, op);
            publish();
            return true;
        }

        void finishOperation(const std::string& devicePath,
                             const bool failed,
                             const std::string& error)
        {
            if (!devicePath.empty()) {
                mActiveOperations.erase(devicePath);
                mState.clearPending(devicePath);
            }

            mState.setLastError(failed ? error : std::string {});
            publish();
        }

        void cancelPendingOperations()
        {
            // The replies of the calls that are in flight will never be handled, so drop the
            // bookkeeping instead of leaving rows that spin forever in the last snapshot. The
            // busy map and the reducer's pending map are cleared together.
            for (const auto& operation : mActiveOperations)
                mState.clearPending(operation.first);

            mActiveOperations.clear();
            publish();
        }

        // Calls a method that takes no arguments.
        void callMethod(const std::string& objectPath,
                        const char* interfaceName,
                        const char* member,
                        const Step step,
                        const std::string& devicePath)
        {
            auto* followUp {new FollowUp {step, devicePath, this}};

            const int result {sd_bus_call_method_async(mBus,
                                                       nullptr,
                                                       sBlueZService,
                                                       objectPath.c_str(),
                                                       interfaceName,
                                                       member,
                                                       onFollowUpReply,
                                                       followUp,
                                                       nullptr)};
            if (result < 0) {
                delete followUp;
                finishOperation(devicePath, true, "Could not reach the Bluetooth service");
            }
        }

        // Sends a method call message that was built with arguments. The pairing setup calls do
        // not report failures to the user, a pair that cannot happen shows up as a failed pair.
        void sendCall(sd_bus_message* message,
                      const Step step,
                      const std::string& devicePath,
                      const bool reportFailure = true)
        {
            auto* followUp {new FollowUp {step, devicePath, this}};

            const int result {
                sd_bus_call_async(mBus, nullptr, message, onFollowUpReply, followUp, 0)};
            sd_bus_message_unref(message);

            if (result < 0) {
                delete followUp;
                if (reportFailure) {
                    finishOperation(devicePath, true, "Could not reach the Bluetooth service");
                }
                else {
                    LOG(LogWarning) << "Bluetooth: could not send a pairing setup call: "
                                    << std::strerror(-result);
                }
            }
        }

        void handleReply(const FollowUp& followUp, const bool failed, const std::string& error)
        {
            switch (followUp.step) {
                case Step::powered:
                case Step::discoveryStarted:
                case Step::discoveryStopped:
                case Step::connected:
                case Step::disconnected:
                case Step::removed:
                    finishOperation(followUp.devicePath, failed, error);
                    break;

                case Step::removedCleanup:
                    // Only drops the half paired device again, the pair failure was reported
                    // already, so a failure here is not shown to the user.
                    if (failed) {
                        LOG(LogWarning) << "Bluetooth: could not remove the half paired device: "
                                        << error;
                    }
                    break;

                case Step::discoveryFiltered:
                    if (failed) {
                        finishOperation(std::string {}, true, error);
                        break;
                    }
                    callMethod(mAdapterPath,
                               sAdapterInterface,
                               "StartDiscovery",
                               Step::discoveryStarted,
                               std::string {});
                    break;

                case Step::discoveryStoppedForPair:
                    // Best effort, the pair is attempted even when the adapter refused to stop.
                    setTrusted(followUp.devicePath);
                    break;

                case Step::discoveryStoppedForConnect:
                    callMethod(followUp.devicePath,
                               sDeviceInterface,
                               "Connect",
                               Step::connected,
                               followUp.devicePath);
                    break;

                case Step::trusted:
                    if (failed) {
                        finishOperation(followUp.devicePath, true, error);
                        break;
                    }
                    callMethod(followUp.devicePath,
                               sDeviceInterface,
                               "Pair",
                               Step::paired,
                               followUp.devicePath);
                    break;

                case Step::agentRegistered:
                    if (failed) {
                        LOG(LogWarning) << "Bluetooth: could not register the pairing agent: "
                                        << error;
                    }
                    else {
                        mAgentRegistered = true;
                    }
                    break;

                case Step::agentDefault:
                case Step::agentUnregistered:
                case Step::pairableSet:
                case Step::pairableTimeoutSet:
                    if (failed) {
                        LOG(LogWarning) << "Bluetooth: a pairing setup call failed: " << error;
                    }
                    break;

                case Step::paired:
                    if (!failed) {
                        finishOperation(followUp.devicePath, false, error);
                        break;
                    }
                    // A failed pair leaves a half paired entry behind. Report the failure first, so
                    // that the removal cannot replace its message.
                    finishOperation(followUp.devicePath, true, error);
                    removeDevice(followUp.devicePath, false);
                    break;
            }
        }


        //------------------------------------------------------------------------------------------
        // Pairing agent. Everything below runs on the loop thread.

        void performSetPairingEnabled(const bool enabled)
        {
            if (mPairingEnabled == enabled)
                return;

            mPairingEnabled = enabled;

            if (enabled)
                registerAgent();
            else
                unregisterAgent();
        }

        void registerAgent()
        {
            if (!hasAdapter())
                return;

            // The agent object lives on this connection, so it only has to be published once.
            if (mAgentSlot == nullptr) {
                const int result {sd_bus_add_object_vtable(mBus,
                                                           &mAgentSlot,
                                                           sAgentPath,
                                                           sAgentInterface,
                                                           agentVTable(),
                                                           this)};
                if (result < 0) {
                    LOG(LogWarning) << "Bluetooth: could not publish the pairing agent: "
                                    << std::strerror(-result);
                    mAgentSlot = nullptr;
                    return;
                }
            }

            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus,
                                               &message,
                                               sBlueZService,
                                               sAgentManagerPath,
                                               sAgentManagerInterface,
                                               "RegisterAgent") == 0) {
                sd_bus_message_append(message, "os", sAgentPath, sAgentCapability);
                sendCall(message, Step::agentRegistered, std::string {}, false);
            }

            sd_bus_message* defaultMessage {nullptr};
            if (sd_bus_message_new_method_call(mBus,
                                               &defaultMessage,
                                               sBlueZService,
                                               sAgentManagerPath,
                                               sAgentManagerInterface,
                                               "RequestDefaultAgent") == 0) {
                sd_bus_message_append(defaultMessage, "o", sAgentPath);
                sendCall(defaultMessage, Step::agentDefault, std::string {}, false);
            }

            setPairable(true);
        }

        void unregisterAgent()
        {
            if (mBus == nullptr)
                return;

            // bluetoothd forgets the registration when it restarts and Release tells us when it
            // dropped us, so only unregister while it still knows about the agent.
            if (mAgentRegistered) {
                sd_bus_message* message {nullptr};
                if (sd_bus_message_new_method_call(mBus,
                                                   &message,
                                                   sBlueZService,
                                                   sAgentManagerPath,
                                                   sAgentManagerInterface,
                                                   "UnregisterAgent") == 0) {
                    sd_bus_message_append(message, "o", sAgentPath);
                    sendCall(message, Step::agentUnregistered, std::string {}, false);
                }
            }

            if (mAgentSlot != nullptr)
                mAgentSlot = sd_bus_slot_unref(mAgentSlot);

            mAgentRegistered = false;
            setPairable(false);
        }

        // Pairable is what lets a device pair at all, and PairableTimeout makes bluetoothd clear it
        // again, so a screen that is never closed cannot leave the adapter pairable forever.
        void setPairable(const bool pairable)
        {
            if (!hasAdapter())
                return;

            if (pairable) {
                sd_bus_message* timeout {
                    newPropertySet(mAdapterPath, sAdapterInterface, "PairableTimeout")};
                if (timeout != nullptr) {
                    appendUnsignedVariant(timeout, sPairableTimeout);
                    sendCall(timeout, Step::pairableTimeoutSet, std::string {}, false);
                }
            }

            sd_bus_message* message {newPropertySet(mAdapterPath, sAdapterInterface, "Pairable")};
            if (message == nullptr)
                return;

            appendBooleanVariant(message, pairable);
            sendCall(message, Step::pairableSet, std::string {}, false);
        }

        // True for the device that has a pair in flight, which is the only one the agent serves.
        bool isPairingDevice(const std::string& devicePath) const
        {
            const auto operation {mActiveOperations.find(devicePath)};
            return operation != mActiveOperations.end() &&
                   operation->second == BluetoothOp::pairing;
        }

        static int rejectAgentRequest(sd_bus_error* error)
        {
            return sd_bus_error_set(error, "org.bluez.Error.Rejected",
                                    "The device is not being paired");
        }

        static int onAgentRelease(sd_bus_message* message, void* userdata, sd_bus_error*)
        {
            auto* manager {static_cast<BlueZBluetoothManager*>(userdata)};
            manager->mAgentRegistered = false;
            LOG(LogWarning) << "Bluetooth: the pairing agent was released by the Bluetooth service";

            return sd_bus_reply_method_return(message, nullptr);
        }

        static int onAgentCancel(sd_bus_message* message, void* userdata, sd_bus_error*)
        {
            auto* manager {static_cast<BlueZBluetoothManager*>(userdata)};

            // bluetoothd gave up on the pairing, so the operation in flight is over.
            std::string pairingPath;
            for (const auto& operation : manager->mActiveOperations) {
                if (operation.second == BluetoothOp::pairing) {
                    pairingPath = operation.first;
                    break;
                }
            }

            if (!pairingPath.empty())
                manager->finishOperation(pairingPath, true, "The pairing was canceled");
            else
                manager->publish();

            return sd_bus_reply_method_return(message, nullptr);
        }

        static int onAgentRequestPinCode(sd_bus_message* message,
                                         void* userdata,
                                         sd_bus_error* error)
        {
            auto* manager {static_cast<BlueZBluetoothManager*>(userdata)};

            const char* devicePath {nullptr};
            if (sd_bus_message_read(message, "o", &devicePath) < 0)
                return rejectAgentRequest(error);
            if (!manager->isPairingDevice(devicePath))
                return rejectAgentRequest(error);

            // Legacy PIN pairing, where the code has to come from somewhere and nothing here can
            // display or type one.
            return sd_bus_reply_method_return(message, "s", "0000");
        }

        static int onAgentRequestPasskey(sd_bus_message*, void*, sd_bus_error* error)
        {
            // A passkey would have to be typed on the device or read from this one.
            return rejectAgentRequest(error);
        }

        static int onAgentRequestConfirmation(sd_bus_message* message,
                                              void* userdata,
                                              sd_bus_error* error)
        {
            auto* manager {static_cast<BlueZBluetoothManager*>(userdata)};

            const char* devicePath {nullptr};
            std::uint32_t passkey {0};
            if (sd_bus_message_read(message, "ou", &devicePath, &passkey) < 0)
                return rejectAgentRequest(error);
            if (!manager->isPairingDevice(devicePath))
                return rejectAgentRequest(error);

            return sd_bus_reply_method_return(message, nullptr);
        }

        // Only the calls a NoInputNoOutput agent can answer are published.
        static const sd_bus_vtable* agentVTable()
        {
            static const sd_bus_vtable vtable[] = {
                SD_BUS_VTABLE_START(0),
                SD_BUS_METHOD("Release", "", "", onAgentRelease, SD_BUS_VTABLE_UNPRIVILEGED),
                SD_BUS_METHOD("Cancel", "", "", onAgentCancel, SD_BUS_VTABLE_UNPRIVILEGED),
                SD_BUS_METHOD("RequestPinCode",
                              "o",
                              "s",
                              onAgentRequestPinCode,
                              SD_BUS_VTABLE_UNPRIVILEGED),
                SD_BUS_METHOD("RequestPasskey",
                              "o",
                              "u",
                              onAgentRequestPasskey,
                              SD_BUS_VTABLE_UNPRIVILEGED),
                SD_BUS_METHOD("RequestConfirmation",
                              "ou",
                              "",
                              onAgentRequestConfirmation,
                              SD_BUS_VTABLE_UNPRIVILEGED),
                SD_BUS_METHOD("RequestAuthorization",
                              "o",
                              "",
                              onAgentRequestAuthorization,
                              SD_BUS_VTABLE_UNPRIVILEGED),
                SD_BUS_METHOD("AuthorizeService",
                              "os",
                              "",
                              onAgentAuthorizeService,
                              SD_BUS_VTABLE_UNPRIVILEGED),
                SD_BUS_VTABLE_END
            };

            return vtable;
        }

        static int onAgentRequestAuthorization(sd_bus_message*, void*, sd_bus_error* error)
        {
            // Only the device that is being paired may ask for anything.
            return rejectAgentRequest(error);
        }

        static int onAgentAuthorizeService(sd_bus_message* message,
                                           void* userdata,
                                           sd_bus_error* error)
        {
            auto* manager {static_cast<BlueZBluetoothManager*>(userdata)};

            const char* devicePath {nullptr};
            const char* uuid {nullptr};
            if (sd_bus_message_read(message, "os", &devicePath, &uuid) < 0)
                return rejectAgentRequest(error);
            if (!manager->isPairingDevice(devicePath))
                return rejectAgentRequest(error);

            return sd_bus_reply_method_return(message, nullptr);
        }

        //------------------------------------------------------------------------------------------
        // Snapshot.

        void setManagedObjects(const BluetoothObjectMap& objects)
        {
            mState.setManagedObjects(objects);
            mState.setLastError(std::string {});
            publish();

            // Both need the adapter path, so they wait for the enumeration to arrive.
            if (!mStartupStateCleared && !mAdapterPath.empty()) {
                // A crash with the Bluetooth screen open would otherwise leave the adapter
                // pairable.
                mStartupStateCleared = true;
                setPairable(false);
            }

            if (mReregisterAgent && !mAdapterPath.empty()) {
                // The registration belongs to bluetoothd, which forgot it when it restarted.
                mReregisterAgent = false;
                registerAgent();
            }
        }

        void setError(const std::string& error)
        {
            mState.setLastError(error);
            publish();
        }

        void publish()
        {
            const std::uint64_t sequence {++mSequence};
            std::shared_ptr<const BluetoothSnapshot> snapshot {
                std::make_shared<BluetoothSnapshot>(mState.toSnapshot(sequence))};

            // Read back by the actions, which run on this thread as well.
            mAdapterPath = snapshot->adapterPath;
            mDiscovering = snapshot->discovering;

            std::lock_guard<std::mutex> lock {mSnapshotMutex};
            mSnapshot = std::move(snapshot);
        }

        //------------------------------------------------------------------------------------------
        // Loop.

        void drainWake()
        {
            std::uint64_t counter {0};
            // The counter is only used as a flag, one read clears it.
            if (read(mWakeFd, &counter, sizeof(counter)) < 0 && errno != EAGAIN) {
                LOG(LogWarning) << "Bluetooth: could not drain the wake file descriptor";
            }
        }

        void processCommands()
        {
            std::vector<Command> commands;
            {
                std::lock_guard<std::mutex> lock {mCommandMutex};
                commands.swap(mCommands);
            }

            for (const Command& command : commands) {
                switch (command.type) {
                    case Command::Type::stop:
                        cancelPendingOperations();
                        sd_event_exit(mEvent, 0);
                        return;

                    case Command::Type::refresh:
                        requestManagedObjects();
                        break;

                    case Command::Type::setPowered:
                        if (ensureBus())
                            performSetPowered(command.state);
                        break;

                    case Command::Type::startDiscovery:
                        if (ensureBus())
                            performStartDiscovery();
                        break;

                    case Command::Type::stopDiscovery:
                        if (ensureBus())
                            performStopDiscovery();
                        break;

                    case Command::Type::pair:
                        if (ensureBus())
                            performPair(command.path);
                        break;

                    case Command::Type::connect:
                        if (ensureBus())
                            performConnect(command.path);
                        break;

                    case Command::Type::disconnect:
                        if (ensureBus())
                            performDisconnect(command.path);
                        break;

                    case Command::Type::forget:
                        if (ensureBus())
                            performForget(command.path);
                        break;

                    case Command::Type::setPairingEnabled:
                        if (ensureBus())
                            performSetPairingEnabled(command.state);
                        break;
                }
            }
        }

        bool ensureBus()
        {
            if (mBus != nullptr)
                return true;

            setError("The Bluetooth service is not available");
            return false;
        }

        void handleInterfacesAdded(sd_bus_message* message)
        {
            const char* path {nullptr};
            if (sd_bus_message_read(message, "o", &path) < 0)
                return;

            BluetoothInterfaces interfaces;
            if (!readInterfaces(message, interfaces) || interfaces.empty())
                return;

            mState.applyInterfacesAdded(path, interfaces);
            publish();
        }

        void handleInterfacesRemoved(sd_bus_message* message)
        {
            const char* path {nullptr};
            if (sd_bus_message_read(message, "o", &path) < 0)
                return;

            std::vector<std::string> interfaces;
            if (!readStringArray(message, interfaces))
                return;

            mState.applyInterfacesRemoved(path, interfaces);
            publish();
        }

        void handlePropertiesChanged(sd_bus_message* message)
        {
            const char* path {sd_bus_message_get_path(message)};
            const char* interfaceName {nullptr};
            if (path == nullptr || sd_bus_message_read(message, "s", &interfaceName) < 0)
                return;

            BluetoothProperties changed;
            if (!readProperties(message, changed))
                return;

            std::vector<std::string> invalidated;
            if (!readStringArray(message, invalidated))
                return;

            mState.applyPropertiesChanged(path, interfaceName, changed, invalidated);
            publish();
        }

        void handleNameOwnerChanged(sd_bus_message* message)
        {
            const char* name {nullptr};
            const char* oldOwner {nullptr};
            const char* newOwner {nullptr};

            if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) < 0)
                return;

            if (name == nullptr || std::strcmp(name, sBlueZService) != 0)
                return;

            // bluetoothd restarted, so every object it knew is gone. Its name coming back means the
            // new instance has to be enumerated; the matches survive, they belong to our
            // connection. Re-registering the pairing agent is done by the pairing agent itself.
            if (newOwner != nullptr && newOwner[0] != '\0') {
                // The registration belongs to bluetoothd, so it is redone once the objects are
                // there again: both the agent and Pairable need the adapter path.
                mReregisterAgent = mPairingEnabled;
                requestManagedObjects();
                return;
            }

            // Nothing can be in flight any more, as the service that would have replied is gone.
            // mActiveOperations and the reducer's pending map are cleared together, otherwise a
            // device that was mid-operation would be refused by beginOperation from now on.
            mActiveOperations.clear();
            mState.clear();
            publish();
        }

        static int onWake(sd_event_source*, int, uint32_t, void* userdata)
        {
            auto* manager {static_cast<BlueZBluetoothManager*>(userdata)};
            manager->drainWake();
            manager->processCommands();
            return 0;
        }

        static int onSignal(sd_bus_message* message, void* userdata, sd_bus_error*)
        {
            auto* manager {static_cast<BlueZBluetoothManager*>(userdata)};
            const char* member {sd_bus_message_get_member(message)};

            if (member == nullptr)
                return 0;

            const std::string name {member};
            if (name == "InterfacesAdded")
                manager->handleInterfacesAdded(message);
            else if (name == "InterfacesRemoved")
                manager->handleInterfacesRemoved(message);
            else if (name == "PropertiesChanged")
                manager->handlePropertiesChanged(message);
            else if (name == "NameOwnerChanged")
                manager->handleNameOwnerChanged(message);

            return 0;
        }

        static int onGetManagedObjects(sd_bus_message* message, void* userdata, sd_bus_error*)
        {
            auto* manager {static_cast<BlueZBluetoothManager*>(userdata)};

            if (sd_bus_message_is_method_error(message, nullptr)) {
                const sd_bus_error* error {sd_bus_message_get_error(message)};
                if (error != nullptr)
                    manager->setError(BluetoothState::describeError(
                        error->name == nullptr ? std::string {} : std::string {error->name},
                        error->message == nullptr ? std::string {} : std::string {error->message}));
                else
                    manager->setError("Could not read the Bluetooth devices");
                return 0;
            }

            BluetoothObjectMap objects;
            if (!readManagedObjects(message, objects)) {
                manager->setError("Could not read the Bluetooth devices");
                return 0;
            }

            manager->setManagedObjects(objects);
            return 0;
        }

        static int onFollowUpReply(sd_bus_message* message, void* userdata, sd_bus_error*)
        {
            auto* followUp {static_cast<FollowUp*>(userdata)};
            BlueZBluetoothManager* manager {followUp->manager};

            const bool failed {sd_bus_message_is_method_error(message, nullptr) > 0};
            std::string error;

            if (failed) {
                const sd_bus_error* busError {sd_bus_message_get_error(message)};
                error = BluetoothState::describeError(
                    busError != nullptr && busError->name != nullptr ? busError->name :
                                                                       std::string {},
                    busError != nullptr && busError->message != nullptr ? busError->message :
                                                                          std::string {});
            }

            manager->handleReply(*followUp, failed, error);
            delete followUp;
            return 0;
        }

        std::thread mThread;
        sd_event* mEvent {nullptr};
        sd_bus* mBus {nullptr};
        int mWakeFd {-1};

        std::atomic<std::uint64_t> mSequence {0};
        mutable std::mutex mSnapshotMutex;
        std::shared_ptr<const BluetoothSnapshot> mSnapshot;

        std::mutex mCommandMutex;
        std::vector<Command> mCommands;

        // Loop thread only.
        std::string mAdapterPath;
        bool mDiscovering {false};
        // The operations that are in flight, mirrored into the snapshot by the reducer's pending
        // map. The two have to be cleared together: a path left here would make beginOperation
        // refuse every later request for that device.
        std::map<std::string, BluetoothOp> mActiveOperations;

        BluetoothState mState;
        bool mPairingEnabled {false};
        bool mAgentRegistered {false};
        bool mReregisterAgent {false};
        bool mStartupStateCleared {false};
        sd_bus_slot* mAgentSlot {nullptr};
    };

} // namespace

std::unique_ptr<BluetoothManager> createBlueZBluetoothManager()
{
    return std::make_unique<BlueZBluetoothManager>();
}

#endif // BLUETOOTH_MANAGER_BLUEZ
