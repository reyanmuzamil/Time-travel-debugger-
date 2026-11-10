#pragma once


// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files. 

struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};
inline uint32_t update_crc32(uint32_t crc, const char* buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= static_cast<uint8_t>(buf[i]);
        for (int k = 0; k < 8; ++k)
        {
            crc = (crc >> 1) ^ (0xEDB88320U & (-(crc & 1)));
        }
    }
    return ~crc;
}
