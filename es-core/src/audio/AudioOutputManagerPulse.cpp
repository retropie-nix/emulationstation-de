//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  AudioOutputManagerPulse.cpp
//
//  libpulse implementation of AudioOutputManager.
//
//  The threaded main loop runs the worker itself: everything that talks to the sound server
//  happens in its callbacks, and the UI thread only takes the lock for the duration of a call,
//  never waiting for it to complete.
//

#include "audio/AudioOutputManager.h"

#if defined(AUDIO_OUTPUT_PULSE)

#include "Log.h"
#include "audio/AudioSinkList.h"

#include <pulse/pulseaudio.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // How long to wait before trying to reach the sound server again. ES-DE can start before
    // pipewire-pulse does, so the first attempts are allowed to fail.
    constexpr unsigned int sFirstRetryDelay {500};
    constexpr unsigned int sMaxRetryDelay {30000};

    // BlueZ addresses are six groups of two hex digits, and only such a device.string is taken as
    // an address.
    bool looksLikeBluetoothAddress(const std::string& text)
    {
        if (text.size() != 17)
            return false;

        for (std::size_t i {0}; i < text.size(); ++i) {
            const bool isSeparator {i % 3 == 2};
            if (isSeparator) {
                if (text.at(i) != ':')
                    return false;
            }
            else if (std::isxdigit(static_cast<unsigned char>(text.at(i))) == 0) {
                return false;
            }
        }

        return true;
    }

    class PulseAudioOutputManager : public AudioOutputManager
    {
        // Carries the generation of one sink list reply through its callbacks.
        struct SinkListRequest {
            PulseAudioOutputManager* manager {nullptr};
            std::uint64_t generation {0};
        };

    public:
        PulseAudioOutputManager()
        {
            // A snapshot is published before anything else, so that getSnapshot() is never null.
            publish();

            mMainloop = pa_threaded_mainloop_new();
            if (mMainloop == nullptr) {
                LOG(LogError) << "Audio: could not create the sound server event loop";
                setError("The sound server is not available");
                return;
            }

            pa_threaded_mainloop_set_name(mMainloop, "audio-manager");

            if (pa_threaded_mainloop_start(mMainloop) < 0) {
                LOG(LogError) << "Audio: could not start the sound server event loop";
                pa_threaded_mainloop_free(mMainloop);
                mMainloop = nullptr;
                setError("The sound server is not available");
                return;
            }

            // The callbacks run on the loop thread from here on, so the connection is made under
            // the lock like every other operation.
            pa_threaded_mainloop_lock(mMainloop);
            connectContext();
            pa_threaded_mainloop_unlock(mMainloop);
        }

        ~PulseAudioOutputManager() override
        {
            if (mMainloop == nullptr)
                return;

            pa_threaded_mainloop_lock(mMainloop);
            mShuttingDown = true;

            if (mRetryEvent != nullptr) {
                pa_mainloop_api* api {pa_threaded_mainloop_get_api(mMainloop)};
                api->time_free(mRetryEvent);
                mRetryEvent = nullptr;
            }

            if (mContext != nullptr) {
                // Detach the callbacks first, so that shutting the context down cannot run any of
                // them while the members are being torn down.
                pa_context_set_state_callback(mContext, nullptr, nullptr);
                pa_context_set_subscribe_callback(mContext, nullptr, nullptr);

                // The replies of the list requests that are still running will never be delivered
                // once the callbacks are detached.
                cancelSinkRequests();

                pa_context_disconnect(mContext);
                pa_context_unref(mContext);
                mContext = nullptr;
            }

            // The lock has to be released before the loop is stopped.
            pa_threaded_mainloop_unlock(mMainloop);

            pa_threaded_mainloop_stop(mMainloop);
            pa_threaded_mainloop_free(mMainloop);
            mMainloop = nullptr;
        }

        bool isAvailable() const override
        {
            const std::shared_ptr<const AudioSnapshot> snapshot {getSnapshot()};
            return snapshot != nullptr && snapshot->available;
        }

        std::shared_ptr<const AudioSnapshot> getSnapshot() const override
        {
            std::lock_guard<std::mutex> lock {mSnapshotMutex};
            return mSnapshot;
        }

        std::uint64_t getSequence() const override { return mSequence; }

        void setDefault(const std::string& outputId) override
        {
            if (mMainloop == nullptr || outputId.empty())
                return;

            // The lock is only held for the call itself. Its completion arrives as a callback, and
            // the changed default as a subscription event.
            pa_threaded_mainloop_lock(mMainloop);

            if (mContext != nullptr && pa_context_get_state(mContext) == PA_CONTEXT_READY) {
                pa_operation* operation {pa_context_set_default_sink(mContext, outputId.c_str(),
                                                                     onDefaultChanged, this)};
                if (operation != nullptr) {
                    pa_operation_unref(operation);
                }
                else {
                    mLastError = "Could not change the audio output";
                    publish();
                }
            }

            pa_threaded_mainloop_unlock(mMainloop);
        }

        void refresh() override
        {
            if (mMainloop == nullptr)
                return;

            pa_threaded_mainloop_lock(mMainloop);

            if (mContext != nullptr && pa_context_get_state(mContext) == PA_CONTEXT_READY) {
                requestOutputs();
                requestServerInfo();
            }

            pa_threaded_mainloop_unlock(mMainloop);
        }

    private:
        // Everything below runs on the loop thread with the lock held.

        void connectContext()
        {
            if (mContext != nullptr || mMainloop == nullptr)
                return;

            mContext = pa_context_new(pa_threaded_mainloop_get_api(mMainloop), "ES-DE");
            if (mContext == nullptr) {
                LOG(LogError) << "Audio: could not create the sound server context";
                mLastError = "The sound server is not available";
                publish();
                scheduleRetry();
                return;
            }

            pa_context_set_state_callback(mContext, onContextState, this);

            // No autospawning: the appliance starts one sound server as a user service, and a
            // second one started behind its back would not be the one the games use.
            if (pa_context_connect(mContext, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
                // Connecting to a socket that is not there fails inside this call, and the failure
                // has already run the state callback, which released the context and scheduled a
                // retry. Only the errors that leave the context alone are handled here.
                if (mContext != nullptr) {
                    pa_context_set_state_callback(mContext, nullptr, nullptr);
                    pa_context_unref(mContext);
                    mContext = nullptr;

                    LOG(LogInfo) << "Audio: the sound server is not there yet, retrying";
                    mLastError = "The sound server is not available";
                    publish();
                    scheduleRetry();
                }
            }
        }

        void scheduleRetry()
        {
            if (mShuttingDown || mMainloop == nullptr || mRetryEvent != nullptr)
                return;

            struct timeval due {};
            gettimeofday(&due, nullptr);
            due.tv_sec += static_cast<time_t>(mRetryDelay / 1000);
            due.tv_usec += static_cast<suseconds_t>(mRetryDelay % 1000) * 1000;
            if (due.tv_usec >= 1000000) {
                due.tv_sec += 1;
                due.tv_usec -= 1000000;
            }

            pa_mainloop_api* api {pa_threaded_mainloop_get_api(mMainloop)};
            mRetryEvent = api->time_new(api, &due, onRetry, this);

            // Back off, so that a sound server that is never started is not polled every second.
            mRetryDelay = std::min(mRetryDelay * 2, sMaxRetryDelay);
        }

        // A list request owns one of these until its reply ends. The ones still running when the
        // context goes away are freed together in cancelSinkRequests().
        SinkListRequest* trackSinkRequest(const std::uint64_t generation)
        {
            auto* request {new SinkListRequest {this, generation}};
            mSinkRequests.push_back(request);
            return request;
        }

        void freeSinkRequest(SinkListRequest* request)
        {
            const auto found {std::find(mSinkRequests.begin(), mSinkRequests.end(), request)};
            if (found != mSinkRequests.end())
                mSinkRequests.erase(found);

            delete request;
        }

        void cancelSinkRequests()
        {
            for (SinkListRequest* request : mSinkRequests) {
                mSinkList.discard(request->generation);
                delete request;
            }

            mSinkRequests.clear();
        }

        void requestOutputs()
        {
            const std::uint64_t generation {mSinkList.start()};
            SinkListRequest* request {trackSinkRequest(generation)};

            pa_operation* operation {pa_context_get_sink_info_list(mContext, onSinkInfo, request)};
            if (operation == nullptr) {
                freeSinkRequest(request);
                mSinkList.discard(generation);

                mLastError = "Could not read the audio outputs";
                publish();
                return;
            }

            pa_operation_unref(operation);
        }

        // A complete reply replaces the list, unless a newer request has superseded it.
        void finishSinkList(const std::uint64_t generation)
        {
            std::vector<AudioOutput> outputs;
            if (!mSinkList.take(generation, outputs))
                return;

            mOutputs = std::move(outputs);
            publish();
        }

        void failSinkList(const std::uint64_t generation)
        {
            mSinkList.discard(generation);

            mLastError = "Could not read the audio outputs";
            publish();
        }

        void requestServerInfo()
        {
            pa_operation* operation {pa_context_get_server_info(mContext, onServerInfo, this)};
            if (operation == nullptr) {
                mLastError = "Could not read the audio server state";
                publish();
                return;
            }

            pa_operation_unref(operation);
        }

        // Called with the lock held or before the loop thread exists.
        void setError(const std::string& error)
        {
            mLastError = error;
            publish();
        }

        void handleDisconnect()
        {
            mAvailable = false;
            mOutputs.clear();
            mDefaultId.clear();

            // A list that was being read when the context went away will never finish.
            cancelSinkRequests();

            if (mContext != nullptr) {
                pa_context_set_state_callback(mContext, nullptr, nullptr);
                pa_context_set_subscribe_callback(mContext, nullptr, nullptr);
                pa_context_unref(mContext);
                mContext = nullptr;
            }

            if (mShuttingDown)
                return;

            mLastError = "The sound server is not available";
            publish();
            scheduleRetry();
        }

        void publish()
        {
            auto snapshot {std::make_shared<AudioSnapshot>()};
            snapshot->sequence = ++mSequence;
            snapshot->available = mAvailable;
            snapshot->defaultId = mDefaultId;
            snapshot->lastError = mLastError;
            snapshot->outputs = mOutputs;

            for (AudioOutput& output : snapshot->outputs)
                output.isDefault = !mDefaultId.empty() && output.id == mDefaultId;

            std::lock_guard<std::mutex> lock {mSnapshotMutex};
            mSnapshot = std::move(snapshot);
        }

        static void onContextState(pa_context* context, void* userdata)
        {
            auto* manager {static_cast<PulseAudioOutputManager*>(userdata)};

            switch (pa_context_get_state(context)) {
                case PA_CONTEXT_READY: {
                    manager->mAvailable = true;
                    manager->mRetryDelay = sFirstRetryDelay;
                    manager->mLastError.clear();

                    pa_context_set_subscribe_callback(context, onSubscription, manager);

                    pa_operation* subscribe {
                        pa_context_subscribe(context,
                                             static_cast<pa_subscription_mask_t>(
                                                 PA_SUBSCRIPTION_MASK_SINK |
                                                 PA_SUBSCRIPTION_MASK_SERVER),
                                             nullptr, nullptr)};
                    if (subscribe == nullptr)
                        manager->mLastError = "Could not watch the audio outputs";
                    else
                        pa_operation_unref(subscribe);

                    manager->requestOutputs();
                    manager->requestServerInfo();
                    manager->publish();
                    break;
                }

                case PA_CONTEXT_FAILED:
                case PA_CONTEXT_TERMINATED:
                    manager->handleDisconnect();
                    break;

                default:
                    break;
            }
        }

        static void onSubscription(pa_context*,
                                   pa_subscription_event_type_t type,
                                   uint32_t,
                                   void* userdata)
        {
            auto* manager {static_cast<PulseAudioOutputManager*>(userdata)};

            const pa_subscription_event_type_t facility {static_cast<pa_subscription_event_type_t>(
                type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK)};

            // A sink appeared or went away. CHANGE events follow those and only report a property
            // update, so re-listing on them would ask for the same list twice in a row.
            if (facility == PA_SUBSCRIPTION_EVENT_SINK) {
                const pa_subscription_event_type_t event {
                    static_cast<pa_subscription_event_type_t>(type &
                                                              PA_SUBSCRIPTION_EVENT_TYPE_MASK)};

                if (event == PA_SUBSCRIPTION_EVENT_NEW || event == PA_SUBSCRIPTION_EVENT_REMOVE)
                    manager->requestOutputs();

                return;
            }

            // The default output changed, usually because one was selected.
            if (facility == PA_SUBSCRIPTION_EVENT_SERVER)
                manager->requestServerInfo();
        }

        static void onSinkInfo(pa_context*, const pa_sink_info* info, int end, void* userdata)
        {
            auto* request {static_cast<SinkListRequest*>(userdata)};
            PulseAudioOutputManager* manager {request->manager};
            const std::uint64_t generation {request->generation};

            if (end != 0) {
                manager->freeSinkRequest(request);

                if (end < 0)
                    manager->failSinkList(generation);
                else
                    manager->finishSinkList(generation);

                return;
            }

            AudioOutput output;
            if (info->name != nullptr)
                output.id = info->name;
            if (info->description != nullptr)
                output.description = info->description;

            // PipeWire names the address in api.bluez5.address, while the native BlueZ modules only
            // report the device in device.string.
            if (info->proplist != nullptr) {
                const char* address {pa_proplist_gets(info->proplist, "api.bluez5.address")};
                const char* device {pa_proplist_gets(info->proplist, "device.string")};

                if (address != nullptr)
                    output.bluetoothAddress = address;
                else if (device != nullptr && looksLikeBluetoothAddress(device))
                    output.bluetoothAddress = device;
            }

            manager->mSinkList.collect(generation, output);
        }

        static void onServerInfo(pa_context*, const pa_server_info* info, void* userdata)
        {
            auto* manager {static_cast<PulseAudioOutputManager*>(userdata)};

            if (info == nullptr) {
                manager->mLastError = "Could not read the audio server state";
                manager->publish();
                return;
            }

            manager->mDefaultId = info->default_sink_name != nullptr ? info->default_sink_name : "";
            manager->publish();
        }

        static void onDefaultChanged(pa_context*, int success, void* userdata)
        {
            auto* manager {static_cast<PulseAudioOutputManager*>(userdata)};

            if (success == 0) {
                manager->mLastError = "Could not change the audio output";
                manager->publish();
                return;
            }

            manager->mLastError.clear();
            manager->requestServerInfo();
            manager->publish();
        }

        static void onRetry(pa_mainloop_api* api,
                            pa_time_event* event,
                            const struct timeval*,
                            void* userdata)
        {
            auto* manager {static_cast<PulseAudioOutputManager*>(userdata)};
            manager->mRetryEvent = nullptr;
            api->time_free(event);
            manager->connectContext();
        }

        pa_threaded_mainloop* mMainloop {nullptr};
        pa_context* mContext {nullptr};
        pa_time_event* mRetryEvent {nullptr};

        std::atomic<std::uint64_t> mSequence {0};
        mutable std::mutex mSnapshotMutex;
        std::shared_ptr<const AudioSnapshot> mSnapshot;

        // Loop thread only.
        AudioSinkList mSinkList;
        // The sink list requests whose replies have not ended yet.
        std::vector<SinkListRequest*> mSinkRequests;
        std::vector<AudioOutput> mOutputs;
        std::string mDefaultId;
        std::string mLastError;
        unsigned int mRetryDelay {sFirstRetryDelay};
        bool mAvailable {false};
        bool mShuttingDown {false};
    };

} // namespace

std::unique_ptr<AudioOutputManager> createPulseAudioOutputManager()
{
    return std::make_unique<PulseAudioOutputManager>();
}

#endif // AUDIO_OUTPUT_PULSE
