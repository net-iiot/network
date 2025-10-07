#pragma once
#include <string>
#include "cJSON.h"

namespace wetzelmesh
{

    class JSONCodec
    {
    public:
        static std::string encode(const char *type, const char *payload);
        static bool decode(const std::string &json, std::string &type, std::string &payload);
    };

}
