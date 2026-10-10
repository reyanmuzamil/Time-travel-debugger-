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
    if (!in.is_open())throw runtime_error("Error couldnot open source file" +string(sourcePath) +" .");
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
                throw runtime_error("Syntax Error [Line " + to_string(lineNumber) +"]: 'func' keyword missing function identifier name.");
            }
            callstack.push(second_word);

        }
        else if (first_word == "func_end") {
            if (callstack.isEmpty()){
                in.close();
                throw runtime_error("syntax error [Line " + to_string(lineNumber) + "]: Unmatched 'func_end' without a preceding 'func' declaration.");
            }
             callstack.pop();
        }
    }
    in.close();
    if (!callstack.isEmpty())throw runtime_error("Syntax Error: Function '" + callstack.peek() +"' was never closed with 'func_end' before end-of-file."); //no function end for a func start
    return true;
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
    if (!in.is_open()) {
        throw runtime_error("Linker Error Could not open source file '" + string(sourcePath) + "'.");
    }
    FILE* f = nullptr;
    fopen_s(&f, resolveBinPath, "wb+");
    if (!f) {
        in.close();
        throw runtime_error("Linker Error Could not create binary file at path '" + string(resolveBinPath) + "'. Check write permissions.");
    }

    string line;
    int64_t totalRecordsWritten = 0;

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

            funcArray[funcCount].funcName = second;
            funcArray[funcCount].byteOffsetInResolveBin = _ftelli64(f);
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
                patches[patchCount].byteOffsetOfOffsetField = _ftelli64(f);
                patchCount++;
            }
        }

        writeResolveRecord(f, targetOffset, line);
        totalRecordsWritten++;
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

        // Backpatch the 8-byte targetOffset field in resolve.bin
        _fseeki64(f, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&resolvedOffset, sizeof(int64_t), 1, f);
    }

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
    // Process  final word at the end of the line
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
    FILE* f = nullptr;
    fopen_s(&f, resolveBinPath, "rb");

    if (!f) {
        throw runtime_error("Runtime Error could not open binary file '" + string(resolveBinPath) + "'.");
    }

    // Main entry Point Validation
    if (mainOffset < 0) {
        fclose(f);
        throw runtime_error("Runtime Error invalid main entry point offset (" + to_string(mainOffset) + ").");
    }

    Stack<Frame> call_stack;

    // initialize Base main Frame
    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.returnLine = -1;
    mainFrame.localCount = 0;

    call_stack.push(mainFrame);

    // Jump to the main function entry point in resolve.bin
    _fseeki64(f, mainOffset, SEEK_SET);

    while (!call_stack.isEmpty()) {
        if (call_stack.depth() >= MAX_STACK_DEPTH) {
            fclose(f);
            throw runtime_error("Stack Overflow Error exceeded maximum call stack depth (" +to_string(MAX_STACK_DEPTH) + ") in function '" +call_stack.peek().func_name + "'.");
        }

        Token tokens[MAX_TOKENS];
        string line = "";

        // Read binary record 
        int64_t targetOffset = readResolveRecord(f, line);

        if (line.empty()) {
            break; // End of file reached
        }

        int32_t token_count = tokenizeLine(line, tokens, MAX_TOKENS);

        if (token_count == 0) {
            continue;
        }

        string op = tokens[0].text;
        Frame& currentFrame = call_stack.peek();
        // SET INSTRUCTION 
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
                    throw runtime_error("Runtime Error exceeded local variable limit (" +to_string(MAX_VARS_PER_FRAME) + ") in frame '" + currentFrame.func_name + "'.");
                }
                currentFrame.locals[currentFrame.localCount].name = var_name;
                currentFrame.locals[currentFrame.localCount].value = val;
                currentFrame.localCount++;
            }
        }

        //  ARITHMETIC INSTRUCTIONS
        else if (op == "add" || op == "sub" || op == "mul" || op == "div") {
            if (token_count < 3) {
                fclose(f);
                throw runtime_error("Syntax Error arithmetic '" + op + "' requires destination variable and operand.");
            }

            string dest_name = tokens[1].text;
            string src_name = tokens[2].text;

            // Resolve destination variable
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
                throw runtime_error("Runtime Error destination variable '" + dest_name +"' is not defined in frame '" + currentFrame.func_name + "'.");
            }

            // Resolve source operand
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
                //  Division by zero
                if (src_val == 0) {
                    fclose(f);
                    throw runtime_error("Math Error Division by zero  is not possible its encounted in frame '" + currentFrame.func_name + "'.");
                }
                destVar->value /= src_val;
            }
        }

        //  CALL INSTRUCTION 
        else if (op == "call") {
            if (token_count < 2) {
                fclose(f);
                throw runtime_error("Syntax Error 'call' instruction missing target function name.");
            }

            if (targetOffset < 0) {
                fclose(f);
                throw runtime_error("Linker Error  unresolved jump target for function '" + tokens[1].text + "'.");
            }

            Frame new_frame;
            new_frame.func_name = tokens[1].text;
            new_frame.localCount = 0;
            new_frame.argc = 0;
            new_frame.returnLine = (int32_t)_ftelli64(f); // Save return offset

            // Bind function call parameters
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
            _fseeki64(f, targetOffset, SEEK_SET);
        }

        //  FUNC HEADER INSTRUCTION 
        else if (op == "func") {
            int expArgc = token_count - 2;
            if (currentFrame.argc != expArgc) {
                fclose(f);
                throw runtime_error("Runtime Error argument count mismatch for '" + currentFrame.func_name +"'. Expected " + to_string(expArgc) + ", got " + to_string(currentFrame.argc) + ".");
            }

            // Bind names to passed argument values
            for (int i = 2; i < token_count; i++) {
                currentFrame.argv[i - 2].name = tokens[i].text;
            }
        }

        //  FUNC_END INSTRUCTION 
        else if (op == "func_end") {
            int32_t address = currentFrame.returnLine;
            call_stack.pop();

            if (address != -1) {
                _fseeki64(f, address, SEEK_SET);
            }
            else {
                break; // Exit loop when pops
            }
        }

        //  UNRECOGNIZED INSTRUCTION CHECK
        else {
            fclose(f);
            throw runtime_error("Runtime Execution Error invalid or unrecognized instruction '" + op + "'.");
        }

        // Record execution step into timeline
        Snapshot* ss = buildSnapshot(call_stack);
        timeline.record(ss);
    }

    fclose(f);
}    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
   
// PASS 0x3: SERIALIZE TIMELINE
bool writeTdbg(Timeline& timeline, const char* traceBinPath)
{
    FILE* f = nullptr;
    fopen_s(&f, traceBinPath, "wb+");
    if (!f) throw runtime_error("IO Error unable to create trace binary file at path '" + string(traceBinPath) + ". Check write permissions.");

    int32_t totalSteps = timeline.getStepCount();
    if (totalSteps == 0) {
        fclose(f);
        throw runtime_error("Warning Attempt to write the trace file with 0  recorded steps ");
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

    // Array to hold the file position of each snapshot for the index table
    int64_t* stepOffsets = new int64_t[totalSteps];
    int32_t stepIndex = 0;

    TimelineNode* current = timeline.begin();

    while (current != nullptr && stepIndex < totalSteps) {
        // Record current disk offset for Step 3's Index Table
        stepOffsets[stepIndex++] = _ftelli64(f);

        Snapshot* s = current->data;

        // Write stack depth for this snapshot
        fwrite(&(s->stackDepth), sizeof(int32_t), 1, f);

        // Serialize each frame in the snapshot's call stack
        for (int32_t i = 0; i < s->stackDepth; i++) {
            Frame& frame = s->callStack[i];

            // Write Function Name
            int32_t nameLen = (int32_t)frame.func_name.size();
            fwrite(&nameLen, sizeof(int32_t), 1, f);
            if (nameLen > 0) {
                fwrite(frame.func_name.c_str(), sizeof(char), nameLen, f);
            }

            // Return Line / Offset
            fwrite(&(frame.returnLine), sizeof(int32_t), 1, f);

            // Positional Arguments 
            fwrite(&(frame.argc), sizeof(int32_t), 1, f);
            for (int32_t a = 0; a < frame.argc; a++) {
                int32_t argNameLen = (int32_t)frame.argv[a].name.size();
                fwrite(&argNameLen, sizeof(int32_t), 1, f);
                if (argNameLen > 0) {
                    fwrite(frame.argv[a].name.c_str(), sizeof(char), argNameLen, f);
                }
                fwrite(&(frame.argv[a].value), sizeof(int32_t), 1, f);
            }

            // Local Variables
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

    int64_t indexTableStart = _ftelli64(f);
    if (totalSteps > 0) {
        fwrite(stepOffsets, sizeof(int64_t), totalSteps, f);
    }

    //  Backpatch header.indexOffset with the actual index table offset
    header.indexOffset = indexTableStart;
    _fseeki64(f, 0, SEEK_SET);
    fwrite(&header, sizeof(TTDBHeader), 1, f);

    delete[] stepOffsets;
    fclose(f);

    return true;
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