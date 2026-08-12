#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <thread>
#include <vector>

class IocpEchoServer{
    private:
        int server_port;
        SOCKET listen_socket = INVALID_SOCKET;
        HANDLE iocp_handle = nullptr;
        std::vector<std::thread> completion_workers;
        std::mutex server_mutex;

        enum class ServerState{
            Creating,
            Running,
            Stopping,
            Cleaning,
            Stopped,
        };

        std::atomic<ServerState> server_state = ServerState::Creating;

        bool winsock_init_state = false;
        bool Init_Winsock();
        bool Create_Listen_Socket();

        bool Create_Completion_Port();
        void Start_Completion_Workers();
        void Accept_Loop();
        bool Register_Client(SOCKET client_socket);
        void Completion_worker_Loop();
        void Close_Client(SOCKET client_socket);
        void Clean_Up();
        void Proceed_Stop();

        const unsigned int worker_count = 4;

    public:
        IocpEchoServer(int port);
        ~IocpEchoServer();

        void Server_Run();
        void Stop();
};