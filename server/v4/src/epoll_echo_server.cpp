#include <sys/socket.h>
#include <epoll_echo_server.h>
#include <netinet/in.h>
#include <unistd.h>
#include <iostream>

#ifdef __linux__
    #include <sys/epoll.h>
    #include <sys/eventfd.h>
#else
    #warning "epoll is Linux-specific; use a Linux target or update include paths."
#endif

constexpr int kBacklog = 128;
constexpr int kMaxEvents = 64;
constexpr int TIME_OUT = 30000;

using std::cout;
using std::endl;

EpollEchoServer::EpollEchoServer(int port){
    this->server_port = port;
}

EpollEchoServer::~EpollEchoServer(){
    this->Stop();
    this->CleanUp();
}

void EpollEchoServer::Server_Run(){
    //  socket -> bind -> listen -> accept
    cout << "[EpollEchoServer] Server starting on port " << this->server_port << endl;

    int socket_result = socket(AF_INET, SOCK_STREAM, 0);
    if(socket_result < 0){
        perror("socket_result < 0");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(this->queue_mutex);
        if(this->stopping){
            close(socket_result);
            return;
        }
        
        this->listen_fd = socket_result;
    }

    int reuse_addr = 1;
    int option_result = setsockopt(
        this->listen_fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse_addr,
        sizeof(reuse_addr)
    );

    if(option_result < 0){
        perror("option_result < 0");
        this->Stop();
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(this->server_port);

    cout << "[EpollEchoServer] Binding port " << this->server_port
         << " with listen_fd " << this->listen_fd << endl;
    int bind_result = bind(this->listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if(bind_result < 0){
        perror("bind_result < 0");
        this->Stop();
        return;
    }

    cout << "[EpollEchoServer] Listening on port " << this->server_port
         << " with listen_fd " << this->listen_fd << endl;
    int listen_result = listen(this->listen_fd, kBacklog);
    if(listen_result < 0){
        perror("listen_result < 0");
        this->Stop();
        return;
    }

    cout << "[EpollEchoServer] Epoll instance creating... " << endl;
    int epoll_result = epoll_create1(0);
    if(epoll_result < 0){
        perror("epoll_result < 0");
        this->Stop();
        return;
    }

    this->epoll_fd = epoll_result;

    epoll_event listen_event{};
    listen_event.events = EPOLLIN;
    listen_event.data.fd = this->listen_fd;
    int listen_ctl = epoll_ctl(this->epoll_fd, EPOLL_CTL_ADD, this->listen_fd, &listen_event);
    if(listen_ctl < 0){
        perror("listen_ctl < 0");
        this->Stop();
        return;
    }

    cout << "[EpollEchoServer] Stop event fd creating... " << endl;
    int stop_fd_result = eventfd(0, EFD_NONBLOCK);
    if(stop_fd_result < 0){
        perror("stop_fd_result < 0");
        this->Stop();
        return;
    }

    this->stop_event_fd = stop_fd_result;

    epoll_event stop_event{};
    stop_event.events = EPOLLIN;
    stop_event.data.fd = this->stop_event_fd;
    int stop_ctl = epoll_ctl(this->epoll_fd, EPOLL_CTL_ADD, this->stop_event_fd, &stop_event);
    if(stop_ctl < 0){
        perror("stop_ctl < 0");
        this->Stop();
        return;
    }
    cout << "[EpollEchoServer] Stop event fd registered: " << this->stop_event_fd << endl;

    epoll_event wait_events[kMaxEvents]{};

    while(!this->stopping){
        int wait_result = epoll_wait(
            this->epoll_fd,
            wait_events,
            kMaxEvents,
            TIME_OUT
        );

        if(wait_result < 0){
            perror("wait_result < 0");
            this->Stop();
            return;
        }

        for(int i=0; i<wait_result; i++){
            int event_fd = wait_events[i].data.fd;
            cout << "[EpollEchoServer] Event ready index=" << i
                 << " fd=" << event_fd << endl;

            if(event_fd == this->stop_event_fd){
                cout << "[EpollEchoServer] Stop event received on fd " << event_fd << endl;
                this->stopping = true;
                
                uint64_t stop_value = 1;
                ssize_t read_result = read(this->stop_event_fd, &stop_value, sizeof(stop_value));
                if(read_result < 0){
                    perror("read_result < 0");
                }

                break;
            }
            else if(event_fd == this->listen_fd){
                cout << "[EpollEchoServer] Accept event on listen_fd " << this->listen_fd << endl;

                sockaddr_in client_addr{};
                socklen_t client_len = sizeof(client_addr);
                int accept_result = accept(event_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
                if(accept_result < 0){
                    perror("accept_result < 0");
                    continue;
                }
                cout << "[EpollEchoServer] Client accepted: " << accept_result << endl;
                
                epoll_event event_epoll{};
                event_epoll.events = EPOLLIN;
                event_epoll.data.fd = accept_result;
                
                int add_ctl = epoll_ctl(this->epoll_fd, EPOLL_CTL_ADD, accept_result, &event_epoll);
                if(add_ctl < 0){
                    perror("event add_ctl < 0");
                    close(accept_result);
                    continue;
                }
                cout << "[EpollEchoServer] Client registered to epoll: " << accept_result << endl;
                
                {
                    std::lock_guard<std::mutex> lock(this->queue_mutex);
                    if(this->client_fds.find(accept_result) != this->client_fds.end()){
                        cout << "[EpollEchoServer] Client already tracked: " << accept_result << endl;
                    }else{
                        cout << "[EpollEchoServer] Client tracked: " << accept_result << endl;
                        this->client_fds.insert(accept_result);
                    }
                }
            }
            else{
                cout << "[EpollEchoServer] Client read event: " << event_fd << endl;

                char buffer[4096]{};
                int recv_result = recv(event_fd, buffer, sizeof(buffer), 0);
                if(recv_result < 0){
                    perror("recv_result < 0");
                    this->DeleteClientFd(event_fd);
                    continue;
                }
                else if(recv_result == 0){
                    cout << "[EpollEchoServer] Client disconnected: " << event_fd << endl;
                    this->DeleteClientFd(event_fd);
                    continue;
                }
                else{
                    cout << "[EpollEchoServer] Received " << recv_result
                         << " bytes from client " << event_fd << endl;
                    bool flag = SendAll(event_fd, buffer, recv_result);
                    if(!flag){
                        this->DeleteClientFd(event_fd);
                        continue;
                    }
                }
            }
        }
    }

    cout << "[EpollEchoServer] Event loop stopped" << endl;
    this->CleanUp();
}

void EpollEchoServer::Stop(){
    int notify_fd = -1;

    {
        std::lock_guard<std::mutex> lock(this->queue_mutex); 
        if(this->stopping) return;

        cout << "[EpollEchoServer] Stop requested" << endl;
        this->stopping = true;
        notify_fd = this->stop_event_fd;
    }

    if(notify_fd >= 0){
        uint64_t stop_value = 1;
        ssize_t write_result = write(notify_fd, &stop_value, sizeof(stop_value));
        if(write_result < 0){
            perror("write_result < 0");
        }else{
            cout << "[EpollEchoServer] Stop event notified" << endl;
        }
    }
}

void EpollEchoServer::CleanUp(){
    {
        std::lock_guard<std::mutex> lock(this->queue_mutex);
        if(this->cleaned_up) return;
        
        this->cleaned_up = true;

        cout << "[EpollEchoServer] Cleanup started" << endl;
        if(this->stop_event_fd >= 0){
            close(this->stop_event_fd);
            this->stop_event_fd = -1;
        }
    
        for(int client_fd : this->client_fds){
            if(client_fd >= 0)
                close(client_fd);
        }
        this->client_fds.clear();
    
        if(this->listen_fd >= 0){
            close(this->listen_fd);
            this->listen_fd = -1;
        }
    
        if(this->epoll_fd >= 0){
            close(this->epoll_fd);
            this->epoll_fd = -1;
        }

        cout << "[EpollEchoServer] Cleanup finished" << endl;
    }
}

void EpollEchoServer::DeleteClientFd(int client_fd){
    if(client_fd >= 0){
        {
            std::lock_guard<std::mutex> lock(this->queue_mutex);
            if(this->client_fds.find(client_fd) != this->client_fds.end()){
                cout << "[EpollEchoServer] Client closed: " << client_fd << endl;
                this->client_fds.erase(client_fd);
                
                int del_ctl = epoll_ctl(this->epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                if(del_ctl < 0){
                    perror("event del del_ctl < 0");
                }
            
                close(client_fd);
            }else{
                cout << "[EpollEchoServer] Client not found while closing: " << client_fd << endl;
            }
        }
    }
}

bool EpollEchoServer::SendAll(int client_fd, const char* data, std::size_t length){
    // 지금까지 실제로 전송한 바이트 수. length에 도달하면 전체 전송이 완료된 것이다.
    std::size_t total_sent = 0;

    while(total_sent < length){
        // partial send 이후에는 이미 보낸 범위를 건너뛰고 남은 주소와 길이만 다시 전달한다.
        const char* start_data = data + total_sent;
        std::size_t send_len = length - total_sent;

        cout << "[EpollEchoServer] Sending to client " << client_fd << ": ";
        cout.write(start_data, send_len);
        cout << endl;

        ssize_t send_result = send(client_fd, start_data, send_len, 0);
        if(send_result > 0){
            total_sent += send_result;
        }
        else if(send_result < 0 && errno == EINTR){
            // 시그널로 중단된 경우에는 전송량을 변경하지 않고 같은 남은 범위를 다시 시도한다.
            continue;
        }
        else{
            perror("send_result <= 0");
            return false;
        }
    }

    return true;
}
