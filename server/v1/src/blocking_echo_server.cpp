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
    cout << "Blocking Echo Server Running...." << endl;
    // 1) TCP/IPv4 소켓 생성. 실패 시 -1 리턴 (예외가 아니라 리턴값으로 에러를 알려줌).
    int socket_result = socket(AF_INET, SOCK_STREAM, 0);
    if(socket_result < 0){
        perror("socket_result < 0");
        return;
    }
    
    this->fd = socket_result;

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

    // 5) accept 루프: 클라이언트를 하나씩 받아서 처리 (한 번에 한 커넥션만 — v1은 동시성이 없는 baseline)
    while(true){
        cout << "Waiting Client's Connection..." << endl;
        sockaddr_in client_addr{};       // output 전용: accept()가 접속한 클라이언트의 주소를 여기에 채워줌
        socklen_t client_len = sizeof(client_addr);

        // accept()도 recv()처럼 진짜로 블로킹된다 — 접속이 들어올 때까지 이 스레드는 멈춰있음.
        // 성공 시 리스닝 소켓(this->fd)과는 별개인 새 fd를 리턴 (이 커넥션 전용 fd).
        int accept_result = accept(this->fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

        if(accept_result < 0){
            perror("accept_result < 0");
            continue; // 이 접속 시도만 실패한 것 — 리스닝 소켓 자체는 멀쩡하므로 서버를 죽이지 않고 다음 accept로 재시도
        }

        char buffer[4096];
        // 이 클라이언트가 연결을 끊을 때까지 반복되는 echo 루프. 다음 accept()로 못 돌아가므로
        // 그동안 다른 클라이언트의 접속 요청은 커널 backlog 큐에 쌓이기만 하고 처리되지 않음(blocking의 한계).
        while(true){
            cout << "Receving Client Buffer..." << endl;
            ssize_t recv_result = recv(accept_result, buffer, sizeof(buffer), 0);

            if(recv_result <= 0){
                // recv_result == 0: 클라이언트가 정상적으로 연결을 종료(FIN)했다는 뜻
                // recv_result < 0: 에러(errno) — 두 경우 모두 이 커넥션은 더 이상 쓸 수 없으므로 정리하고 다음 accept로 복귀
                cout << "Closing Client Connection..." << endl;
                close(accept_result); // 이 클라이언트 전용 fd만 닫음. 리스닝 소켓(this->fd)은 절대 여기서 닫지 않음(닫으면 이후 accept가 전부 실패)
                break;
            }
            else if(recv_result > 0) {
                // 받은 바이트 수(recv_result)만큼만 그대로 돌려보냄 (buffer는 널 종료가 보장되지 않으므로 길이를 명시)
                cout << "Sending Client Buffer: " << buffer << endl;
                send(accept_result, buffer, recv_result, 0);
            }
        }
    }

    cout << "Blocking Echo Server Stopped...." << endl;
}