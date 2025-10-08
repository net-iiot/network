#pragma once
#include <string>

namespace WetzelMesh
{
    // Mantido apenas se você ainda usa em algum lugar. Hoje o Protocol cuida do JSON.
    class JSONCodec
    {
    public:
        static std::string wrap(const std::string &type, const std::string &payload);
        static bool unwrap(const std::string &json, std::string &type, std::string &payload);
    };
} // namespace WetzelMesh
