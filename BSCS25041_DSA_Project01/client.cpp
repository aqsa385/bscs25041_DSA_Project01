#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <vector>
#include <sstream>
#include "helper.h"
using namespace std;


const char* PROGRAM_NAME = "ttdb";
const char* PROMPT = "ttdb> ";

const char* TDBG_FILE = "ttdb_last.tdbg";
const char* TEMP_TDBG_FILE = "ttdb_new.tdbg";
const int MAX_JUMP = 1000000000;   

string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;

struct TdbgReader
{
    FILE* file = NULL;
    TTDBHeader header;
    vector<int64_t> index;
};

struct DebugSession
{
    int active_flag = 0;     
    TdbgReader reader;   
    int64_t currentStep = 0;
};

bool recvAll(int sock, char* buf, size_t n)
{
    size_t got = 0;
    while (got < n)
    {
        int r = recv(sock, buf + got, (int)(n - got), 0);
        if (r <= 0)
        {
            return false;
        }
        got += r;
    }
    return true;
}

bool sendAll(int sock, const char* buf, size_t n)
{
    size_t sent = 0;
    while (sent < n)
    {
        int r = send(sock, buf + sent, (int)(n - sent), MSG_NOSIGNAL);
        if (r <= 0)
        {
            return false;
        }
        sent += r;
    }
    return true;
}

int connectToHost(const char* host, int port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
    {
        return -1;
    }
    int sock = (int)s;

    sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;

    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        closeSocket(sock);
        return -1;
    }

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        closeSocket(sock);
        return -1;
    }
    return sock;
}
bool uploadSource(int sock, const char* sourcePath)
{
    FILE* f = fopen(sourcePath, "rb");
    if (f == NULL)
    {
        return false;
    }

    char status = 0;

    uint32_t crc = 0;

    crc = crc32Update(crc, &status, 1);

    if (!sendAll(sock, &status, 1))
    {
        fclose(f);
        return false;
    }

    char buf[IO_BUFFER_SIZE];
    while (true)
    {
        size_t n = fread(buf, 1, IO_BUFFER_SIZE, f);
        if (n == 0)
        {
            break;
        }
        crc = crc32Update(crc, buf, n);

        if (!sendAll(sock, buf, n))
        {
            fclose(f);
            return false;
        }
    }
    fclose(f);

    if (!sendAll(sock, (const char*)&crc, 4))
    {
        return false;
    }

    shutdown(sock, SHUT_WR);

    return true;
}
bool receiveResponse(int sock, const char* outTdbgPath, string& outError)
{
    TrailerChecker tc;

    char status;
    if (!recvAll(sock, &status, 1))
    {
        outError = "no reply received (the server may have been busy or timed out)";
        return false;
    }
    tc.addStatus(status);

    FILE* f = NULL;
    string errorText = "";

    if (status == 0)
    {
        f = fopen(outTdbgPath, "wb");
        if (f == NULL)
        {
            outError = "cannot create the .tdbg file";
            return false;
        }
    }

    char buf[IO_BUFFER_SIZE];
    while (true)
    {
        int got = recv(sock, buf, IO_BUFFER_SIZE, 0);
        if (got == 0)
        {
            break;
        }
        if (got < 0)
        {
            if (f != NULL)
            {
                fclose(f);
                remove(outTdbgPath);
            }
            outError = "connection lost while receiving the reply";
            return false;
        }

        string body = tc.feed(buf, got);
        if (body.size() > 0)
        {
            if (status == 0)
            {
                fwrite(body.c_str(), 1, body.size(), f);
            }
            else
            {
                errorText += body;
            }
        }
    }

    if (f != NULL)
    {
        fclose(f);
    }

    if (!tc.verify())
    {
        if (status == 0)
        {
            remove(outTdbgPath);
        }
        outError = "checksum mismatch, response corupted";
        return false;
    }

    if (status == 1)
    {
        outError = errorText;
        return false;
    }
    return true;
}

bool requestDebugFile(const char* host, int port, const char* sourcePath, const char* outPath, string& outError)
{
    FILE* test = fopen(sourcePath, "rb");

    if (test == NULL)
    {
        outError = string("cannot open ") + sourcePath;

        return false;
    }

    fclose(test);

    int sock = connectToHost(host, port);
    if (sock == -1)
    {
        outError = "cannot connect to the load balancer";
        return false;
    }

    uploadSource(sock, sourcePath);
    bool ok = receiveResponse(sock, outPath, outError);

    closeSocket(sock);
    return ok;
}

bool readInt(FILE* f, int32_t& x)
{
    return fread(&x, sizeof(int32_t), 1, f) == 1;
}

bool readString(FILE* f, string& s)
{
    int32_t len;
    if (!readInt(f, len))
    {
        return false;
    }

    if (len < 0 or len > 10000)
    {
        return false;
    }

    s.resize(len);
    if (len > 0)
    {
        if (fread(&s[0], 1, len, f) != (size_t)len)
        {
            return false;
        }
    }
    return true;
}

bool readVariable(FILE* f, Variable& v)
{
    if (!readString(f, v.name))
    {
        return false;
    }
    return readInt(f, v.value);
}

bool readFrame(FILE* f, Frame& fr)
{
    if (!readString(f, fr.func_name))
    {
        return false;
    }

    if (!readInt(f, fr.argc))
    {
        return false;
    }

    if (fr.argc < 0 or fr.argc > MAX_VARS_PER_FRAME)
    {
        return false;
    }

    for (int i = 0; i < fr.argc; i++)
    {
        if (!readVariable(f, fr.argv[i]))
        {
            return false;
        }
    }

    if (!readInt(f, fr.returnLine))
    {
        return false;
    }

    if (!readInt(f, fr.localCount))
    {
        return false;
    }

    if (fr.localCount < 0 or fr.localCount > MAX_VARS_PER_FRAME)
    {
        return false;
    }
    for (int i = 0; i < fr.localCount; i++)
    {
        if (!readVariable(f, fr.locals[i]))
        {
            return false;
        }
    }
    return true;
}


bool openTdbg(const char* path, TdbgReader& r)
{
    r.file = fopen(path, "rb");
    if (r.file == NULL)
    {
        return false;
    }

    bool ok = true;
    if (fread(r.header.magic, 1, 4, r.file) != 4)
    {
        ok = false;
    }

    if (ok && fread(&r.header.version, sizeof(int32_t), 1, r.file) != 1) 
    {
        ok = false;
    }

    if (ok && fread(&r.header.stepCount, sizeof(int32_t), 1, r.file) != 1) 
    {
        ok = false;
    }

    if (ok && fread(&r.header.indexOffset, sizeof(int64_t), 1, r.file) != 1) 
    {
        ok = false;
    }

    if (ok && memcmp(r.header.magic, "TTDB", 4) != 0) 
    {
        ok = false;
    }

    if (ok && r.header.stepCount <= 0) 
    {
        ok = false;
    }

    if (ok && r.header.indexOffset < 20) 
    {
        ok = false;
    }

    if (ok)
    {
        fseek(r.file, 0, SEEK_END);
        int64_t fileSize = ftell(r.file);

        if (r.header.indexOffset + (int64_t)r.header.stepCount * 8 > fileSize)
        {
            ok = false;
        }
    }

    if (ok)
    {
        fseek(r.file, (long)r.header.indexOffset, SEEK_SET);

        r.index.resize(r.header.stepCount);

        for (int i = 0; i < r.header.stepCount; i++)
        {
            if (fread(&r.index[i], sizeof(int64_t), 1, r.file) != 1)
            {
                ok = false;
                break;
            }
            if (r.index[i] < 20 or r.index[i] >= r.header.indexOffset)
            {
                ok = false;
                break;
            }
        }
    }

    if (!ok)
    {
        closeTdbg(r);
        return false;
    }
    return true;
}
void closeTdbg(TdbgReader& r)
{
    r.index.clear();

    if (r.file != NULL)
    {
        fclose(r.file);
        r.file = NULL;
    }
}
Snapshot* readSnapshotAt(TdbgReader& r, int32_t step)
{
    if (r.file == NULL or step < 0 or step >= r.header.stepCount)
    {
        return nullptr;
    }

    fseek(r.file, (long)r.index[step], SEEK_SET);

    Snapshot* s = new Snapshot;

    if (!readInt(r.file, s->stackDepth) or s->stackDepth < 0 or s->stackDepth > MAX_STACK_DEPTH)
    {
        delete s;
        return nullptr;
    }

    for (int i = 0; i < s->stackDepth; i++)
    {
        if (!readFrame(r.file, s->callStack[i]))
        {
            delete s;
            return nullptr;
        }
    }
    return s;
}

void initSession(DebugSession& s)
{
    s.active_flag = 0;

    s.currentStep = 0;

    s.reader.file = NULL;
}
void closeSession(DebugSession& s)
{
    if (s.active_flag)
    {
        closeTdbg(s.reader);
    }
    s.active_flag = 0;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s); // defined in Part E

bool showCurrent(DebugSession& s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
}
bool moveBy(DebugSession& s, int32_t delta)
{
}
bool cmdStart(DebugSession& s, const string& sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
}
bool cmdNext(DebugSession& s) {}
bool cmdPrv(DebugSession& s) {}
bool cmdJmpNext(DebugSession& s, int32_t n) {}
bool cmdJmpPrv(DebugSession& s, int32_t n) {}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s)
{
}
void printHelp()
{
    // Print the commands
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession& s, const string& line, bool& quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
}
int main(int argc, char* argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.

}