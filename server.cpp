// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to Source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolver.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolver.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
//#include <unistd.h>
//#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include "helper.h"
#ifdef _WIN32
#define fseeko _fseeki64
#define ftello _ftelli64
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SSIZE_T ssize_t; // Windows fallback
#define close_socket(s) closesocket(s)
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define close_socket(s) close(s)
#endif

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
        while (current != nullptr && written < maxLen) {
            out[written++] = current->data;
            current = current->next;
        }

        // Reverse so index 0 is main (bottom) and highest index is top of stack
        for (int32_t i = 0; i < written / 2; i++) {
            T temp = out[i];
            out[i] = out[written - 1 - i];
            out[written - 1 - i] = temp;
        }
        return written;
        // copies every frame, bottom to top in the array given as a parameter
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
    TimelineNode(Snapshot* s1) {
        data = s1;
        next = prev = nullptr;
    }
};
class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = tail = nullptr;
        stepCount = 0;
    }
    ~Timeline()
    {
        TimelineNode* current = head;
        while (current != nullptr) {
            TimelineNode* nextNode = current->next;
            delete current->data;
            delete current;
            current = nextNode;
        }
    }
    void record(Snapshot* s)
    {
        TimelineNode* temp = new TimelineNode(s);
        if (stepCount == 0) {
            head = tail = temp;
            stepCount++;
            return;

        }
        tail->next = temp;
        temp->prev = tail;
        tail = temp;
        stepCount++;
        // add record in the timeline
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
    int64_t returnLine; // FIXED: 64-bit to store file byte offsets accurately
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

// resolver.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolver.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING Source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    string line = "";
    char ch;
    while (in.get(ch)) {
        if (ch == '\n' || ch == 10) {
            size_t first = line.find_first_not_of(" \t\r"); // returns the position size)t is unsigned int 
            if (first != string::npos) { // npos is basically not found used to check if something is in string or not 
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
    for (size_t i = 0; i < line.size(); i++) {
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

    for (size_t i = 0; i < line.size(); i++) {
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
    ifstream in(sourcePath, ios::binary);
    if (!in.is_open())throw runtime_error("Error couldnot open source file" + string(sourcePath) + " .");
    string line;
    Stack<string>callstack;
    int32_t lineNumber = 0;
    while (readSourceLine(in, line)) {
        string first_word = firstWord(line);
        lineNumber++;
        if (first_word == "func") {
            if (!callstack.isEmpty()) {
                in.close();
                throw runtime_error("Syntax Error [Line " + to_string(lineNumber) + "]: nested function definition '" + secondWord(line) + "' inside '" + callstack.peek() + "' is not allowed.");
            }
            // pushing the function name onto the stack now 
            string second_word = secondWord(line);
            if (second_word.empty()) {
                in.close();
                throw runtime_error("Syntax Error [Line " + to_string(lineNumber) + "]: 'func' keyword missing function identifier name.");
            }
            callstack.push(second_word);

        }
        else if (first_word == "func_end") {
            if (callstack.isEmpty()) {
                in.close();
                throw runtime_error("syntax error [Line " + to_string(lineNumber) + "]: Unmatched 'func_end' without a preceding 'func' declaration.");
            }
            callstack.pop();
        }
    }
    in.close();
    if (!callstack.isEmpty())throw runtime_error("Syntax Error: Function '" + callstack.peek() + "' was never closed with 'func_end' before end-of-file."); //no function end for a func start
    return true;
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
}

// PASS 0x1: RESOLVE() -> resolver.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text) // 8 byte target offset
{
    int64_t recordoffset = ftello(f); // current file position in bytes 
    fwrite(&offsetField, sizeof(int64_t), 1, f); // if call instruction then it swrites 8byte offset otherwise -1
    int32_t Stringsize = static_cast<int32_t>(text.size());
    fwrite(&Stringsize, sizeof(int32_t), 1, f);
    if (Stringsize > 0) {
        fwrite(text.c_str(), sizeof(char), Stringsize, f);
    }
    return recordoffset;
    // writes one [offset(8B)][size(4B)][string] record at the current file position
// returns this record's own starting byte position
}
int64_t readResolveRecord(FILE* f, string& outText) // f is basically current position 
{
    int64_t offset_field = -1;
    int32_t Stringsize = 0;
    // read the 8byte offset
    if (fread(&offset_field, sizeof(int64_t), 1, f) != 1) {
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
        char* buffer = new char[Stringsize + 1];
        size_t bytesRead = fread(buffer, sizeof(char), Stringsize, f);
        buffer[bytesRead] = '\0';

        outText = string(buffer);
        delete[] buffer;
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
    if (!in.is_open()) {
        throw runtime_error("Linker Error Could not open source file '" + string(sourcePath) + "'.");
    }
    FILE* f = fopen(resolveBinPath, "wb+");
    if (!f) {
        in.close();
        throw runtime_error("Linker Error Could not create binary file at path '" + string(resolveBinPath) + "'. Check write permissions.");
    }

    string line;
    int64_t mainEntryOffset = -1;

    //  Record Functions, Patch Targets, and Write Binary Records
    while (readSourceLine(in, line)) {
        string first = firstWord(line);
        string second = secondWord(line);
        int64_t targetOffset = -1;

        if (first == "func") {
            //  Exceeded maximum function symbol capacity
            if (funcCount >= MAX_FUNCS) {
                in.close();
                fclose(f);
                throw runtime_error("Linker Error Exceeded maximum allowed functions limit (" + to_string(MAX_FUNCS) + ").");
            }

            //  Duplicate function definition
            for (int i = 0; i < funcCount; i++) {
                if (funcArray[i].funcName == second) {
                    in.close();
                    fclose(f);
                    throw runtime_error("Linker Error Duplicate function definition '" + second + "' encountered.");
                }
            }

            int64_t recordStart = ftello(f);
            if (second == "main") {
                mainEntryOffset = recordStart;
            }

            funcArray[funcCount].funcName = second;
            funcArray[funcCount].byteOffsetInResolveBin = recordStart;
            funcCount++;
        }
        else if (first == "call") {
            // if target function was already defined earlier in source
            for (int i = 0; i < funcCount; i++) {
                if (funcArray[i].funcName == second) {
                    targetOffset = funcArray[i].byteOffsetInResolveBin;
                    break;
                }
            }

            // Forward call reference -> record pending patch
            if (targetOffset == -1) {
                //  Exceeded maximum pending forward patch capacity
                if (patchCount >= MAX_PATCHES) {
                    in.close();
                    fclose(f);
                    throw runtime_error("Linker Error Exceeded maximum forward calls limit (" + to_string(MAX_PATCHES) + ").");
                }

                patches[patchCount].targetFuncName = second;
                patches[patchCount].byteOffsetOfOffsetField = ftello(f);
                patchCount++;
            }
        }

        writeResolveRecord(f, targetOffset, line);
    }

    in.close();

    // Backpatch Forward Calls to Resolved Offsets
    for (int i = 0; i < patchCount; i++) {
        int64_t resolvedOffset = -1;
        for (int j = 0; j < funcCount; j++) {
            if (funcArray[j].funcName == patches[i].targetFuncName) {
                resolvedOffset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }

        // Function called in program but never defined
        if (resolvedOffset == -1) {
            fclose(f);
            throw runtime_error("Linker Error Unresolved reference to function '" + patches[i].targetFuncName + "'.");
        }

        // Backpatch the 8-byte targetOffset field in resolver.bin
        fseeko(f, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&resolvedOffset, sizeof(int64_t), 1, f);
    }

    fclose(f);

    if (mainEntryOffset == -1) {
        throw runtime_error("Linker Error Entry point 'func main' was not found in source.");
    }

    return mainEntryOffset;
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum _TTokenType
{
    TOKEN_KEYWORD,
    TOKEN_IDENTIFIER,
    TOKEN_PARAM
};
struct Token
{
    _TTokenType type;
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
                if (count == 0) tokens[count].type = TOKEN_KEYWORD;
                else if (count == 1) tokens[count].type = TOKEN_IDENTIFIER;
                else tokens[count].type = TOKEN_PARAM;

                count++;
                word = "";
            }
        }
        else {
            word += line[i];
        }
    }
    if (!word.empty() && count < maxTokens) {
        tokens[count].text = word;
        if (count == 0) tokens[count].type = TOKEN_KEYWORD;
        else if (count == 1) tokens[count].type = TOKEN_IDENTIFIER;
        else tokens[count].type = TOKEN_PARAM;
        count++;
    }
    return count;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    Snapshot* s = new Snapshot();
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    FILE* f = fopen(resolveBinPath, "rb");

    if (!f) {
        throw runtime_error("Runtime Error could not open binary file '" + string(resolveBinPath) + "'.");
    }

    if (mainOffset < 0) {
        fclose(f);
        throw runtime_error("Runtime Error invalid main entry point offset (" + to_string(mainOffset) + ").");
    }

    Stack<Frame> call_stack;

    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.returnLine = -1;
    mainFrame.localCount = 0;

    call_stack.push(mainFrame);

    fseeko(f, mainOffset, SEEK_SET);

    while (!call_stack.isEmpty()) {
        if (call_stack.depth() >= MAX_STACK_DEPTH) {
            fclose(f);
            throw runtime_error("Stack Overflow Error exceeded maximum call stack depth (" + to_string(MAX_STACK_DEPTH) + ") in function '" + call_stack.peek().func_name + "'.");
        }

        Token tokens[MAX_TOKENS];
        string line = "";

        int64_t targetOffset = readResolveRecord(f, line);

        if (line.empty()) {
            break;
        }

        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }

        int32_t token_count = tokenizeLine(line, tokens, MAX_TOKENS);

        if (token_count == 0) {
            continue;
        }

        string op = tokens[0].text;
        Frame& currentFrame = call_stack.peek();

        if (op == "set") {
            if (token_count < 3) {
                fclose(f);
                throw runtime_error("Syntax Error 'set' instruction requires variable name and value.");
            }

            string var_name = tokens[1].text;
            int32_t val = 0;
            try {
                val = stoi(tokens[2].text);
            }
            catch (...) {
                fclose(f);
                throw runtime_error("Runtime Error invalid integer value '" + tokens[2].text + "' for set instruction.");
            }

            Variable* destVar = nullptr;
            for (int i = 0; i < currentFrame.argc; i++) {
                if (currentFrame.argv[i].name == var_name) { destVar = &currentFrame.argv[i]; break; }
            }
            if (!destVar) {
                for (int i = 0; i < currentFrame.localCount; i++) {
                    if (currentFrame.locals[i].name == var_name) { destVar = &currentFrame.locals[i]; break; }
                }
            }

            if (destVar != nullptr) {
                destVar->value = val;
            }
            else {
                if (currentFrame.localCount >= MAX_VARS_PER_FRAME) {
                    fclose(f);
                    throw runtime_error("Runtime Error exceeded local variable limit (" + to_string(MAX_VARS_PER_FRAME) + ") in frame '" + currentFrame.func_name + "'.");
                }
                currentFrame.locals[currentFrame.localCount].name = var_name;
                currentFrame.locals[currentFrame.localCount].value = val;
                currentFrame.localCount++;
            }
        }

        else if (op == "add" || op == "sub" || op == "mul" || op == "div") {
            if (token_count < 3) {
                fclose(f);
                throw runtime_error("Syntax Error arithmetic '" + op + "' requires destination variable and operand.");
            }

            string dest_name = tokens[1].text;
            string src_name = tokens[2].text;

            Variable* destVar = nullptr;
            for (int i = 0; i < currentFrame.argc; i++) {
                if (currentFrame.argv[i].name == dest_name) { destVar = &currentFrame.argv[i]; break; }
            }
            if (!destVar) {
                for (int i = 0; i < currentFrame.localCount; i++) {
                    if (currentFrame.locals[i].name == dest_name) { destVar = &currentFrame.locals[i]; break; }
                }
            }

            if (destVar == nullptr) {
                fclose(f);
                throw runtime_error("Runtime Error destination variable '" + dest_name + "' is not defined in frame '" + currentFrame.func_name + "'.");
            }

            Variable* srcVar = nullptr;
            for (int i = 0; i < currentFrame.argc; i++) {
                if (currentFrame.argv[i].name == src_name) { srcVar = &currentFrame.argv[i]; break; }
            }
            if (!srcVar) {
                for (int i = 0; i < currentFrame.localCount; i++) {
                    if (currentFrame.locals[i].name == src_name) { srcVar = &currentFrame.locals[i]; break; }
                }
            }

            int32_t src_val = 0;
            if (srcVar != nullptr) {
                src_val = srcVar->value;
            }
            else {
                try {
                    src_val = stoi(src_name);
                }
                catch (...) {
                    fclose(f);
                    throw runtime_error("Runtime Error symbol '" + src_name + "' is neither a valid variable nor integer literal.");
                }
            }

            if (op == "add") {
                destVar->value += src_val;
            }
            else if (op == "sub") {
                destVar->value -= src_val;
            }
            else if (op == "mul") {
                destVar->value *= src_val;
            }
            else if (op == "div") {
                if (src_val == 0) {
                    fclose(f);
                    throw runtime_error("Math Error Division by zero is not possible its encounted in frame '" + currentFrame.func_name + "'.");
                }
                destVar->value /= src_val;
            }
        }

        else if (op == "call") {
            if (token_count < 2) {
                fclose(f);
                throw runtime_error("Syntax Error 'call' instruction missing target function name.");
            }

            if (targetOffset < 0) {
                fclose(f);
                throw runtime_error("Linker Error unresolved jump target for function '" + tokens[1].text + "'.");
            }

            Frame new_frame;
            new_frame.func_name = tokens[1].text;
            new_frame.localCount = 0;
            new_frame.argc = 0;
            new_frame.returnLine = ftello(f); // FIXED: 64-bit return offset

            for (int i = 2; i < token_count; i++) {
                if (new_frame.argc >= MAX_VARS_PER_FRAME) {
                    fclose(f);
                    throw runtime_error("Runtime Error exceeded maximum argument limit for function '" + new_frame.func_name + "'.");
                }

                string argName = tokens[i].text;
                Variable* argVar = nullptr;

                for (int j = 0; j < currentFrame.argc; j++) {
                    if (currentFrame.argv[j].name == argName) { argVar = &currentFrame.argv[j]; break; }
                }
                if (!argVar) {
                    for (int j = 0; j < currentFrame.localCount; j++) {
                        if (currentFrame.locals[j].name == argName) { argVar = &currentFrame.locals[j]; break; }
                    }
                }

                int32_t val = 0;
                if (argVar != nullptr) {
                    val = argVar->value;
                }
                else {
                    try {
                        val = stoi(argName);
                    }
                    catch (...) {
                        fclose(f);
                        throw runtime_error("Runtime Error argument '" + argName + "' passed to '" + new_frame.func_name + "' is invalid.");
                    }
                }
                new_frame.argv[new_frame.argc].name = "arg" + to_string(new_frame.argc);
                new_frame.argv[new_frame.argc].value = val;
                new_frame.argc++;
            }

            call_stack.push(new_frame);
            fseeko(f, targetOffset, SEEK_SET);
        }

        else if (op == "func") {
            int expArgc = token_count - 2;
            if (currentFrame.argc != expArgc) {
                fclose(f);
                throw runtime_error("Runtime Error argument count mismatch for '" + currentFrame.func_name + "'. Expected " + to_string(expArgc) + ", got " + to_string(currentFrame.argc) + ".");
            }

            for (int i = 2; i < token_count; i++) {
                currentFrame.argv[i - 2].name = tokens[i].text;
            }
        }

        else if (op == "func_end") {
            int64_t address = currentFrame.returnLine;
            call_stack.pop();

            if (address != -1) {
                fseeko(f, address, SEEK_SET);
            }
            else {
                break;
            }
        }

        else {
            fclose(f);
            throw runtime_error("Runtime Execution Error invalid or unrecognized instruction '" + op + "'.");
        }

        Snapshot* ss = buildSnapshot(call_stack);
        timeline.record(ss);
    }

    fclose(f);
}

// PASS 0x3: SERIALIZE TIMELINE
bool writeTdbg(Timeline& timeline, const char* traceBinPath)
{
    FILE* f = fopen(traceBinPath, "wb+");
    if (!f) throw runtime_error("IO Error unable to create trace binary file at path '" + string(traceBinPath) + ". Check write permissions.");

    int32_t totalSteps = timeline.getStepCount();
    if (totalSteps == 0) {
        fclose(f);
        throw runtime_error("Warning Attempt to write the trace file with 0 recorded steps ");
    }
    TTDBHeader header;
    header.magic[0] = 'T';
    header.magic[1] = 'T';
    header.magic[2] = 'D';
    header.magic[3] = 'B';
    header.version = 1;
    header.stepCount = totalSteps;
    header.indexOffset = 0;

    fwrite(&header, sizeof(TTDBHeader), 1, f);

    int64_t* stepOffsets = new int64_t[totalSteps];
    int32_t stepIndex = 0;

    TimelineNode* current = timeline.begin();

    while (current != nullptr && stepIndex < totalSteps) {
        stepOffsets[stepIndex++] = ftello(f);

        Snapshot* s = current->data;

        fwrite(&(s->stackDepth), sizeof(int32_t), 1, f);

        for (int32_t i = 0; i < s->stackDepth; i++) {
            Frame& frame = s->callStack[i];

            int32_t nameLen = (int32_t)frame.func_name.size();
            fwrite(&nameLen, sizeof(int32_t), 1, f);
            if (nameLen > 0) {
                fwrite(frame.func_name.c_str(), sizeof(char), nameLen, f);
            }

            int32_t retLine32 = static_cast<int32_t>(frame.returnLine);
            fwrite(&retLine32, sizeof(int32_t), 1, f);

            fwrite(&(frame.argc), sizeof(int32_t), 1, f);
            for (int32_t a = 0; a < frame.argc; a++) {
                int32_t argNameLen = (int32_t)frame.argv[a].name.size();
                fwrite(&argNameLen, sizeof(int32_t), 1, f);
                if (argNameLen > 0) {
                    fwrite(frame.argv[a].name.c_str(), sizeof(char), argNameLen, f);
                }
                fwrite(&(frame.argv[a].value), sizeof(int32_t), 1, f);
            }

            fwrite(&(frame.localCount), sizeof(int32_t), 1, f);
            for (int32_t v = 0; v < frame.localCount; v++) {
                int32_t varNameLen = (int32_t)frame.locals[v].name.size();
                fwrite(&varNameLen, sizeof(int32_t), 1, f);
                if (varNameLen > 0) {
                    fwrite(frame.locals[v].name.c_str(), sizeof(char), varNameLen, f);
                }
                fwrite(&(frame.locals[v].value), sizeof(int32_t), 1, f);
            }
        }

        current = current->next;
    }

    int64_t indexTableStart = ftello(f);
    if (totalSteps > 0) {
        fwrite(stepOffsets, sizeof(int64_t), totalSteps, f);
    }

    header.indexOffset = indexTableStart;
    fseeko(f, 0, SEEK_SET);
    fwrite(&header, sizeof(TTDBHeader), 1, f);

    delete[] stepOffsets;
    fclose(f);

    return true;
}

// Network Helpers
bool sendAll(int sock, const char* buf, size_t n)
{
    size_t totalsent = 0;
    while (totalsent < n) {
        ssize_t bytes = send(sock, buf + totalsent, static_cast<int>(n - totalsent), 0);
        if (bytes <= 0) return false;
        totalsent += static_cast<size_t>(bytes);
    }
    return true;
}
void sendServerError(int sock, const string& msg)
{
    char statusByte = 1;
    uint32_t crc = 0;
    crc = update_crc32(crc, &statusByte, 1);
    crc = update_crc32(crc, msg.data(), msg.size());

    string packet;
    packet.push_back(statusByte);
    packet.append(msg);

    uint32_t netCrc = htonl(crc);
    packet.append(reinterpret_cast<const char*>(&netCrc), 4);

    sendAll(sock, packet.data(), packet.size());
}

void sendServerSuccess(int sock, const vector<char>& tdbgData)
{
    char statusByte = 0;
    uint32_t crc = 0;
    crc = update_crc32(crc, &statusByte, 1);
    crc = update_crc32(crc, tdbgData.data(), tdbgData.size());

    string packet;
    packet.push_back(statusByte);
    packet.append(tdbgData.data(), tdbgData.size());

    uint32_t netCrc = htonl(crc);
    packet.append(reinterpret_cast<const char*>(&netCrc), 4);

    sendAll(sock, packet.data(), packet.size());
}
int createListenSocket(int port)
{
    int sock = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (sock < 0) return -1;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close_socket(sock);
        return -1;
    }

    if (listen(sock, 16) < 0) {
        close_socket(sock);
        return -1;
    }

    return sock;
}
void printTimelineToConsole(Timeline& timeline)
{
    cout << "\n";
    cout << "               EXECUTION TIMELINE HISTORY               \n";
    cout << "\n";

    TimelineNode* current = timeline.begin();
    int step = 0;

    while (current != nullptr) {
        Snapshot* s = current->data;
        cout << "\n[Step " << step++ << "] Stack Depth: " << s->stackDepth << "\n";

        for (int i = s->stackDepth - 1; i >= 0; i--) {
            Frame& frame = s->callStack[i];
            cout << "   Frame: " << frame.func_name;

            if (frame.argc > 0) {
                cout << " (Args: ";
                for (int a = 0; a < frame.argc; a++) {
                    cout << frame.argv[a].name << "=" << frame.argv[a].value;
                    if (a < frame.argc - 1) cout << ", ";
                }
                cout << ")";
            }

            if (frame.localCount > 0) {
                cout << " [Locals: ";
                for (int v = 0; v < frame.localCount; v++) {
                    cout << frame.locals[v].name << "=" << frame.locals[v].value;
                    if (v < frame.localCount - 1) cout << ", ";
                }
                cout << "]";
            }
            cout << "\n";
        }

        current = current->next;
    }

    cout << "\n";
    cout << "Total Steps Recorded: " << timeline.getStepCount() << "\n\n";
}

// Main Routine
int main(int argc, char* argv[])
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    int port = 5001;
    if (argc >= 2) port = atoi(argv[1]);
    int listenSock = createListenSocket(port);
    if (listenSock < 0)
    {
        cerr << "[Server] Failed to create listen socket on port " << port << endl;
        return 1;
    }
    cout << "[Server] Listening on port " << port << "..." << endl;
    while (true)
    {
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);
        int sock = static_cast<int>(accept(listenSock, (struct sockaddr*)&clientAddr, &addrLen));
        if (sock < 0) continue;
        vector<char> incomingData;
        char buffer[4096];
        while (true)
        {
            ssize_t bytes = recv(sock, buffer, sizeof(buffer), 0);
            if (bytes <= 0) break;
            incomingData.insert(incomingData.end(), buffer, buffer + static_cast<size_t>(bytes));
        }
        if (incomingData.size() < 4)
        {
            sendServerError(sock, "invalid or empty upload payload");
            close_socket(sock);
            continue;
        }
        size_t payloadLen = incomingData.size() - 4;
        uint32_t receivedCrc;
        memcpy(&receivedCrc, incomingData.data() + payloadLen, 4);
        receivedCrc = ntohl(receivedCrc);
        uint32_t computedCrc = 0;
        computedCrc = update_crc32(computedCrc, incomingData.data(), payloadLen);
        if (computedCrc != receivedCrc)
        {
            sendServerError(sock, "upload checksum mismatch");
            close_socket(sock);
            continue;
        }
        ofstream sourceFile("Source.bin", ios::binary);
        if (!sourceFile.is_open())
        {
            sendServerError(sock, "failed to create Source.bin on server disk");
            close_socket(sock);
            continue;
        }
        sourceFile.write(incomingData.data(), payloadLen);
        sourceFile.close();
        Timeline timeline;
        try
        {
            if (!validateProgram("Source.bin"))
            {
                sendServerError(sock, "program validation failed");
                close_socket(sock);
                continue;
            }
            int64_t mainOffset = resolveProgram("Source.bin", "resolver.bin");
            executeProgram("resolver.bin", mainOffset, timeline);
            writeTdbg(timeline, "session.tdbg");
            ifstream tdbgFile("session.tdbg", ios::binary | ios::ate);
            if (!tdbgFile.is_open())
            {
                sendServerError(sock, "failed to open session.tdbg after generation");
                close_socket(sock);
                continue;
            }
            streamsize size = tdbgFile.tellg();
            tdbgFile.seekg(0, ios::beg);
            vector<char> tdbgBuffer(size);
            tdbgFile.read(tdbgBuffer.data(), size);
            tdbgFile.close();
            sendServerSuccess(sock, tdbgBuffer);
        }
        catch (const exception& e)
        {
            sendServerError(sock, string(e.what()));
        }
        close_socket(sock);
    }
    close_socket(listenSock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}