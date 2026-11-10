#pragma once

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <csignal>
#include <ctime>
#pragma comment(lib, "ws2_32.lib")     

#define MSG_NOSIGNAL 0                  
#define SHUT_WR SD_SEND                 
typedef SSIZE_T ssize_t;

inline void initNetwork()             
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}
inline void closeSocket(int sock)      
{
    closesocket(sock);
}
inline void sleepMs(int ms)             
{
    Sleep(ms);
}
inline void makeNotInheritable(int sock)   
{
    SetHandleInformation((HANDLE)(SOCKET)sock, HANDLE_FLAG_INHERIT, 0);
}

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

class TrailerChecker
{
    string held;
    uint32_t crc;

public:
    TrailerChecker()
    {
        crc = 0;
    }

    void addStatus(char status)
    {
        crc = crc32Update(crc, &status, 1);
    }

    string feed(const char* buf, size_t n)
    {
        held.append(buf, n);

        string released = "";
        if (held.size() > 4)
        {
            size_t extra = held.size() - 4;

            released = held.substr(0, extra);
            held.erase(0, extra);

            crc = crc32Update(crc, released.c_str(), released.size());
        }
        return released;
    }

    bool verify()
    {
        if (held.size() != 4)
        {
            return false;
        }
        uint32_t received;

        memcpy(&received, held.c_str(), 4);

        return received == crc;
    }

    uint32_t currentCrc()
    {
        return crc;
    }
};