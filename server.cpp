// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <cstdio>
#include <sstream>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
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
    // Implement these functions:
    Stack()
    { // initialize the stack
        top = nullptr;
        count = 0;
    }
    void push(const T& val)
    {
        if (count >= MAX_STACK_DEPTH)
        {
            return;
        }
        Node* newNode = new Node;
        newNode->data = val;
        newNode->next = top;
        top = newNode;
        count++;
        // pushes the value on the stack if max limit is not reached yet.
    }
    T pop()
    {
        if (isEmpty())
        {
            return T();
        }
        Node* temp = top;
        T val = temp->data;
        top = top->next;
        delete temp;
        count--;
        return val;
        // pop the top value on the stack
    }
    T& peek()
    {
        return top->data;
        // returns the top value on the stack
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        Node* curr = top;
        int32_t idx = 0;
        while (curr != nullptr && idx < maxLen)
        {
            out[idx] = curr->data;
            idx++;
            curr = curr->next;
        }
        return idx;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
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
    // Implement these functions
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot* s)
    {
        // add record in the timeline
        TimelineNode* newNode = new TimelineNode;
        newNode->data = s;
        newNode->next = nullptr;
        newNode->prev = tail;
        if (tail == nullptr)
        {
            head = newNode;
            tail = newNode;
        }
        else
        {
            tail->next = newNode;
            tail = newNode;
        }
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

// Core structs
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
    // placeholder for other two data members
}

// resolve.bin - bookkeeping
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



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    string line;
    while (getline(in, line))
    {
        bool isBlank = true;
        for (int i = 0; i < (int)line.length(); i++)
        {
            if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
            {
                isBlank = false;
                break;
            }
        }
        if (!isBlank)
        {
            out = line;
            return true;
        }
    }
    return false;
    // reads the next nonblank line
}
string firstWord(string line)
{
    stringstream ss(line);
    string word = "";
    ss >> word;
    return word;
    // returns first word from the input string
}
string secondWord(string line)
{
    stringstream ss(line);
    string word1 = "";
    string word2 = "";
    ss >> word1 >> word2;
    return word2;
    // returns the second word
}
bool validateProgram(const char* sourcePath)
{
    ifstream in(sourcePath);
    if (!in.is_open())
    {
        return false;
    }
    bool insideFunc = false;
    string line;
    while (readSourceLine(in, line))
    {
        string keyWord = firstWord(line);
        if (keyWord == "func")
        {
            if (insideFunc)
            {
                in.close();
                return false;
            }
            insideFunc = true;
        }
        else if (keyWord == "func_end")
        {
            if (!insideFunc)
            {
                in.close();
                return false;
            }
            insideFunc = false;
        }
        else
        {
            if (!insideFunc)
            {
                in.close();
                return false;
            }
        }
    }
    in.close();
    if (insideFunc)
    {
        return false;
    }
    return true;
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    int64_t startPos = _ftelli64(f);
    int32_t strSize = (int32_t)text.length();

    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&strSize, sizeof(int32_t), 1, f);
    if (strSize > 0)
    {
        fwrite(text.c_str(), sizeof(char), strSize, f);
    }
    return startPos;
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
    int64_t offsetField = 0;
    int32_t strSize = 0;

    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1)
    {
        return -1;
    }
    if (fread(&strSize, sizeof(int32_t), 1, f) != 1)
    {
        return -1;
    }

    outText = "";
    for (int i = 0; i < strSize; i++)
    {
        char ch;
        fread(&ch, sizeof(char), 1, f);
        outText += ch;
    }
    return offsetField;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
    ifstream in(sourcePath);
    if (!in.is_open())
    {
        return -1;
    }
    FILE* fOut = fopen(resolveBinPath, "wb+");
    if (!fOut)
    {
        in.close();
        return -1;
    }
    string line;
    while (readSourceLine(in, line))
    {
        string keyWord = firstWord(line);
        int64_t pos = _ftelli64(fOut);

        if (keyWord == "func")
        {
            string fName = secondWord(line);
            if (funcCount < MAX_FUNCS)
            {
                funcArray[funcCount].funcName = fName;
                funcArray[funcCount].byteOffsetInResolveBin = pos;
                funcCount++;
            }
            writeResolveRecord(fOut, pos, line);
        }
        else if (keyWord == "call")
        {
            string targetFunc = secondWord(line);
            if (patchCount < MAX_PATCHES)
            {
                patches[patchCount].byteOffsetOfOffsetField = pos;
                patches[patchCount].targetFuncName = targetFunc;
                patchCount++;
            }
            writeResolveRecord(fOut, 0, line);
        }
        else
        {
            writeResolveRecord(fOut, pos, line);
        }
    }
    in.close();
    for (int i = 0; i < patchCount; i++)
    {
        int64_t targetOff = -1;
        for (int j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == patches[i].targetFuncName)
            {
                targetOff = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }
        if (targetOff == -1)
        {
            fclose(fOut);
            return -1;
        }
        _fseeki64(fOut, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&targetOff, sizeof(int64_t), 1, fOut);
    }
    fclose(fOut);
    int64_t mainOffset = -1;
    for (int i = 0; i < funcCount; i++)
    {
        if (funcArray[i].funcName == "main")
        {
            mainOffset = funcArray[i].byteOffsetInResolveBin;
            break;
        }
    }
    return mainOffset;
}

bool isInteger(string s)
{
    if (s == "")
    {
        return false;
    }
    int i = 0;
    if (s[0] == '-' || s[0] == '+')
    {
        if (s.length() == 1)
        {
            return false;
        }
        i = 1;
    }
    while (i < (int)s.length())
    {
        if (s[i] < '0' || s[i] > '9')
        {
            return false;
        }
        i++;
    }
    return true;
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
    stringstream ss(line);
    string word;
    int32_t tokenCount = 0;
    while (ss >> word && tokenCount < maxTokens)
    {
        if (tokenCount == 0)
        {
            tokens[tokenCount].type = KEYWORD;
        }
        else if (tokenCount == 1)
        {
            tokens[tokenCount].type = IDENTIFIER;
        }
        else
        {
            tokens[tokenCount].type = PARAM;
        }
        tokens[tokenCount].text = word;
        tokenCount++;
    }
    return tokenCount;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    // build the snapshot based on the callStack given
    Snapshot* s = new Snapshot();
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
}
int32_t getVariableValue(const Frame& frame, string name)
{
    for (int i = 0; i < frame.argc; i++)
    {
        if (frame.argv[i].name == name)
        {
            return frame.argv[i].value;
        }
    }
    for (int i = 0; i < frame.localCount; i++)
    {
        if (frame.locals[i].name == name)
        {
            return frame.locals[i].value;
        }
    }
    return 0;
}
void setVariableValue(Frame& frame, string name, int32_t val)
{
    for (int i = 0; i < frame.argc; i++)
    {
        if (frame.argv[i].name == name)
        {
            frame.argv[i].value = val;
            return;
        }
    }
    for (int i = 0; i < frame.localCount; i++)
    {
        if (frame.locals[i].name == name)
        {
            frame.locals[i].value = val;
            return;
        }
    }
    if (frame.localCount < MAX_VARS_PER_FRAME)
    {
        frame.locals[frame.localCount].name = name;
        frame.locals[frame.localCount].value = val;
        frame.localCount++;
    }
}
int32_t resolveOperand(const Frame& frame, string tokenText)
{
    if (isInteger(tokenText))
    {
        return stoi(tokenText);
    }
    return getVariableValue(frame, tokenText);
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
    FILE* f = fopen(resolveBinPath, "rb");
    if (!f)
    {
        return;
    }
    Stack<Frame> callStack;

    fseek(f, (long)mainOffset, SEEK_SET);
    string rawLine;
    readResolveRecord(f, rawLine);

    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.localCount = 0;
    mainFrame.returnLine = -1;
    callStack.push(mainFrame);
    timeline.record(buildSnapshot(callStack));
    Token tokens[MAX_TOKENS];

    while (!callStack.isEmpty())
    {
        int64_t recOffset = readResolveRecord(f, rawLine);
        if (recOffset == -1)
        {
            break;
        }
        int32_t count = tokenizeLine(rawLine, tokens, MAX_TOKENS);
        if (count == 0)
        {
            continue;
        }
        string inst = tokens[0].text;
        if (inst == "set")
        {
            string varName = tokens[1].text;
            int32_t val = resolveOperand(callStack.peek(), tokens[2].text);
            setVariableValue(callStack.peek(), varName, val);
            timeline.record(buildSnapshot(callStack));
        }
        else if (inst == "add" || inst == "sub" || inst == "mul" || inst == "div")
        {
            string targetVar = tokens[1].text;
            int32_t val1 = resolveOperand(callStack.peek(), tokens[1].text);
            int32_t val2 = resolveOperand(callStack.peek(), tokens[2].text);
            int32_t result = 0;
            if (inst == "add")
            {
                result = val1 + val2;
            }
            else if (inst == "sub")
            {
                result = val1 - val2;
            }
            else if (inst == "mul")
            {
                result = val1 * val2;
            }
            else if (inst == "div")
            {
                if (val2 != 0)
                {
                    result = val1 / val2;
                }
                else
                {
                    result = 0;
                }
            }
            setVariableValue(callStack.peek(), targetVar, result);
            timeline.record(buildSnapshot(callStack));
        }
        else if (inst == "call")
        {
            int64_t targetOff = recOffset;
            int64_t retPos = ftell(f);
            fseek(f, (long)targetOff, SEEK_SET);
            string funcHeaderLine;
            readResolveRecord(f, funcHeaderLine);
            Token funcTokens[MAX_TOKENS];
            int32_t fCount = tokenizeLine(funcHeaderLine, funcTokens, MAX_TOKENS);
            Frame newFrame;
            if (fCount > 1)
            {
                newFrame.func_name = funcTokens[1].text;
            }
            else
            {
                newFrame.func_name = "";
            }
            newFrame.argc = 0;
            newFrame.localCount = 0;
            newFrame.returnLine = (int32_t)retPos;
            int32_t numArgs = count - 2;
            for (int i = 0; i < numArgs && i < MAX_VARS_PER_FRAME; i++)
            {
                string paramName = "";
                if (i + 2 < fCount)
                {
                    paramName = funcTokens[i + 2].text;
                }
                else
                {
                    paramName = "arg" + to_string(i);
                }
                int32_t argVal = resolveOperand(callStack.peek(), tokens[i + 2].text);
                newFrame.argv[newFrame.argc].name = paramName;
                newFrame.argv[newFrame.argc].value = argVal;
                newFrame.argc++;
            }
            callStack.push(newFrame);
            timeline.record(buildSnapshot(callStack));
        }
        else if (inst == "func_end")
        {
            Frame tempFrame = callStack.pop();
            if (!callStack.isEmpty())
            {
                fseek(f, (long)tempFrame.returnLine, SEEK_SET);
                for (int i = 0; i < tempFrame.argc; i++)
                {
                    setVariableValue(callStack.peek(), tempFrame.argv[i].name, tempFrame.argv[i].value);
                }
                timeline.record(buildSnapshot(callStack));
            }
        }
    }
    fclose(f);
}

// PASS 0x3: SERIALIZE TIMELINE
void writeString(FILE* f, string str)
{
    int32_t len = (int32_t)str.length();
    fwrite(&len, sizeof(int32_t), 1, f);
    if (len > 0)
    {
        fwrite(str.c_str(), sizeof(char), len, f);
    }
}
void writeFrame(FILE* f, const Frame& frame)
{
    writeString(f, frame.func_name);
    fwrite(&frame.argc, sizeof(int32_t), 1, f);
    for (int i = 0; i < frame.argc; i++)
    {
        writeString(f, frame.argv[i].name);
        fwrite(&frame.argv[i].value, sizeof(int32_t), 1, f);
    }
    fwrite(&frame.returnLine, sizeof(int32_t), 1, f);
    fwrite(&frame.localCount, sizeof(int32_t), 1, f);
    for (int i = 0; i < frame.localCount; i++)
    {
        writeString(f, frame.locals[i].name);
        fwrite(&frame.locals[i].value, sizeof(int32_t), 1, f);
    }
}
void writeSnapshot(FILE* f, const Snapshot& s)
{
    fwrite(&s.stackDepth, sizeof(int32_t), 1, f);
    for (int i = 0; i < s.stackDepth; i++)
    {
        writeFrame(f, s.callStack[i]);
    }
}

void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
    FILE* f = fopen(tdbgPath, "wb+");
    if (!f)
    {
        return;
    }
    int32_t steps = timeline.getStepCount();

    TTDBHeader header;
    header.magic[0] = 'T';
    header.magic[1] = 'T';
    header.magic[2] = 'D';
    header.magic[3] = 'B';
    header.version = 1;
    header.stepCount = steps;
    header.indexOffset = 0;
    writeHeader(f, header);

    int64_t* offsets = nullptr;
    if (steps > 0)
    {
        offsets = new int64_t[steps];
    }

    TimelineNode* curr = timeline.begin();
    int idx = 0;
    while (curr != nullptr && idx < steps)
    {
        offsets[idx] = ftell(f);
        if (curr->data != nullptr)
        {
            writeSnapshot(f, *(curr->data));
        }
        idx++;
        curr = curr->next;
    }

    int64_t tableOffset = ftell(f);
    header.indexOffset = tableOffset;
    if (steps > 0 && offsets != nullptr)
    {
        fwrite(offsets, sizeof(int64_t), steps, f);
        delete[] offsets;
    }

    fseek(f, 0, SEEK_SET);
    writeHeader(f, header);
    fclose(f);
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
