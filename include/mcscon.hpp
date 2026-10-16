// Thin C++ wrapper over mcscon.h: RAII + a std::function chat callback.
#pragma once
#include "mcscon.h"
#include <functional>
#include <stdexcept>
#include <string>

namespace mcscon {
class Bot {
public:
    explicit Bot(int version = MC_V1_12_2) : bot_(mc_bot_new(version)) {
        if (!bot_) throw std::runtime_error("unknown version");
    }
    ~Bot() { mc_bot_free(bot_); }
    Bot(const Bot&) = delete;
    Bot& operator=(const Bot&) = delete;

    void onChat(std::function<void(const char* text)> fn) {
        chat_ = std::move(fn);
        mc_bot_on_chat(bot_, [](void* self, const char* text, int) { static_cast<Bot*>(self)->chat_(text); }, this);
    }
    bool connect(const std::string& host, uint16_t port, const std::string& name) { return mc_bot_connect(bot_, host.c_str(), port, name.c_str()); }
    bool poll() { return mc_bot_poll(bot_) >= 0; }   // false once disconnected
    bool spawned() const { return mc_bot_spawned(bot_); }
    float health() const { return mc_bot_health(bot_); }
    void position(double xyz[3]) const { mc_bot_position(bot_, xyz); }
    bool say(const std::string& message) { return mc_bot_say(bot_, message.c_str()); }
    void walk(bool forward) { mc_bot_walk(bot_, forward); }
    void disconnect() { mc_bot_disconnect(bot_); }

private:
    McBot* bot_;
    std::function<void(const char*)> chat_;
};
}  // namespace mcscon
