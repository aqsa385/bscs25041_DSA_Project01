#pragma once

#include <string>
#include <cstdint>
#include <cstring>
using namespace std;

const int IO_BUFFER_SIZE = 64 * 1024; 

const int MAX_VARS_PER_FRAME = 16;

const int MAX_STACK_DEPTH = 64;

struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};

struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};

struct TTDBHeader
{
    char magic[4]; 
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; 
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; 
    string targetFuncName;
};

inline uint32_t crc32Update(uint32_t crc, const char* data, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
    {
        crc ^= (unsigned char)data[i];

        for (int b = 0; b < 8; b++)
        {
            if (crc & 1)
            {
                crc = (crc >> 1) ^ 0xEDB88320;
            }
            else
            {
                crc = crc >> 1;
            }
        }
    }
    return ~crc;
}
