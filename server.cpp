// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
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
        Node(const T& val) : data(val), next(nullptr) {}
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
    ~Stack()
    {
        while (!isEmpty()) {
            pop();
        }
    }
    void push(const T& val)
    {
        if (count >= MAX_STACK_DEPTH) return;
        Node* temp = new Node(val);
        if (isEmpty()) {
            top = temp;
            count++;
            return;
        }
        temp->next = top;
        top = temp;
        count++;

        // pushes the value on the stack if max limit is not reached yet.
    }
    void pop()
    {
        if (isEmpty()) {
            return;
        }
        Node* temp = top;
        top = top->next;
        delete temp;
        count--;
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
        int32_t written = 0;
        Node* current = top;
        while (current != nullptr and written < maxLen) {
            out[written++] = current->data;
            current = current->next;
        }
        return written;
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
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
    }
    void record(Snapshot* s)
    {
        // add record in the timeline
    }
    TimelineNode* begin()
    {
    }
    int32_t getStepCount()
    {
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
    string line="";
    char ch;
    while (in.get(ch)) {
        if (ch == '\n' || ch == 10) {
            size_t first = line.find_first_not_of(" \t\r");
            if (first != string::npos) {
                size_t last = line.find_last_not_of(" \t\r");
                out = line.substr(first, (last - first + 1));
                return true; 
            }
            line = ""; 
        }
        else if (ch != '\r' && ch != 13) {
            line += ch;
        }
    }
    size_t first = line.find_first_not_of(" \t\r");
    if (first != string::npos) {
        size_t last = line.find_last_not_of(" \t\r");
        out = line.substr(first, (last - first + 1));
        return true;
    }

    out = "";
    return false;
    // reads the next nonblank line
}
string firstWord(const string& line)
{
    string s = "";
    for (int i = 0; i < line.size(); i++) {
        if (line[i] != ' ')s += line[i];
        else {
            break;
        }
    }
    return s;
    // returns first word from the input string
}
string secondWord(const string& line)
{
    int count = 0;
    string s = "";

    for (int i = 0; i < line.size(); i++) {
        if (line[i] == ' ') {
            if (count == 0) count = 1;
            else if (count == 2) return s;
        }
        else if (line[i] != ' ') {
            if (count == 1) count = 2;

            if (count == 2) {
                s += line[i];
            }
        }
    }
    return s;
}
bool validateProgram(const char* sourcePath)
{
    ifstream in(sourcePath,ios::binary);
    if (!in.is_open())return false;
    string line;
    Stack<string>callstack;
    while (readSourceLine(in, line)) {
        string first_word = firstWord(line);
        if (first_word == "func") {
            if (!callstack.isEmpty())return false;
            // pushing the function name onto the stack now 
            string second_word = secondWord(line);
            callstack.push(second_word);
        }
        else if (first_word == "func_end") {
            if (callstack.isEmpty())return false;
             callstack.pop();
        }
    }
    if (callstack.isEmpty())return true; // meaning that every func has a func_end
    if (!callstack.isEmpty())return false; // no function end for a func start
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    int64_t recordoffset = _ftelli64(f);
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    int32_t Stringsize = static_cast<int32_t>(text.size());
    fwrite(&Stringsize, sizeof(int32_t), 1, f);
    if (Stringsize > 0) {
        fwrite(text.c_str(), sizeof(char), Stringsize, f);
    }
    return recordoffset;
        // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    int64_t offset_field = -1;
    int32_t Stringsize = 0;
    // read the 8byte offset
    if (fread(&offset_field, sizeof(int64_t), 1, f)!= 1) {
        outText = "";
        return -1;
    }
    // read 4 byte string size
    if (fread(&Stringsize, sizeof(int32_t), 1, f) != 1) {
        outText = "";
        return -1;
    }
    // now read string
    if (Stringsize > 0) {
        outText.resize(Stringsize);
        fread(&outText[0], sizeof(char), Stringsize, f);
    }
    else {
        outText = "";
    }
    return offset_field;
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    ifstream in(sourcePath, ios::binary);
    if (!in.is_open()) return -1;
    FILE* f = fopen(resolveBinPath, "wb+");
    if (!f) { 
      in.close();
      return -1;
    }
    string line;
    int64_t totalRecordsWritten = 0;
    while (readSourceLine(in, line)) {
        string first = firstWord(line);
        string second = secondWord(line);
        int64_t targetoffset = -1;
        if (first == "func") {
            funcArray[funcCount].funcName = second;
            funcArray[funcCount].byteOffsetInResolveBin = _ftelli64(f);
            funcCount++;
        }
        else if (first == "call") {
            for (int i = 0; i < funcCount; i++) {
                if (funcArray[i].funcName == second) {
                    targetoffset = funcArray[i].byteOffsetInResolveBin;
                    break;
                }
            }
        }
        if (targetoffset == -1) {
            patches[patchCount].targetFuncName = second;
            patches[patchCount].byteOffsetOfOffsetField = _ftelli64(f);
            patchCount++;
        }
        writeResolveRecord(f, targetoffset, line);
        totalRecordsWritten++;
    }
    in.close();
    fclose(f);
    return totalRecordsWritten;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
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
    string word = "";
    int32_t count = 0;

    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == ' ' || line[i] == '\t') {
            if (!word.empty()) {
                if (count == maxTokens) return count;
                tokens[count].text = word;
                if (count == 0) tokens[count].type = KEYWORD; // first is keywprd
                else if (count == 1) tokens[count].type = IDENTIFIER; // second word is identifier
                else tokens[count].type = PARAM; // arguments 

                count++;
                word = ""; // Resetting word for next token
            }
        }
        else {
            word += line[i];
        }
    }
    // Process the final word at the end of the line
    if (!word.empty() && count < maxTokens) {
        tokens[count].text = word;
        if (count == 0) tokens[count].type = KEYWORD;
        else if (count == 1) tokens[count].type = IDENTIFIER;
        else tokens[count].type = PARAM;
        count++;
    }
    return count;
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    Snapshot* s = new Snapshot();
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
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