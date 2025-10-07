#pragma once
#include "esp_err.h"

namespace wetzelmesh {
class MeshTransport {
public:
    esp_err_t init();
    void process();
};
}
