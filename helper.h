#pragma once

#include <string>
#include <cstdint>
#include <cstddef>

struct FuncEntry
{
    std::string funcName;
    int64_t byteOffsetInResolveBin;
};

struct PendingPatch
{
    int64_t byteOffsetOfOffsetField;
    std::string targetFuncName;
};

inline uint32_t update_crc32(uint32_t crc, const void* buf, size_t len)
{
    const uint8_t* byteBuf = static_cast<const uint8_t*>(buf);
    crc = ~crc;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= byteBuf[i];
        for (int k = 0; k < 8; ++k)
        {
            crc = (crc >> 1) ^ (0xEDB88320U & ((crc & 1) ? 0xFFFFFFFFU : 0U));
        }
    }
    return ~crc;
}