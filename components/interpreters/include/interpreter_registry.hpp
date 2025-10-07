#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <string>

namespace wetzelmesh {
struct Message {
    uint16_t map_id;
    std::string payload;
};

class InterpreterRegistry {
public:
    using Handler = std::function<void(const Message&)>;

    static InterpreterRegistry& instance();
    void registerHandler(uint16_t map_id, Handler handler);
    void handle(const Message& msg);

private:
    std::unordered_map<uint16_t, Handler> handlers;
};
}
