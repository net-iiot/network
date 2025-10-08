#pragma once
#include <stdint.h>

namespace WetzelMesh
{
    class LedManager
    {
    public:
        static void init(bool isGateway);
        static void blink();
    };
}
