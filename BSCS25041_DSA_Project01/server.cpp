#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <cstdint>
#include <cstdio>
using namespace std;

const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; 
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; 
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  
const int32_t SOCKET_TIMEOUT_SEC = 5;                      

template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    Stack()
    { 
        top = nullptr;
        count = 0;
    }
    void push(const T& val)
    {
        if (count >= MAX_STACK_DEPTH)
        {
            cout << "Stack overflow: max depth reached" << endl;
            return;
        }
        Node* n = new Node;
        n->data = val;
        n->next = top;   
        top = n;         
        count++;      
    }
    T pop()
    {
        if (top == nullptr)
        {
            throw runtime_error("pop on empty stack");
        }
        Node* temp = top;
        T val = temp->data;   
        top = top->next; 

        delete temp;
        count--;
        return val;
    }
    T& peek()
    {
        if (top == nullptr)
        {
            throw runtime_error("peek on empty stack");
        }
        return top->data;
    }
    bool isEmpty()
    {
        return top == nullptr;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        int32_t i = 0;
        Node* cur = top;
        while (cur != nullptr && i < maxLen)
        {
            out[i] = cur->data;
            cur = cur->next;
            i++;
        }
        return i;
    }

    ~Stack() {
        while (top != nullptr)
        {
            Node* temp = top;
            top = top->next;
            delete temp;
        }
    }
};

struct Snapshot; 
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};

class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    Timeline()
    {
        head = tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot* s)
    {
        TimelineNode* n = new TimelineNode;

        n->data = s;
        n->next = nullptr;
        n->prev = tail;       

        if (tail == nullptr)  
        {
            head = n;
        }
        else
        {
            tail->next = n;   
        }

        tail = n;             
        stepCount++;
    }
    TimelineNode* begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

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
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

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


bool readSourceLine(ifstream& in, string& out)
{
    string line;
    while (getline(in, line))
    {

        if (line.size() > 0 && line[line.size() - 1] == '\r')
        {
            line.pop_back();
        }

        bool blank = true;
        for (int i = 0; i < line.size(); i++)
        {
            if (line[i] != ' ' && line[i] != '\t')
            {
                blank = false;   
                break;
            }
        }

        if (blank == false)
        {
            out = line;
            return true;
        }

    }
    return false;
    
}
string getWord(const string& line, int n)   
{
    int i = 0;
    int len = line.size();
    int count = 0;

    while (i < len)
    {

        while (i < len && (line[i] == ' ' or line[i] == '\t'))
        {
            i++;
        }

        string word = "";
        while (i < len && line[i] != ' ' && line[i] != '\t')
        {
            word += line[i];
            i++;
        }

        count++;
        if (count == n)
        {
            return word;
        }
    }
    return "";   
}

string firstWord(const string& line)
{
    return getWord(line, 1);
}

string secondWord(const string& line)
{
    return getWord(line, 2);
}

bool validateProgram(const char* sourcePath)
{
    ifstream read(sourcePath);
    if (!read.is_open())
    {
        cout << "Error: cannot open " << sourcePath << endl;
        return false;
    }

    bool inFunc = false;       
    string line;
    int32_t lineNo = 0;

    while (readSourceLine(read, line))
    {
        lineNo++;
        string kw = firstWord(line);

        if (kw == "func")
        {
            if (inFunc)
            {
                cout << "Error (instruction " << lineNo << "): nested func or missing func_end" << endl;
                return false;
            }
            inFunc = true;
        }
        else if (kw == "func_end")
        {
            if (!inFunc)
            {
                cout << "Error (instruction " << lineNo << "): func_end without a matching func" << endl;
                return false;
            }
            inFunc = false;
        }
    }

    if (inFunc)
    {
        cout << "Error: function was never closed with func_end" << endl;
        return false;
    }
    return true;
}

int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    int64_t startPos = ftell(f);             

    int32_t size = (int32_t)text.size();     
    fwrite(&offsetField, sizeof(int64_t), 1, f); 

    fwrite(&size, sizeof(int32_t), 1, f); 

    fwrite(text.c_str(), 1, size, f);              

    return startPos;
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    int64_t offsetField;
    int32_t size;

    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1)
    {
        return -1;
    }
    fread(&size, sizeof(int32_t), 1, f);

    outText.resize(size);                    
    if (size > 0)
    {
        fread(&outText[0], 1, size, f);
    }

    return offsetField;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    
    ifstream in(sourcePath);
    if (!in.is_open())
    {
        cout << "Error: cannot open " << sourcePath << endl;
        return -1;
    }

    FILE* f = fopen(resolveBinPath, "wb+");   

    if (f == NULL)
    {
        cout << "Error: cannot create " << resolveBinPath << endl;
        return -1;
    }


    string line;

    while (readSourceLine(in, line))
    {
        string kw = firstWord(line);

        int64_t pos = ftell(f);              

        if (kw == "call")
        {
            writeResolveRecord(f, 0, line);   

            if (patchCount >= MAX_PATCHES)
            {
                cout << "Error: too many call instructions" << endl;
                fclose(f);

                return -1;
            }
            patches[patchCount].byteOffsetOfOffsetField = pos;

            patches[patchCount].targetFuncName = secondWord(line);
            patchCount++;
        }
        else
        {
            writeResolveRecord(f, pos, line);

            if (kw == "func")
            {
                if (funcCount >= MAX_FUNCS)
                {
                    cout << "Error: too many functions" << endl;
                    fclose(f);
                    return -1;
                }
                funcArray[funcCount].funcName = secondWord(line);

                funcArray[funcCount].byteOffsetInResolveBin = pos;

                funcCount++;
            }
        }
    }

    for (int i = 0; i < patchCount; i++)
    {
        int64_t target = -1;

        for (int j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == patches[i].targetFuncName)
            {
                target = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }

        if (target == -1)
        {
            cout << "Error: call to undefined function " << patches[i].targetFuncName << endl;

            fclose(f);
            return -1;
        }

        fseek(f, (long)patches[i].byteOffsetOfOffsetField, SEEK_SET);

        fwrite(&target, sizeof(int64_t), 1, f);
    }

    fclose(f);

    for (int i = 0; i < funcCount; i++)
    {
        if (funcArray[i].funcName == "main")
        {
            return funcArray[i].byteOffsetInResolveBin;
        }
    }

    cout << "Error: no main function\n";
    return -1;
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}