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

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s); // defined in Part E

bool showCurrent(DebugSession& s)
{
    Snapshot* snap = readSnapshotAt(s.reader, (int32_t)s.currentStep);

    if (snap == nullptr)
    {
         cout << "damged .tdbg file" << endl;
        return false;
    }

    printSnapshot((int32_t)s.currentStep + 1, s.reader.header.stepCount, *snap);
    delete snap;

    return true;
}
bool moveBy(DebugSession& s, int32_t delta)
{
    if (!s.active_flag)
    {
        cout << "No sesion yet. Use: start <source file>" << endl;
        return false;
    }

    int64_t last = s.reader.header.stepCount - 1;
    int64_t oldStep = s.currentStep;

    int64_t target = s.currentStep + delta;

    if (target > last)
    {
        if (oldStep == last)
        {
            cout << "Already at the last step." << endl;
            return false;
        }
        cout << "Not enough steps, moving to the last step." << endl;
        target = last;
    }
    else if (target < 0)
    {
        if (oldStep == 0)
        {
            cout << "Already at the first step." << endl;
            return false;
        }
        cout << "Not enough steps, moving to the first step." << endl;
        target = 0;
    }

    s.currentStep = target;
    if (!showCurrent(s))
    {
        s.currentStep = oldStep;
        return false;
    }
    return true;
}
bool cmdStart(DebugSession& s, const string& sourcePath)
{
    string err;
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(), TEMP_TDBG_FILE, err))
    {
        cout << err << endl;
        return false;
    }

    TdbgReader newReader;

    if (!openTdbg(TEMP_TDBG_FILE, newReader))
    {
        cout << "server sent a damaged .tdbg file" << endl;

        remove(TEMP_TDBG_FILE);
        return false;
    }

    closeTdbg(newReader);

    if (s.active_flag)
    {
        closeTdbg(s.reader);
        s.active_flag = 0;
    }
    remove(TDBG_FILE);

    rename(TEMP_TDBG_FILE, TDBG_FILE);

    if (!openTdbg(TDBG_FILE, s.reader))
    {
        cout << "server sent a damaged .tdbg file" << endl;
        return false;
    }
    s.currentStep = 0;

    s.active_flag = 1;

    return showCurrent(s);
}
bool cmdNext(DebugSession& s)
{
    return moveBy(s, 1);
}
bool cmdPrv(DebugSession& s)
{
    return moveBy(s, -1);
}

bool cmdJmpNext(DebugSession& s, int32_t n)
{
    return moveBy(s, n);
}

bool cmdJmpPrv(DebugSession& s, int32_t n) 
{
    return moveBy(s, -n);
}

void printVariables(const Variable vars[], int32_t count)
{
    if (count == 0)
    {
        cout << "(none)";
    }
    for (int i = 0; i < count; i++)
    {
        cout << vars[i].name << " = " << vars[i].value;
        if (i < count - 1)
        {
            cout << ", ";
        }
    }
    cout << endl;
}

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s)
{
    cout << endl;
    cout << "Step " << step << " of " << totalSteps << endl;

    for (int i = 0; i < s.stackDepth; i++)
    {
        const Frame& fr = s.callStack[i];

        cout << "[" << i << "] " << fr.func_name;
        if (i == 0)
        {
            cout << "   <- running now";
        }
        cout << endl;

        cout << "      args:   ";
        printVariables(fr.argv, fr.argc);

        cout << "      locals: ";
        printVariables(fr.locals, fr.localCount);
    }
    cout << endl;
}
void printHelp()
{
    cout << "Commands:" << endl;
    cout << "  start <file>        send a source file to the server and begin debugging" << endl;
    cout << "  next                go one step forward" << endl;
    cout << "  prv                 go one step back" << endl;
    cout << "  jmp next <n>        jump n steps forward" << endl;
    cout << "  jmp prv <n>         jump n steps back" << endl;
    cout << "  help                show this text" << endl;
    cout << "  quit / exit         leave the debugger" << endl;
}

bool parseNumber(const string& w, int32_t& out)
{
    if (w.size() == 0 or w.size() > 9)
    {
        return false;
    }

    int32_t value = 0;

    for (int i = 0; i < w.size(); i++)
    {
        if (w[i] < '0' or w[i] > '9')
        {
            return false;
        }
        value = value * 10 + (w[i] - '0');
    }

    if (value <= 0 or value > MAX_JUMP)
    {
        return false;
    }
    out = value;
    return true;
}

bool runCommand(DebugSession& s, const string& line, bool& quitRequested)
{
    vector<string> words;

    istringstream in(line);
    string w;

    while (in >> w)
    {
        words.push_back(w);
    }

    if (words.size() == 0)
    {
        return true;       
    }

    if (words[0] == PROGRAM_NAME)
    {
        words.erase(words.begin());
    }

    if (words.size() > 0)
    {
        string cmd = words[0];

        if (cmd == "start" && words.size() == 2)
        {
            return cmdStart(s, words[1]);
        }

        if (cmd == "next" && words.size() == 1)
        {
            return cmdNext(s);
        }

        if (cmd == "prv" && words.size() == 1)
        {
            return cmdPrv(s);
        }

        if (cmd == "jmp" && words.size() == 3)
        {
            int32_t n;
            if (parseNumber(words[2], n))
            {
                if (words[1] == "next")
                {
                    return cmdJmpNext(s, n);
                }
                if (words[1] == "prv")
                {
                    return cmdJmpPrv(s, n);
                }
            }
        }
        if (cmd == "help" && words.size() == 1)
        {
            printHelp();
            return true;
        }
        if ((cmd == "quit" or cmd == "exit") && words.size() == 1)
        {
            quitRequested = true;
            return true;
        }
    }

    cout << "unknown command" << endl;

    printHelp();
    return false;
}
int main(int argc, char* argv[])
{
    initNetwork();

    if (argc >= 3)
    {
        LB_HOST = argv[1];
        LB_PORT = atoi(argv[2]);
    }

    DebugSession session;
    initSession(session);

    cout << "Welcome to the Time-Travel Debuger" << endl;
    printHelp();

    bool quitRequested = false;

    while (true)
    {
        if (quitRequested)
        {
            break;
        }

        cout << PROMPT;
        string line;
        if (!getline(cin, line))
        {
            break;              
        }

        runCommand(session, line, quitRequested);
    }

    closeSession(session);
    WSACleanup();
    return 0;
}