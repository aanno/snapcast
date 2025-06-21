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

#pragma once

// local headers
#include "player.hpp"

// 3rd party headers
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

// standard headers
#include <atomic>
#include <cstdio>
#include <memory>
#include <optional>

namespace player
{

static constexpr auto PIPEWIRE = "pipewire";

/// PipeWire Player
/// Audio player implementation using PipeWire's native API
class PipeWirePlayer : public Player
{
public:
    /// c'tor
    PipeWirePlayer(boost::asio::io_context& io_context, const ClientSettings::Player& settings, std::shared_ptr<Stream> stream);
    
    /// d'tor
    virtual ~PipeWirePlayer();

    void start() override;
    void stop() override;

    /// List the system's audio output devices
    static std::vector<PcmDevice> pcm_list(const std::string& parameter);

private:
    bool needsThread() const override;
    void worker() override;
    void connect();
    void disconnect();
    bool getHardwareVolume(Volume& volume) override;
    void setHardwareVolume(const Volume& volume) override;
    
    // PipeWire callbacks
    static void on_process(void* userdata);
    static void on_state_changed(void* userdata, enum pw_stream_state old, enum pw_stream_state state, const char* error);
    static void on_param_changed(void* userdata, uint32_t id, const struct spa_pod* param);
    static void on_io_changed(void* userdata, uint32_t id, void* area, uint32_t size);
    static void on_drained(void* userdata);
    
    // Registry callbacks for device enumeration
    static void registry_event_global(void* data, uint32_t id, uint32_t permissions, const char* type, uint32_t version, const struct spa_dict* props);
    static void registry_event_global_remove(void* data, uint32_t id);

    std::vector<char> buffer_;
    std::chrono::microseconds latency_;
    int underflows_ = 0;
    std::atomic<bool> stream_ready_;
    long last_chunk_tick_;
    
    struct pw_main_loop* main_loop_;
    struct pw_context* context_;
    struct pw_core* core_;
    struct pw_stream* stream_;
    struct pw_registry* registry_;
    
    struct spa_hook stream_listener_;
    struct spa_hook registry_listener_;
    
    std::optional<std::string> target_node_;
    std::map<std::string, std::string> properties_;
    
    // Volume control
    std::chrono::time_point<std::chrono::steady_clock> last_change_;
    uint32_t node_id_;
    
    // Stream parameters
    struct spa_audio_info_raw audio_info_;
    uint32_t frame_size_;
    
    static inline const struct pw_stream_events stream_events_ = {
        .version = PW_VERSION_STREAM_EVENTS,
        .destroy = nullptr,
        .state_changed = on_state_changed,
        .control_info = nullptr,
        .io_changed = on_io_changed,
        .param_changed = on_param_changed,
        .add_buffer = nullptr,
        .remove_buffer = nullptr,
        .process = on_process,
        .drained = on_drained,
    };
    
    static inline const struct pw_registry_events registry_events_ = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = registry_event_global,
        .global_remove = registry_event_global_remove,
    };
};

} // namespace player
