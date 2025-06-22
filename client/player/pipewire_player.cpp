/***
    This file is part of snapcast
    Copyright (C) 2014-2025  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

// prototype/interface header file
#include "pipewire_player.hpp"

// local headers
#include "common/aixlog.hpp"
#include "common/snap_exception.hpp"
#include "common/str_compat.hpp"
#include "common/utils/logging.hpp"
#include "common/utils/string_utils.hpp"

// standard headers
#include <iostream>
#include <thread>
#include <cstring>

using namespace std::chrono_literals;
using namespace std;

namespace player
{

static constexpr std::chrono::milliseconds BUFFER_TIME = 100ms;
static constexpr auto LOG_TAG = "PipeWirePlayer";

// Global device list and synchronization data for enumeration
static std::vector<PcmDevice> g_devices;
struct EnumData {
    int pending;
    struct pw_main_loop* loop;
};

// C++11 compatible stream events initialization
struct pw_stream_events PipeWirePlayer::get_stream_events()
{
    struct pw_stream_events events = {};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.destroy = nullptr;
    events.state_changed = on_state_changed;
    events.control_info = nullptr;
    events.io_changed = on_io_changed;
    events.param_changed = on_param_changed;
    events.add_buffer = nullptr;
    events.remove_buffer = nullptr;
    events.process = on_process;
    events.drained = on_drained;
    return events;
}

// C++11 compatible registry events initialization
struct pw_registry_events PipeWirePlayer::get_registry_events()
{
    struct pw_registry_events events = {};
    events.version = PW_VERSION_REGISTRY_EVENTS;
    events.global = registry_event_global;
    events.global_remove = registry_event_global_remove;
    return events;
}

std::vector<PcmDevice> PipeWirePlayer::pcm_list(const std::string& parameter)
{
    std::ignore = parameter;
    
    pw_init(nullptr, nullptr);
    
    auto* main_loop = pw_main_loop_new(nullptr);
    if (!main_loop)
        throw SnapException("Failed to create PipeWire main loop");
    
    auto* context = pw_context_new(pw_main_loop_get_loop(main_loop), nullptr, 0);
    if (!context)
    {
        pw_main_loop_destroy(main_loop);
        throw SnapException("Failed to create PipeWire context");
    }
    
    auto* core = pw_context_connect(context, nullptr, 0);
    if (!core)
    {
        pw_context_destroy(context);
        pw_main_loop_destroy(main_loop);
        throw SnapException("Failed to connect to PipeWire core");
    }
    
    auto* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (!registry)
    {
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(main_loop);
        throw SnapException("Failed to get PipeWire registry");
    }
    
    g_devices.clear();
    
    // Set up enumeration data
    EnumData enum_data = { 1, main_loop };
    
    // Add registry listener
    struct spa_hook registry_hook;
    auto registry_events = get_registry_events();
    pw_registry_add_listener(registry, &registry_hook, &registry_events, nullptr);
    
    // Add core listener for synchronization
    struct spa_hook core_listener;
    struct pw_core_events core_events = {};
    core_events.version = PW_VERSION_CORE_EVENTS;
    core_events.done = [](void *data, uint32_t id, int seq) {
        auto* d = static_cast<EnumData*>(data);
        if (id == PW_ID_CORE && seq == 0) {
            d->pending--;
            if (d->pending <= 0)
                pw_main_loop_quit(d->loop);
        }
    };
    
    pw_core_add_listener(core, &core_listener, &core_events, &enum_data);
    pw_core_sync(core, PW_ID_CORE, 0);
    
    // Run until enumeration is complete
    pw_main_loop_run(main_loop);
    
    // Cleanup
    spa_hook_remove(&core_listener);
    spa_hook_remove(&registry_hook);
    pw_proxy_destroy((struct pw_proxy*)registry);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(main_loop);
    
    // Add default device
    g_devices.emplace(g_devices.begin(), -1, DEFAULT_DEVICE, "Let PipeWire choose the device");
    
    return g_devices;
}

void PipeWirePlayer::registry_event_global(void* data, uint32_t id, uint32_t permissions, const char* type, uint32_t version, const struct spa_dict* props)
{
    std::ignore = data;
    std::ignore = permissions;
    std::ignore = version;
    
    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
    {
        const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        
        if (media_class && strcmp(media_class, "Audio/Sink") == 0 && name && description)
        {
            g_devices.emplace_back(id, name, description);
            LOG(DEBUG, LOG_TAG) << "Found audio sink: " << name << " (" << description << ")\n";
        }
    }
}

void PipeWirePlayer::registry_event_global_remove(void* data, uint32_t id)
{
    std::ignore = data;
    std::ignore = id;
    // Remove device from list if needed
}

PipeWirePlayer::PipeWirePlayer(boost::asio::io_context& io_context, const ClientSettings::Player& settings, std::shared_ptr<Stream> stream)
    : Player(io_context, settings, std::move(stream)), 
      latency_(BUFFER_TIME), 
      underflows_(0),
      stream_ready_(false),
      last_chunk_tick_(0),
      main_loop_(nullptr), 
      context_(nullptr), 
      core_(nullptr), 
      pw_stream_(nullptr), 
      registry_(nullptr),
      target_node_(std::nullopt), 
      node_id_(0), 
      frame_size_(0),
      position_(nullptr)
{
    auto params = utils::string::split_pairs_to_container<std::vector<std::string>>(settings.parameter, ',', '=');
    
    if (params.find("buffer_time") != params.end())
        latency_ = std::chrono::milliseconds(std::max(cpt::stoi(params["buffer_time"].front()), 10));
    
    if (params.find("target") != params.end())
        target_node_ = params["target"].front();
    
    // Set default properties
    properties_[PW_KEY_MEDIA_TYPE] = "Audio";
    properties_[PW_KEY_MEDIA_CATEGORY] = "Playback";
    properties_[PW_KEY_MEDIA_ROLE] = "Music";
    properties_[PW_KEY_APP_NAME] = "Snapcast";
    properties_[PW_KEY_APP_ID] = "snapcast";
    properties_[PW_KEY_APP_ICON_NAME] = "snapcast";
    properties_[PW_KEY_NODE_NAME] = "Snapcast";
    properties_[PW_KEY_NODE_DESCRIPTION] = "Snapcast Audio Stream";
    
    // Calculate latency in samples
    const SampleFormat& format = stream_->getFormat();
    uint32_t latency_samples = (latency_.count() * format.rate()) / 1000;
    properties_[PW_KEY_NODE_LATENCY] = std::to_string(latency_samples) + "/" + std::to_string(format.rate());
    
    // Process custom properties
    if (params.find("property") != params.end())
    {
        for (const auto& p : params["property"])
        {
            std::string value;
            std::string key = utils::string::split_left(p, '=', value);
            if (!key.empty())
                properties_[key] = value;
        }
    }
    
    for (const auto& property : properties_)
    {
        if (!property.second.empty())
            LOG(INFO, LOG_TAG) << "Setting property \"" << property.first << "\" to \"" << property.second << "\"\n";
    }
    
    LOG(INFO, LOG_TAG) << "Using buffer_time: " << latency_.count() / 1000 << " ms, target: " << target_node_.value_or("default") << "\n";
}

PipeWirePlayer::~PipeWirePlayer()
{
    LOG(DEBUG, LOG_TAG) << "Destructor\n";
    stop();
}

bool PipeWirePlayer::needsThread() const
{
    return true;
}

void PipeWirePlayer::worker()
{
    while (active_)
    {
        if (main_loop_)
            pw_main_loop_run(main_loop_);
        
        // if we are still active, wait for a chunk and attempt to reconnect
        while (active_ && !stream_->waitForChunk(100ms))
        {
            static utils::logging::TimeConditional cond(2s);
            LOG(DEBUG, LOG_TAG) << cond << "Waiting for a chunk to become available before reconnecting\n";
        }
        
        while (active_)
        {
            LOG(INFO, LOG_TAG) << "Chunk available, reconnecting to PipeWire\n";
            try
            {
                connect();
                break;
            }
            catch (const std::exception& e)
            {
                LOG(ERROR, LOG_TAG) << "Exception while connecting to PipeWire: " << e.what() << "\n";
                disconnect();
                chronos::sleep(100);
            }
        }
    }
}

void PipeWirePlayer::start()
{
    LOG(INFO, LOG_TAG) << "Start\n";
    
    pw_init(nullptr, nullptr);
    this->connect();
    Player::start();
}

void PipeWirePlayer::connect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    LOG(INFO, LOG_TAG) << "Connecting to PipeWire\n";
    
    if (settings_.pcm_device.idx == -1)
        throw SnapException("Can't open " + settings_.pcm_device.name + ", error: No such device");
    
    const SampleFormat& format = stream_->getFormat();
    
    // Set up audio format
    spa_zero(audio_info_);
    audio_info_.format = SPA_AUDIO_FORMAT_UNKNOWN;
    
    if (format.bits() == 8)
        audio_info_.format = SPA_AUDIO_FORMAT_U8;
    else if (format.bits() == 16)
        audio_info_.format = SPA_AUDIO_FORMAT_S16_LE;
    else if ((format.bits() == 24) && (format.sampleSize() == 3))
        audio_info_.format = SPA_AUDIO_FORMAT_S24_LE;
    else if ((format.bits() == 24) && (format.sampleSize() == 4))
        audio_info_.format = SPA_AUDIO_FORMAT_S24_32_LE;
    else if (format.bits() == 32)
        audio_info_.format = SPA_AUDIO_FORMAT_S32_LE;
    else
        throw SnapException("Unsupported sample format \"" + cpt::to_string(format.bits()) + "\"");
    
    audio_info_.channels = format.channels();
    audio_info_.rate = format.rate();
    frame_size_ = format.frameSize();
    
    // Create main loop
    main_loop_ = pw_main_loop_new(nullptr);
    if (!main_loop_)
        throw SnapException("Failed to create PipeWire main loop");
    
    // Create context
    context_ = pw_context_new(pw_main_loop_get_loop(main_loop_), nullptr, 0);
    if (!context_)
        throw SnapException("Failed to create PipeWire context");
    
    // Connect to core
    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_)
        throw SnapException("Failed to connect to PipeWire core");
    
    // Create stream properties
    struct pw_properties* props = pw_properties_new(nullptr, nullptr);
    for (const auto& property : properties_)
    {
        if (!property.second.empty())
            pw_properties_set(props, property.first.c_str(), property.second.c_str());
    }
    
    // Set target node if specified
    if (target_node_.has_value() && target_node_.value() != DEFAULT_DEVICE)
        pw_properties_set(props, PW_KEY_NODE_TARGET, target_node_.value().c_str());
    else if (settings_.pcm_device.name != DEFAULT_DEVICE)
        pw_properties_set(props, PW_KEY_NODE_TARGET, settings_.pcm_device.name.c_str());
    
    // Create playback stream
    pw_stream_ = pw_stream_new(core_, "Snapcast Playback", props);
    if (!pw_stream_)
        throw SnapException("Failed to create PipeWire stream");
    
    // Add stream listener
    auto stream_events = get_stream_events();
    pw_stream_add_listener(pw_stream_, &stream_listener_, &stream_events, this);
    
    // Create audio format parameters using spa_pod_builder
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    
    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audio_info_);
    
    // Connect stream
    if (pw_stream_connect(pw_stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                         static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | 
                                                      PW_STREAM_FLAG_MAP_BUFFERS | 
                                                      PW_STREAM_FLAG_RT_PROCESS),
                         params, 1) < 0)
    {
        throw SnapException("Failed to connect PipeWire stream");
    }
    
    // Wait for stream to be ready
    stream_ready_ = false;
    auto wait_start = std::chrono::steady_clock::now();
    while (!stream_ready_ && active_)
    {
        auto now = std::chrono::steady_clock::now();
        if (now - wait_start > 5s)
            throw SnapException("Timeout while waiting for PipeWire stream to become ready");
        
        pw_main_loop_run(main_loop_);
        std::this_thread::sleep_for(1ms);
    }
    
    last_chunk_tick_ = chronos::getTickCount();
}

void PipeWirePlayer::stop()
{
    LOG(INFO, LOG_TAG) << "Stop\n";
    
    active_ = false;
    this->disconnect();
    Player::stop();
}

void PipeWirePlayer::disconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    LOG(INFO, LOG_TAG) << "Disconnecting from PipeWire\n";
    
    stream_ready_ = false;
    
    if (main_loop_)
        pw_main_loop_quit(main_loop_);
    
    if (pw_stream_)
    {
        spa_hook_remove(&stream_listener_);
        pw_stream_destroy(pw_stream_);
        pw_stream_ = nullptr;
    }
    
    if (core_)
    {
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    
    if (context_)
    {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    
    if (main_loop_)
    {
        pw_main_loop_destroy(main_loop_);
        main_loop_ = nullptr;
    }
}

void PipeWirePlayer::setHardwareVolume(const Volume& volume)
{
    if (!pw_stream_ || !stream_ready_)
        return;
    
    // Volume struct has volume in range [0..1] and mute flag
    float vol = static_cast<float>(volume.volume);
    
    // If muted, set volume to 0
    if (volume.mute)
        vol = 0.0f;
    
    float values[2] = { vol, vol };  // Same volume for both channels
    
    int ret = pw_stream_set_control(pw_stream_, SPA_PROP_channelVolumes, 2, values, 0);
    
    if (ret >= 0)
    {
        LOG(DEBUG, LOG_TAG) << "Set hardware volume to " << (volume.volume * 100.0) 
                            << "% (mute: " << volume.mute << ")\n";
        
        // Update our internal volume state
        volume_ = volume;
    }
    else
    {
        LOG(ERROR, LOG_TAG) << "Failed to set hardware volume\n";
    }
}

bool PipeWirePlayer::getHardwareVolume(Volume& volume)
{
    if (!pw_stream_ || !stream_ready_)
        return false;
    
    // PipeWire doesn't have a standard way to get volume yet
    // For now, return the last set volume from the base class
    volume = volume_;
    return false;  // Indicate we can't retrieve actual hardware volume
}

void PipeWirePlayer::on_state_changed(void* userdata, enum pw_stream_state old, enum pw_stream_state state, const char* error)
{
    auto* self = static_cast<PipeWirePlayer*>(userdata);
    
    LOG(DEBUG, LOG_TAG) << "Stream state changed from " << pw_stream_state_as_string(old) 
                        << " to " << pw_stream_state_as_string(state);
    if (error)
        LOG(DEBUG, LOG_TAG) << " (error: " << error << ")";
    LOG(DEBUG, LOG_TAG) << "\n";
    
    switch (state)
    {
        case PW_STREAM_STATE_STREAMING:
            self->stream_ready_ = true;
            self->node_id_ = pw_stream_get_node_id(self->pw_stream_);
            LOG(INFO, LOG_TAG) << "Stream node " << self->node_id_ << " streaming\n";
            
            // Set initial volume if not muted
            if (!self->volume_.mute && self->volume_.volume > 0.0)
            {
                self->setHardwareVolume(self->volume_);
            }
            break;
            
        case PW_STREAM_STATE_ERROR:
            LOG(ERROR, LOG_TAG) << "Stream error: " << (error ? error : "unknown") << "\n";
            self->stream_ready_ = false;
            if (self->active_.load(std::memory_order_acquire))
            {
                // Attempt reconnection by breaking out of main loop
                pw_main_loop_quit(self->main_loop_);
            }
            break;
            
        case PW_STREAM_STATE_UNCONNECTED:
            LOG(INFO, LOG_TAG) << "Stream disconnected\n";
            self->stream_ready_ = false;
            break;
            
        case PW_STREAM_STATE_PAUSED:
            LOG(DEBUG, LOG_TAG) << "Stream paused\n";
            break;
            
        default:
            break;
    }
}

void PipeWirePlayer::on_process(void* userdata)
{
    auto* self = static_cast<PipeWirePlayer*>(userdata);
    
    // Use atomic operations for thread-safe access
    if (!self->active_.load(std::memory_order_acquire))
        return;
    
    struct pw_buffer* buffer = pw_stream_dequeue_buffer(self->pw_stream_);
    if (!buffer)
    {
        LOG(ERROR, LOG_TAG) << "Failed to dequeue buffer\n";
        return;
    }
    
    struct spa_buffer* spa_buffer = buffer->buffer;
    struct spa_data* d = &spa_buffer->datas[0];
    
    if (!d->data)
    {
        pw_stream_queue_buffer(self->pw_stream_, buffer);
        return;
    }
    
    uint32_t offset = SPA_MIN(d->chunk->offset, d->maxsize);
    uint32_t stride = self->frame_size_;
    uint32_t n_frames = (d->maxsize - offset) / stride;
    
    void* dst = static_cast<uint8_t*>(d->data) + offset;
    
    if (!self->stream_->getPlayerChunkOrSilence(dst, std::chrono::microseconds(0), n_frames))
    {
        // Check timeout with more sophisticated mechanism
        auto now = chronos::getTickCount();
        if (now - self->last_chunk_tick_ > 5000)
        {
            LOG(INFO, LOG_TAG) << "No chunk received for 5000ms, producing silence\n";
            self->underflows_++;
            
            // Fill with silence instead of disconnecting immediately
            memset(dst, 0, n_frames * stride);
            
            // Consider disconnecting after too many underflows
            if (self->underflows_ > 10)
            {
                LOG(ERROR, LOG_TAG) << "Too many underflows, disconnecting\n";
                pw_stream_set_error(self->pw_stream_, -EPIPE, "No data");
                pw_stream_queue_buffer(self->pw_stream_, buffer);
                return;
            }
        }
        else
        {
            // Still within timeout, just fill with silence
            memset(dst, 0, n_frames * stride);
        }
    }
    else
    {
        self->last_chunk_tick_ = chronos::getTickCount();
        self->underflows_ = 0; // Reset underflow counter on successful data
        self->adjustVolume(static_cast<char*>(dst), n_frames);
    }
    
    // Properly set chunk metadata
    d->chunk->offset = offset;
    d->chunk->stride = stride;
    d->chunk->size = n_frames * stride;
    
    pw_stream_queue_buffer(self->pw_stream_, buffer);
}

void PipeWirePlayer::on_param_changed(void* userdata, uint32_t id, const struct spa_pod* param)
{
    auto* self = static_cast<PipeWirePlayer*>(userdata);
    
    LOG(TRACE, LOG_TAG) << "Stream param changed: " << id << "\n";
    
    if (id != SPA_PARAM_Format || param == NULL)
        return;
    
    struct spa_audio_info_raw info;
    spa_zero(info);
    
    if (spa_format_audio_raw_parse(param, &info) < 0)
        return;
    
    LOG(DEBUG, LOG_TAG) << "Format changed - rate: " << info.rate 
                        << ", channels: " << info.channels << "\n";
    
    std::ignore = self;
}

void PipeWirePlayer::on_io_changed(void* userdata, uint32_t id, void* area, uint32_t size)
{
    auto* self = static_cast<PipeWirePlayer*>(userdata);
    std::ignore = size;
    
    switch (id)
    {
        case SPA_IO_Position:
            self->position_ = static_cast<struct spa_io_position*>(area);
            LOG(TRACE, LOG_TAG) << "Position IO changed\n";
            break;
        default:
            LOG(TRACE, LOG_TAG) << "IO changed: " << id << "\n";
            break;
    }
}

void PipeWirePlayer::on_drained(void* userdata)
{
    auto* self = static_cast<PipeWirePlayer*>(userdata);
    LOG(DEBUG, LOG_TAG) << "Stream drained\n";
    std::ignore = self;
}

} // namespace player
