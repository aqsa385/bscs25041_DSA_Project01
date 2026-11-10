#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <signal.h>
#include "helper.h"
using namespace std;

const char* SERVER_PATH = ".\\server.exe";  

const int LISTEN_BACKLOG = 16;

const int MAX_PENDING_CLIENTS = 8;
const int STARTUP_MAX_ATTEMPTS = 50;

const int STARTUP_POLL_INTERVAL_MS = 100;

int lbPort = 5000;

string serverHost = "127.0.0.1";
int serverPort = 5001;                      

volatile sig_atomic_t flag = 1;

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

bool forwardMessage(int fromSock, int toSock)
{
    TrailerChecker tc;
    char buf[IO_BUFFER_SIZE];

    char status;
    if (recv(fromSock, &status, 1, 0) <= 0)
    {
        return false;
    }

    tc.addStatus(status);
    if (!sendAll(toSock, &status, 1))
    {
        return false;
    }

    while (true)
    {
        int got = recv(fromSock, buf, IO_BUFFER_SIZE, 0);
        if (got == 0)
        {
            break;             
        }
        if (got < 0)
        {
            return false;     
        }

        string body = tc.feed(buf, got);      
        if (body.size() > 0)
        {
            if (!sendAll(toSock, body.c_str(), body.size()))
            {
                return false;
            }
        }
    }

    if (!tc.verify())
    {
        return false;
    }
    uint32_t crc = tc.currentCrc();
    return sendAll(toSock, (const char*)&crc, 4);
}

void relayOneClient(ClientInfo& c)
{
    int serverSock = connectToServerWithWakeup();

    if (serverSock == -1)
    {
        sendError(c.sock, "backend server faiiled to start");

        closeSocket(c.sock);
        return;
    }

    bool uploadOk = forwardMessage(c.sock, serverSock);

    shutdown(serverSock, SHUT_WR);

    if (!uploadOk)
    {
        sendError(c.sock, "checksum mismatch, upload corupted");
        closeSocket(serverSock);

        closeSocket(c.sock);
        return;
    }

    forwardMessage(serverSock, c.sock);

    closeSocket(serverSock);

    closeSocket(c.sock);
}

void acceptLoop(int listenSock)
{
    while (flag)
    {
        sockaddr_in addr;
        int len = sizeof(addr);

        SOCKET s = accept(listenSock, (sockaddr*)&addr, &len);

        if (s == INVALID_SOCKET)
        {
             cout << "accept() failed: " << WSAGetLastError() << endl;
            sleepMs(100);
            continue;
        }

        ClientInfo c;

        c.sock = (int)s;

        makeNotInheritable(c.sock);

        char ip[INET_ADDRSTRLEN];

        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        c.address = ip;

        c.port = ntohs(addr.sin_port);

        c.arrivedAt = time(nullptr);

        queueMutex.lock();

        bool full = (pendingClients.size() >= MAX_PENDING_CLIENTS);

        if (!full)
        {
            pendingClients.enqueue(c);
        }
        queueMutex.unlock();

        if (full)
        {
            sendError(c.sock, "server busy, try again later");
            closeSocket(c.sock);       
        }
    }
}
void dispatcherLoop()
{
    while (flag)
    {
        bool gotClient = false;
        ClientInfo c;

        queueMutex.lock();
        if (!pendingClients.isEmpty())
        {
            c = pendingClients.dequeue();
            gotClient = true;
        }
        queueMutex.unlock();

        if (!gotClient)
        {
            sleepMs(50);        
            continue;
        }

        if (time(nullptr) - c.arrivedAt > CLIENT_TIMEOUT_SEC)
        {
            closeSocket(c.sock);
            continue;
        }

        relayOneClient(c);      
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char* argv[])
{
    initNetwork();
    signal(SIGINT, handler);

    if (argc >= 3)
    {
        serverHost = argv[1];
        serverPort = atoi(argv[2]);
    }

    int listenSock = createListenSocket(lbPort);

    thread acceptor(acceptLoop, listenSock);
    acceptor.detach();

    dispatcherLoop();

    closeSocket(listenSock);
    WSACleanup();
    return 0;
    
}