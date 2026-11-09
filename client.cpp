// ====================== = TIME - TRAVEL DEBUGGER - CLIENT TEMPLATE ====================== =

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include<fstream>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <vector>
#include "helper.h"
#ifdef _WIN32
      #include <winsock2.h>
      #include <ws2tcpip.h>
      #pragma comment(lib, "ws2_32.lib")
      typedef int socklen_t;
      #define close_socket closesocket
      #define SHUT_WR SD_SEND
      #ifndef MSG_NOSIGNAL
      #define MSG_NOSIGNAL 0
#endif
#else
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#define close_socket close
#endif
using namespace std;

// ---- Constants ----
const char* PROGRAM_NAME = "ttdb";
const char* PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char* TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

// These two must be the same as in Phase 1

const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
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

// ---- Structs only the client needs ----
struct TdbgReader
{
    size_t file_index; // file stream pointer keeps the file open so client doesnot always have to open it again and again 
    TTDBHeader header; 
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep;
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char* buf, size_t n)
// sock the connection socket handle we use this revall is because when we use rec it just gives us whats arrived in the current millisecond if out of 24 5 bytes came it will return us this but we still need to havr all 24 bytes 
{
    int32_t total_recieved = 0;
    while (total_recieved < n) {
        size_t bytes_read = recv(sock, buf + total_recieved, static_cast<int>(n - total_recieved), 0);
        // buf+total pointer to next empty spot... n-total is how many bytes we are still waiting for
        if (bytes_read > 0) total_recieved += bytes_read;
        else if (bytes_read == 0) {
            // LOAD balancer closed the connection 
            return false;
        }
        else if (bytes_read < 0) {
            // socket error occured 
            return false;
        }
        if (total_recieved == n)return true;// we got all bytes
    }
    return true;
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.
}
bool sendAll(int sock, const char* buf, size_t n)
{
    int32_t totalsent = 0;
    while (totalsent < n) {
        size_t bytes = send(sock, buf + totalsent, n - totalsent, MSG_NOSIGNAL);
        if (bytes <= 0)return false;// connection closed or error 
        if (bytes > 0)totalsent += bytes;
        if (totalsent == n)return true;
    }
    return true;
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
}
// connect client to loadbalanv=cer using its ip and port number
int connectToHost(const char* host, int port)
{
    if (!initSockets())return -1; // to check for windows network initialization
    // int sock is an integer value which the operator system assigns to yout TCP socket 
    int sock = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0)); // AF_inet tells the OS to use ipv4 address, SOCKSTZREAM specifies reliable TCP connection 
    // 0 means let OS chose default protocol for TCP
    if (sock <0)return -1;// socket creation fails 
    struct sockaddr_in addr; // struct for storing ipv4 address and port number 
    memset(&addr, 0, sizeof(addr)); // wipes memort blovk of addr with zeros to ensure  no garbage memory interfare with address configuration
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port); // Convert port number to network byte order
    if (inet_pton(AF_INET, host, &addr.sin_addr)<=0) { // convert and check if ip is valid or not malformed
        close_socket(sock);
        return -1;
    }// convert ip string into binary
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {// sends TCP SYN packet to initiate 3 way tcp connection 
        close(sock); // Clean up socket descriptor if connection fails
        return -1;
    }
    return sock;
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
}
// reads chunk from compute checksum and send the data accross sock
bool uploadSource(int sock, const char* sourcePath)
{
    ifstream source(sourcePath, ios::binary);
    if (!source.is_open()) return false;

    char buffer[IO_BUFFER_SIZE];
    uint32_t checksum = 0; // Initialize rolling CRC-32 checksum

    while (source.read(buffer, IO_BUFFER_SIZE) || source.gcount() > 0)
    {
        streamsize bytesRead = source.gcount();
        if (bytesRead <= 0) break;

        // 1. Calculate rolling checksum on chunk
        checksum = update_crc32(checksum, buffer, static_cast<size_t>(bytesRead));

        // 2. Send chunk over socket
        if (!sendAll(sock, buffer, static_cast<size_t>(bytesRead)))
        {
            source.close();
            return false;
        }
    }

    source.close();

    // 3. Append 4-byte CRC32 Checksum Trailer at end of file stream
    uint32_t netChecksum = htonl(checksum);
    if (!sendAll(sock, reinterpret_cast<const char*>(&netChecksum), sizeof(netChecksum)))
    {
        return false;
    }

    // 4. Half-close socket to signal upload completion to Load Balancer
    shutdown(sock, SHUT_WR);

    return true;
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
}
bool receiveResponse(int sock, const char* outTdbgPath, string& outError)
{
    char statusByte = 0;
    if (!recvAll(sock, &statusByte, 1))
    {
        outError = "Failed to receive response status from server.";
        return false;
    }

    uint8_t status = static_cast<uint8_t>(statusByte);

    // Initialize rolling CRC32 calculation starting with the status byte
    uint32_t computedCrc = 0;
    computedCrc = update_crc32(computedCrc, &statusByte, 1);

    // Open output file if Status == 0 (Success)
    ofstream outFile;
    if (status == 0)
    {
        outFile.open(outTdbgPath, ios::binary);
        if (!outFile.is_open())
        {
            outError = "Cannot open output file to save .tdbg response.";
            return false;
        }
    }

    // 2. Sliding Window Ring Buffer to hold back the last 4 bytes (CRC-32 trailer)
    char window[4];
    size_t windowCount = 0;
    string errorPayload = "";

    char inputBuf[IO_BUFFER_SIZE];
    ssize_t bytesRead = 0;

    // Loop until the remote end closes the socket connection (EOF)
    while ((bytesRead = recv(sock, inputBuf, IO_BUFFER_SIZE, 0)) > 0)
    {
        for (ssize_t i = 0; i < bytesRead; ++i)
        {
            char currentByte = inputBuf[i];

            if (windowCount < 4)
            {
                // Fill initial window buffer
                window[windowCount++] = currentByte;
            }
            else
            {
                // Shift out the oldest byte from the window
                char byteToWrite = window[0];
                window[0] = window[1];
                window[1] = window[2];
                window[2] = window[3];
                window[3] = currentByte;

                // Process the byte pushed out of the 4-byte trailer window
                computedCrc = update_crc32(computedCrc, &byteToWrite, 1);

                if (status == 0)
                {
                    outFile.put(byteToWrite);
                }
                else
                {
                    errorPayload += byteToWrite;
                }
            }
        }
    }

    if (bytesRead < 0)
    {
        if (outFile.is_open())
        {
            outFile.close();
            remove(outTdbgPath);
        }
        outError = "Socket error occurred while receiving payload stream.";
        return false;
    }

    // Must have received at least the 4-byte CRC-32 trailer
    if (windowCount < 4)
    {
        if (outFile.is_open())
        {
            outFile.close();
            remove(outTdbgPath);
        }
        outError = "Truncated response stream: missing checksum trailer.";
        return false;
    }

    // 3. Extract received CRC-32 checksum trailer from window
    uint32_t receivedCrc = 0;
    memcpy(&receivedCrc, window, 4);
    receivedCrc = ntohl(receivedCrc);

    // 4. Verify Checksum Integrity
    if (computedCrc != receivedCrc)
    {
        if (outFile.is_open())
        {
            outFile.close();
            remove(outTdbgPath); // Delete corrupted payload from disk
        }
        outError = "checksum mismatch, response corrupted";
        return false;
    }

    // Checksum verified successfully
    if (status == 0)
    {
        outFile.close();
        return true;
    }
    else
    {
        outError = errorPayload;
        return false;
    }
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
    FILE* f = fopen(path, "rb");
    if (!f)return false;
    // read 24 byte TTDB header 
    if (fread(&r.header, sizeof(TTDBHEADER), 1, f) != 1) {
        fclose(f);
        return false;
    }
    // verifying header 
    if (r.header.magic[0] != 'T' || r.header.magic[1] != 'T' || r.header.magic[2] != 'D' || r.header.magic[3] != 'B') {
        fclose(f);
        return false;
    }
    // seeking into dense index trailer
    if (fseek(f, r.header.indexOffset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    // allocate size and store it in r,index
    r.index.resize(r.header.stepCount);
    if (r.header.stepCount > 0)
    {
        size_t readCount = fread(r.index.data(), sizeof(int64_t), r.header.stepCount, f);
        if (readCount != static_cast<size_t>(r.header.stepCount))
        {
            r.index.clear();
            fclose(f);
            return false;
        }
    }
    r.file_index = reinterpret_cast<ssize_t>(f);
    return true;
}
void closeTdbg(TdbgReader& r)
{
    // closing file 
    if (r.file_index != 0 && r.file_index != -1)
    {
        FILE* f = reinterpret_cast<FILE*>(r.file_index);
        fclose(f);
        r.file_index = -1;
    }
    r.index.clear();
    r.header.stepCount = 0;
    r.header.indexOffset = 0;
    r.header.version = 0;
    r.header.magic[0] = '\0'
    // Free r.index and close the file.
}
// uses the step index TO GO DIRECTLY TO Exact byte location 
Snapshot* readSnapshotAt(TdbgReader& r, int32_t step)
{
    if (step<0 and step>=r.header.stepCount)return nullptr; // out of bounds check
    // jump to target offset
    if (r.file_index == 0 || r.file_index == -1)
    {
        return nullptr;
    }
    // seeking to the exact byte position 
    FILE* f = reinterpret_cast<FILE*>(r.file_index);
    int64_t targetByteOffset = r.index[step];
    if (fseeko(f, targetByteOffset, SEEK_SET) != 0)
    {
        return nullptr;
    }
    Snapshot* s = new Snapshot();
    if (fread(&(s->stackDepth), sizeof(int32_t), 1, f) != 1)
    {
        delete s;
        return nullptr;
    }

    // each frame in stack
    for (int32_t i = 0; i < s->stackDepth; i++)
    {
        Frame& frame = s->callStack[i];

        // reading function name 
        int32_t nameLen = 0;
        if (fread(&nameLen, sizeof(int32_t), 1, f) != 1) { delete s; return nullptr; }
        if (nameLen > 0)
        {
            vector<char> nameBuf(nameLen + 1, '\0');
            fread(nameBuf.data(), sizeof(char), nameLen, f);
            frame.func_name = string(nameBuf.data());
        }
        else
        {
            frame.func_name = "";
        }

        //  read return Line
        if (fread(&(frame.returnLine), sizeof(int32_t), 1, f) != 1) { delete s; return nullptr; }

        //  read positional arguments 
        if (fread(&(frame.argc), sizeof(int32_t), 1, f) != 1) { delete s; return nullptr; }
        for (int32_t a = 0; a < frame.argc; a++)
        {
            int32_t argNameLen = 0;
            if (fread(&argNameLen, sizeof(int32_t), 1, f) != 1) { delete s; return nullptr; }

            if (argNameLen > 0)
            {
                vector<char> argBuf(argNameLen + 1, '\0');
                fread(argBuf.data(), sizeof(char), argNameLen, f);
                frame.argv[a].name = string(argBuf.data());
            }
            else
            {
                frame.argv[a].name = "";
            }

            if (fread(&(frame.argv[a].value), sizeof(int32_t), 1, f) != 1) { delete s; return nullptr; }
        }

        // Read Local Variables
        if (fread(&(frame.localCount), sizeof(int32_t), 1, f) != 1) { delete s; return nullptr; }
        for (int32_t v = 0; v < frame.localCount; v++)
        {
            int32_t varNameLen = 0;
            if (fread(&varNameLen, sizeof(int32_t), 1, f) != 1) { delete s; return nullptr; }

            if (varNameLen > 0)
            {
                vector<char> varBuf(varNameLen + 1, '\0');
                fread(varBuf.data(), sizeof(char), varNameLen, f);
                frame.locals[v].name = string(varBuf.data());
            }
            else
            {
                frame.locals[v].name = "";
            }

            if (fread(&(frame.locals[v].value), sizeof(int32_t), 1, f) != 1) { delete s; return nullptr; }
        }
    }

    return s;


}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================
//sets up a fresh, clean, empty session when the client program starts
void initSession(DebugSession& s)
{
    s.active_flag = 0;
    s.currentStep = 0;
    s.reader.file_index = -1;
    s.reader.index.clear();
    s.reader.header.indexOffset = 0;
    s.reader.header.stepCount = 0;
    s.reader.header.magic[0] = '\0';
    s.reader.header.version = 0;
}
void closeSession(DebugSession& s)
{
    if (s.active_flag) {
        closeTdbg(s.reader);
        s.active_flag = 0;
    }
    s.currentStep = 0;

    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s); // defined in Part E

bool showCurrent(DebugSession& s)
{
    Snapshot *S1 = readSnapshotAt(s.reader, s.currentStep);
    if (!S1) {
        cout << "\n The file is damaged";
        return false;
    }
    else {
        printSnapshot(s.currentStep + 1, s.reader.header.stepCount, *S1);
        delete S1;
        return true;
    }
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
}
bool moveBy(DebugSession& s, int32_t delta)
{
    if (!s.active_flag) {
        cout << "\n No active debug session . Use 'start <source.bin>' first ";
        return false;
    }
    int64_t targetStep = s.currentStep + delta;
    if (targetStep < 0)s.currentStep = 0;
    else if (targetStep >= s.reader.header.stepCount) s.currentStep = s.reader.header.stepCount - 1;
    else s.currentStep = targetStep;
    showCurrent(s);
    return true;
}
//establishes a new debugging session from a source code binary file while protecting your active session if anything fails.
bool cmdStart(DebugSession& s, const string& sourcePath)
{
    string errorMsg;

    // Request .tdbg file from Load Balancer into TDBG_FILE
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(), TDBG_FILE, errorMsg))
    {
        // Network / Checksum failure: print error and return false. Keep old session untouched.
        cout << errorMsg << endl;
        return false;
    }

    //Open downloaded file into a TEMPORARY local TdbgReader first
    TdbgReader tempReader;
    tempReader.file_index = -1;
    tempReader.header.stepCount = 0;
    tempReader.header.indexOffset = 0;

    if (!openTdbg(TDBG_FILE, tempReader))
    {
        // Headers or index corrupted: print exact error, return false, preserve old session.
        cout << "server sent a damaged .tdbg file" << endl;
        return false;
    }

    //  Verification passed: Close old session resources safely if currently active
    if (s.active_flag)
    {
        closeSession(s);
    }

    //Swap temporary reader into s.reader
    s.reader = tempReader;
    s.active_flag = 1;
    s.currentStep = 0;

    // Render Step 1
    return showCurrent(s);
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
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
    if (n <= 0 || n > MAX_JUMP)
    {
        cout << "invalid jump count" << endl;
        return false;
    }
    return moveBy(s, n);
}

bool cmdJmpPrv(DebugSession& s, int32_t n)
{
    if (n <= 0 || n > MAX_JUMP)
    {
        cout << "invalid jump count" << endl;
        return false;
    }
    return moveBy(s, -n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s)
{
    cout << "---Step " << step << " / " << totalSteps << "---" << endl;

    if (s.stackDepth <= 0)
    {
        cout << "  (Empty stack)" << endl;
        return;
    }

    // Display top of stack (recent call) down to bottom (main)
    for (int32_t i = s.stackDepth - 1; i >= 0; --i)
    {
        const Frame& frame = s.callStack[i];

        cout << "[" << i << "] Frame: " << frame.func_name << "(";

        // Printing arguments
        for (int32_t a = 0; a < frame.argc; ++a)
        {
            cout << frame.argv[a].name << "=" << frame.argv[a].value;
            if (a + 1 < frame.argc) cout << ", ";
        }
        cout << ")";

        if (frame.returnLine >= 0)
        {
            cout << " [ret: line " << frame.returnLine << "]";
        }
        cout << endl;

        // Printing local variables
        if (frame.localCount > 0)
        {
            cout << "    Locals: ";
            for (int32_t v = 0; v < frame.localCount; ++v)
            {
                cout << frame.locals[v].name << "=" << frame.locals[v].value;
                if (v + 1 < frame.localCount) cout << ", ";
            }
            cout << endl;
        }
    }
}
void printHelp()
{
    cout << "Available commands:" << endl;
    cout << "  start <source.bin>   - Upload source code to load balancer and initialize trace session" << endl;
    cout << "  next                 - Move forward 1 step" << endl;
    cout << "  prv                  - Move backward 1 step" << endl;
    cout << "  jmp next <n>         - Jump forward n steps" << endl;
    cout << "  jmp prv <n>          - Jump backward n steps" << endl;
    cout << "  help                 - Display command help" << endl;
    cout << "  quit / exit          - Exit the time-travel debugger" << endl;
    // Print the commands
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession& s, const string& line, bool& quitRequested)
{
    // 1. Split line into words
    vector<string> words;
    size_t start = 0;
    while (start < line.length())
    {
        size_t first = line.find_first_not_of(" \t\r\n", start);
        if (first == string::npos) break;
        size_t last = line.find_first_of(" \t\r\n", first);
        if (last == string::npos)
        {
            words.push_back(line.substr(first));
            break;
        }
        words.push_back(line.substr(first, last - first));
        start = last;
    }

    // Ignore empty lines
    if (words.empty()) return true;

    // 2. Drop "ttdb" prefix if user types "ttdb start ..."
    size_t idx = 0;
    if (words[0] == "ttdb")
    {
        idx = 1;
        if (words.size() <= 1) return true;
    }

    string cmd = words[idx];

    // 3. Command Matching & Dispatch
    if (cmd == "start")
    {
        if (idx + 1 >= words.size())
        {
            cout << "usage: start <source.bin>" << endl;
            return false;
        }
        return cmdStart(s, words[idx + 1]);
    }
    else if (cmd == "next")
    {
        if (words.size() - idx != 1)
        {
            cout << "unknown command" << endl;
            printHelp();
            return false;
        }
        return cmdNext(s);
    }
    else if (cmd == "prv")
    {
        if (words.size() - idx != 1)
        {
            cout << "unknown command" << endl;
            printHelp();
            return false;
        }
        return cmdPrv(s);
    }
    else if (cmd == "jmp")
    {
        if (words.size() - idx != 3)
        {
            cout << "usage: jmp next <n> OR jmp prv <n>" << endl;
            return false;
        }

        string direction = words[idx + 1];
        int32_t n = 0;
        try
        {
            n = stoi(words[idx + 2]);
        }
        catch (...)
        {
            cout << "invalid jump count" << endl;
            return false;
        }

        if (direction == "next")
        {
            return cmdJmpNext(s, n);
        }
        else if (direction == "prv")
        {
            return cmdJmpPrv(s, n);
        }
        else
        {
            cout << "usage: jmp next <n> OR jmp prv <n>" << endl;
            return false;
        }
    }
    else if (cmd == "help")
    {
        printHelp();
        return true;
    }
    else if (cmd == "quit" || cmd == "exit")
    {
        quitRequested = true;
        return true;
    }
    else
    {
        cout << "unknown command" << endl;
        printHelp();
        return false;
    }
}
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
int main(int argc, char* argv[])
{
    if (argc >= 2)
    {
        LB_HOST = argv[1];
    }
    if (argc >= 3)
    {
        LB_PORT = atoi(argv[2]);
    }

    DebugSession session;
    initSession(session);

    cout << "Welcome to " << PROGRAM_NAME << " (Time-Travel Debugger)" << endl;
    printHelp();
    cout << endl;

    bool quitRequested = false;
    string inputLine;

    // CLI Loop
    while (!quitRequested)
    {
        cout << PROMPT;
        if (!getline(cin, inputLine))
        {
            // End of input stream (EOF / Ctrl+D)
            break;
        }   
        runCommand(session, inputLine, quitRequested);
    }

    // Clean up session resources before exiting
    closeSession(session);
    cleanupSockets();

    return 0;
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
