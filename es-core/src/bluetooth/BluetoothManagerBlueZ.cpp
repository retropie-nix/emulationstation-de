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
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    constexpr const char* sBlueZService {"org.bluez"};
    constexpr const char* sObjectManagerPath {"/"};
    constexpr const char* sObjectManagerInterface {"org.freedesktop.DBus.ObjectManager"};

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

    class BlueZBluetoothManager : public BluetoothManager
    {
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

        // The actions are implemented by the ticket that adds them; the command queue and the
        // snapshot bookkeeping they need are already in place.
        void setPowered(const bool) override { }
        void startDiscovery() override { }
        void stopDiscovery() override { }
        void pair(const std::string&) override { }
        void connect(const std::string&) override { }
        void disconnect(const std::string&) override { }
        void forget(const std::string&) override { }

    private:
        enum class Command { stop, refresh };

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
                mCommands.push_back(Command::stop);
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

        void setManagedObjects(const BluetoothObjectMap& objects)
        {
            mState.setManagedObjects(objects);
            mState.setLastError(std::string {});
            publish();
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

            std::lock_guard<std::mutex> lock {mSnapshotMutex};
            mSnapshot = std::move(snapshot);
        }

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

            for (const Command command : commands) {
                switch (command) {
                    case Command::stop:
                        sd_event_exit(mEvent, 0);
                        break;
                    case Command::refresh:
                        requestManagedObjects();
                        break;
                }
            }
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
                requestManagedObjects();
                return;
            }

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

        std::thread mThread;
        sd_event* mEvent {nullptr};
        sd_bus* mBus {nullptr};
        int mWakeFd {-1};

        std::atomic<std::uint64_t> mSequence {0};
        mutable std::mutex mSnapshotMutex;
        std::shared_ptr<const BluetoothSnapshot> mSnapshot;

        std::mutex mCommandMutex;
        std::vector<Command> mCommands;

        BluetoothState mState;
    };

} // namespace

std::unique_ptr<BluetoothManager> createBlueZBluetoothManager()
{
    return std::make_unique<BlueZBluetoothManager>();
}

#endif // BLUETOOTH_MANAGER_BLUEZ
