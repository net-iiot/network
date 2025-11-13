#pragma once
#include "protocol.hpp"
#include <stdint.h>

namespace WetzelMesh {
void start_test_generation();
void on_token_received(const Protocol::Packet &token_pkt);
uint32_t get_token_hold_time_ms();
}
