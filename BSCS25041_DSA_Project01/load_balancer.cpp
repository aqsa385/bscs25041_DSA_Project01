#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <signal.h>
#include "helper.h"
using namespace std;

const char* SERVER_PATH = "./server";      // the program we start when the server is down
const int IO_BUFFER_SIZE = 64 * 1024;      // fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.0";
int serverPort = 5001;

template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* front, * rear;
    int count;


public:
    
    Queue()
    {
        front = nullptr;
        rear = nullptr;
        count = 0;
    }
    ~Queue()
    {
        while (front != nullptr)
        {
            Node* temp = front;
            front = front->next;
            delete temp;
        }
    }
    void enqueue(const T& val)
    {
        Node* n = new Node;
        n->data = val;
        n->next = nullptr;

        if (rear == nullptr)      
        {
            front = n;
            rear = n;
        }
        else
        {
            rear->next = n;       
            rear = n;
        }
        count++;
    }
    T dequeue()
    {
        if (front == nullptr)
        {
            throw runtime_error("dequeue on empty queue");
        }

        Node* temp = front;
        T val = temp->data;
        front = front->next;

        if (front == nullptr)     
        {
            rear = nullptr;
        }

        delete temp;
        count--;
        return val;
    }
    bool isEmpty()
    {
        return front == nullptr;
    }
    int size()
    {
        return count;
    }
};

struct ClientInfo
{
    int sock;           
    string address;     
    int port;           
    time_t arrivedAt;   
};

Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


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

bool sendError(int sock, const string& message)
{
    char status = 1;

    uint32_t crc = 0;
    crc = crc32Update(crc, &status, 1);

    crc = crc32Update(crc, message.c_str(), message.size());

    if (!sendAll(sock, &status, 1))
    {
        return false;
    }

    if (!sendAll(sock, message.c_str(), message.size()))
    {
        return false;
    }

    if (!sendAll(sock, (const char*)&crc, 4))
    {
        return false;
    }

    return true;
}
int createListenSocket(int port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
    {
        cout << "socket() failed: " << WSAGetLastError() << endl;
        exit(1);
    }
    int sock = (int)s;
    makeNotInheritable(sock);

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;

    addr.sin_addr.s_addr = htonl(INADDR_ANY);   

    addr.sin_port = htons(port);

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        cout << "bind() failed: " << WSAGetLastError() << endl;
        exit(1);
    }
    if (listen(sock, LISTEN_BACKLOG) == SOCKET_ERROR)
    {
        cout << "listen() failed: " << WSAGetLastError() << endl;
        exit(1);
    }
    return sock;
}
int connectToServer(const char* host, int port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
    {
        return -1;
    }
    int sock = (int)s;
    makeNotInheritable(sock);

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

bool startServer(int port)
{
    string cmd = string("start /B ") + SERVER_PATH + " " + to_string(port);
    return system(cmd.c_str()) == 0;
}
int connectToServerWithWakeup()
{
    int s = connectToServer(serverHost.c_str(), serverPort);
    if (s != -1)
    {
        return s;
    }

    if (!startServer(serverPort))
    {
        return -1;
    }

    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++)
    {
        sleepMs(STARTUP_POLL_INTERVAL_MS);
        s = connectToServer(serverHost.c_str(), serverPort);
        if (s != -1)
        {
            return s;
        }
    }
    return -1;
}

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