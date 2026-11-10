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
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

bool openTdbg(const char* path, TdbgReader& r)
{
}
void closeTdbg(TdbgReader& r)
{
    // Free r.index and close the file.
}
Snapshot* readSnapshotAt(TdbgReader& r, int32_t step)
{
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession& s)
{

}
void closeSession(DebugSession& s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
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