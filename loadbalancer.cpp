#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <signal.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#define close_socket(s) closesocket(s)
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define close_socket(s) close(s)
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

#include "helper.h"
using namespace std;


// ---- Constants ----
#ifdef _WIN32
const char* SERVER_PATH = "server.exe";    // executable name on Windows
#else
const char* SERVER_PATH = "./server";      // the program we start when the server is down
#endif

const int IO_BUFFER_SIZE = 64 * 1024;      // fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.1";           // local loopback address
int serverPort = 5001;

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node* next;
        Node(const T& d) : data(d), next(nullptr) {}
    };
    Node* front;
    Node* rear;
    int count;


public:
    // Implement these functions:
    Queue()
    {
        front = rear = nullptr;
        count = 0;
    }
    ~Queue()
    {
        while (!isEmpty()) {
            dequeue();
        }
    }
    void enqueue(const T& val)
    {
        Node* temp = new Node(val);
        if (isEmpty()) {
            front = rear = temp;
        }
        else {
            rear->next = temp;
            rear = temp;
        }
        count++;
    }
    T dequeue()
    {
        if (isEmpty()) {
            throw underflow_error("Queue is empty");
        }
        Node* temp = front;
        T val = temp->data;

        front = front->next;
        delete temp;
        count--;

        if (front == nullptr) {
            rear = nullptr;
        }
        return val;
    }
    bool isEmpty() const
    {
        return count == 0;
    }
    int size() const
    {
        return count;
    }
};

struct ClientInfo
{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char* buf, size_t n)
{
    size_t totalsent = 0;
    while (totalsent < n) {
        ssize_t bytes = send(sock, buf + totalsent, n - totalsent, MSG_NOSIGNAL);
        if (bytes <= 0) return false; // connection closed or error 
        totalsent += static_cast<size_t>(bytes);
    }
    return true;
    // Reply format: [status: 1 byte][sending_material][checksum: 4 bytes].
    // send() may send fewer bytes than asked for, so keep calling it until all n bytes are sent.
}
bool sendError(int sock, const string& message) // if server queue is full or server is offline loadbalancer sends error msg to client
{
    char statusByte = 1;

    // Compute checksum covering status byte and msg
    uint32_t crc = 0;
    crc = update_crc32(crc, &statusByte, 1);
    crc = update_crc32(crc, message.data(), message.size());

    // assemble the full buffer in memory
    string packet;
    packet.push_back(statusByte);
    packet.append(message);

    // append checksum in Network Byte Order
    uint32_t netCrc = htonl(crc);
    packet.append(reinterpret_cast<const char*>(&netCrc), 4);

    // send the entire binary packet over socket
    return sendAll(sock, packet.data(), packet.size());
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text. 
}
int createListenSocket(int port)
{
    // Create IPv4 TCP socket
    int sock = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (sock < 0)
    {
        cerr << "Failed to create listening socket" << endl;
        exit(1);
    }

    // Set SO_REUSEADDR so port frees up instantly on restart
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt)) < 0)
    {
        cerr << "Failed to set SO_REUSEADDR" << endl;
        close_socket(sock);
        exit(1);
    }

    // Configure socket address structure
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    // Bind socket to port
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        cerr << "Failed to bind socket to port " << port << endl;
        close_socket(sock);
        exit(1);
    }

    // Start listening for incoming connections
    if (listen(sock, LISTEN_BACKLOG) < 0)
    {
        cerr << "Failed to listen on socket" << endl;
        close_socket(sock);
        exit(1);
    }

    return sock;
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.
}
int connectToServer(const char* host, int port)
{
    // creating a socket
    int sock = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (sock < 0) return -1;

    struct sockaddr_in addr; // struct for storing ipv4 address and port number 
    memset(&addr, 0, sizeof(addr)); // wipes memory block of addr with zeros to ensure no garbage memory interferes with address configuration
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port); // Convert port number to network byte order

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) { // convert and check if ip is valid or not malformed
        close_socket(sock);
        return -1;
    }
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { // sends TCP SYN packet to initiate 3 way tcp connection 
        close_socket(sock); // Clean up socket descriptor if connection fails
        return -1;
    }
    return sock;
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails 
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================

// launches the backend server using a system command 
bool startServer(int port)
{
    string command;
#ifdef _WIN32
    command = "start /B " + string(SERVER_PATH) + " " + to_string(port);
#else
     command = string(SERVER_PATH) + " " + to_string(port) + " &";
#endif
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    if (system(command.c_str()) == 0) return true;
    return false;
    // Return true if system() returned 0, otherwise false.
    // & is very important without it the system blocks until the server exits, freezing the entire load balancer. & tells the OS to detach the process into the background 
}
int connectToServerWithWakeup()
{
    int s = connectToServer(serverHost.c_str(), serverPort);
    if (s != -1) return s; // meaning server is already alive 
    if (!startServer(serverPort)) return -1; // server down attempt auto spawn 

    // trying to connect server up to 50 times 
    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; ++i)
    {
        SLEEP_MS(STARTUP_POLL_INTERVAL_MS); // 100 ms sleep
        int sock = connectToServer(serverHost.c_str(), serverPort);
        if (sock != -1) return sock;
    }

    // Server failed to open port within 5 seconds
    return -1;
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times 
    // 4. Return the first working socket, or -1 if the server never came up.
}

// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


void relayOneClient(ClientInfo& c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.
}


// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).
}
void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char* argv[])
{

    signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
    // 2. listenSock = createListenSocket(LB_PORT).
    // 3. Start a thread that runs acceptLoop(listenSock) and detach it.
    // 4. Call dispatcherLoop().
    return 0;
}