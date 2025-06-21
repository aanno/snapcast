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

using namespace std::chrono_literals;
using namespace std;

namespace player
{

static constexpr std::chrono::milliseconds BUFFER_TIME = 100ms;
static constexpr auto LOG_TAG = "PipeWirePlayer";

// Global device list for enumeration
static std::vector<PcmDevice> g_devices;

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
    
    struct spa_hook registry_hook;
    pw_registry_add_listener(registry, &registry_hook, &registry_events_, nullptr);
    
    // Run the main loop for a short time to enumerate devices
    auto start_time = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_time < 2s)
    {
        pw_main_loop_run(main_loop);
        std::this_thread::sleep_for(10ms);
    }
    
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
    : Player(io_context, settings, std::move(stream)), latency_(BUFFER_TIME), last_chunk_tick_(0),
      main_loop_(nullptr), context_(nullptr), core_(nullptr), stream_(nullptr), registry_(nullptr),
      stream_ready_(false), target_node_(std::nullopt), node_id_(0), frame_size_(0)
{
    auto params = utils::string::split_pairs_to_container<std::vector<std::string>>(settings.parameter, ',', '=');
    
    if (params.find("buffer_time") != params.end())
        latency_ = std::chrono::milliseconds(std::max(cpt::stoi(params["buffer_time"].front()), 10));
    
    if (params.find("target") != params.end())
        target_node_ = params["target"].front();
    
    properties_[PW_KEY_MEDIA_TYPE] = "Audio";
    properties_[PW_KEY_MEDIA_CATEGORY] = "Playback";
    properties_[PW_KEY_MEDIA_ROLE] = "Music";
    properties_[PW_KEY_APP_NAME] = "Snapcast";
    properties_[PW_KEY_APP_ID] = "snapcast";
    properties_[PW_KEY_APP_ICON_NAME] = "snapcast";
    
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
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, target_node_.value().c_str());
    else if (settings_.pcm_device.name != DEFAULT_DEVICE)
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, settings_.pcm_device.name.c_str());
    
    // Create playback stream
    stream_ = pw_stream_new(core_, "Snapcast Playback", props);
    if (!stream_)
        throw SnapException("Failed to create PipeWire stream");
    
    // Add stream listener
    pw_stream_add_listener(stream_, &stream_listener_, &stream_events_, this);
    
    // Create audio format parameters
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    
    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audio_info_);
    
    // Connect stream
    if (pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                         static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
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
    
    this->disconnect();
    Player::stop();
}

void PipeWirePlayer::disconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    LOG(INFO, LOG_TAG) << "Disconnecting from PipeWire\n";
    
    stream_ready_ = false;
    
    if (main_loop_)
        pw_main_loop_quit(main_loop_, 0);
    
    if (stream_)
    {
        spa_hook_remove(&stream_listener_);
        pw_stream_destroy(stream_);
        stream_ = nullptr;
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
    // PipeWire volume control would be implemented here
    // This is more complex than PulseAudio and requires separate implementation
    std::ignore = volume;
    LOG(DEBUG, LOG_TAG) << "Hardware volume control not yet implemented for PipeWire\n";
}

bool PipeWirePlayer::getHardwareVolume(Volume& volume)
{
    std::ignore = volume;
    return false;
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
            break;
        case PW_STREAM_STATE_ERROR:
            LOG(ERROR, LOG_TAG) << "Stream error: " << (error ? error : "unknown") << "\n";
            self->stream_ready_ = false;
            break;
        default:
            break;
    }
}

void PipeWirePlayer::on_process(void* userdata)
{
    auto* self = static_cast<PipeWirePlayer*>(userdata);
    
    struct pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream_);
    if (!buffer)
    {
        LOG(ERROR, LOG_TAG) << "Failed to dequeue buffer\n";
        return;
    }
    
    struct spa_buffer* spa_buffer = buffer->buffer;
    void* data = spa_buffer->datas[0].data;
    if (!data)
    {
        pw_stream_queue_buffer(self->stream_, buffer);
        return;
    }
    
    uint32_t stride = spa_buffer->datas[0].maxsize;
    uint32_t num_frames = stride / self->frame_size_;
    
    if (self->buffer_.size() < stride)
        self->buffer_.resize(stride);
    
    if (!self->stream_->getPlayerChunkOrSilence(self->buffer_.data(), std::chrono::microseconds(0), num_frames))
    {
        // if we haven't got a chunk for a while, it's time to disconnect
        if (chronos::getTickCount() - self->last_chunk_tick_ > 5000)
        {
            LOG(INFO, LOG_TAG) << "No chunk received for 5000ms, disconnecting from PipeWire.\n";
            self->disconnect();
            return;
        }
    }
    else
    {
        self->last_chunk_tick_ = chronos::getTickCount();
        self->adjustVolume(static_cast<char*>(self->buffer_.data()), num_frames);
    }
    
    memcpy(data, self->buffer_.data(), stride);
    spa_buffer->datas[0].chunk->offset = 0;
    spa_buffer->datas[0].chunk->stride = self->frame_size_;
    spa_buffer->datas[0].chunk->size = stride;
    
    pw_stream_queue_buffer(self->stream_, buffer);
}

void PipeWirePlayer::on_param_changed(void* userdata, uint32_t id, const struct spa_pod* param)
{
    std::ignore = userdata;
    std::ignore = id;
    std::ignore = param;
    
    LOG(TRACE, LOG_TAG) << "Stream param changed\n";
}

void PipeWirePlayer::on_io_changed(void* userdata, uint32_t id, void* area, uint32_t size)
{
    std::ignore = userdata;
    std::ignore = id;
    std::ignore = area;
    std::ignore = size;
    
    LOG(TRACE, LOG_TAG) << "Stream IO changed\n";
}

void PipeWirePlayer::on_drained(void* userdata)
{
    auto* self = static_cast<PipeWirePlayer*>(userdata);
    LOG(DEBUG, LOG_TAG) << "Stream drained\n";
    std::ignore = self;
}

} // namespace player
