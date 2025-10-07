#pragma once
#include "esp_err.h"

namespace monimesh {
class MeshTransport {
public:
    esp_err_t init();
    void process();
};
}
