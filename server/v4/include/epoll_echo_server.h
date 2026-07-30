#pragma once

#include <string>
#include <mutex>
#include <unordered_set>

class EpollEchoServer{
    private:
        int server_port;
        int listen_fd = -1;
        int epoll_fd = -1;
        int stop_event_fd = -1;

        std::mutex queue_mutex;
        std::unordered_set<int> client_fds{};
        
        bool stopping = false;
        bool cleaned_up = false;
        void CleanUp();

    public:
        EpollEchoServer(int port);
        ~EpollEchoServer();

        void Server_Run();
        void Stop();
        bool SendAll(int client_fd, const char* data, std::size_t length);
        void DeleteClientFd(int client_fd);
};

struct ClientState{
    std::string send_buffer;
    std::size_t sent_size;
};