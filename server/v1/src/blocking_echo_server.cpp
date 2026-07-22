#include <blocking_echo_server.h>
#include <unistd.h>
#include <cstdio>
#include <netinet/in.h>
#include <sys/socket.h>
#include <iostream>

using std::cout;
using std::endl;

constexpr int kBacklog = 128;

// port 값만 멤버에 저장. 소켓은 아직 열지 않음(fd는 헤더에서 -1로 기본 초기화됨).
BlockingEchoServer::BlockingEchoServer(int port){
    this->server_port = port;
}

// 객체 소멸 시 자동 호출. Server_Run()이 실제로 소켓을 열었을 때만(fd >= 0) close.
BlockingEchoServer::~BlockingEchoServer(){
    if (this->fd >= 0)
        close(this->fd);
}

void BlockingEchoServer::Server_Run(){
    cout << "Server Running...." << endl;
    // 1) TCP/IPv4 소켓 생성. 실패 시 -1 리턴 (예외가 아니라 리턴값으로 에러를 알려줌).
    int socket_result = socket(AF_INET, SOCK_STREAM, 0);
    if(socket_result < 0){
        perror("socket_result < 0");
        return;
    }else{
        this->fd = socket_result;
    }

    // 2) 이 소켓을 어떤 주소/포트에 묶을지 채움
    sockaddr_in addr{};
    addr.sin_family = AF_INET;         // IPv4
    addr.sin_addr.s_addr = INADDR_ANY; // 모든 로컬 인터페이스에서 접속 허용
    addr.sin_port = htons(this->server_port); // 호스트 바이트오더 -> 네트워크 바이트오더 변환

    // 3) 소켓을 위 주소/포트에 바인딩
    cout << "Server Binding with " << this->fd << endl;
    int bind_result = bind(this->fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if(bind_result < 0){
        perror("bind_result < 0");
        return;
    }

    // 4) 접속 대기 상태로 전환. 16은 대기 큐(backlog) 크기.
    cout << "Listening " << this->fd << " : " << kBacklog << "..." << endl;
    int listen_result = listen(this->fd, kBacklog);
    if(listen_result < 0){
        perror("listen_result < 0");
        return;
    }

    // 5) accept 루프: 클라이언트를 하나씩 받아서 처리
    while(true){
        cout << "Waiting Client's Connection..." << endl;
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int accept_result = accept(this->fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        
        if(accept_result < 0){
            perror("accept_result < 0");
            continue;
        }

        char buffer[4096];
        while(true){
            cout << "Receving Client Buffer..." << endl;
            ssize_t recv_result = recv(accept_result, buffer, sizeof(buffer), 0);

            if(recv_result == 0 || recv_result < 0){
            cout << "Closing Client Connection..." << endl;
                close(accept_result);
                break;
            }
            else if(recv_result > 0) {
                cout << "Sending Client Buffer: " << buffer << endl;
                send(accept_result, buffer, recv_result, 0);
            }
        }
    }
}