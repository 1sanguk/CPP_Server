#include <blocking_echo_server.h>
#include <unistd.h>
#include <cstdio>
#include <netinet/in.h>
#include <sys/socket.h>
#include <iostream>

#include <cerrno>

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

    // 기존에는 socket() 직후 바로 bind()했지만, 서버 종료 후 남아 있는 이전 연결 상태 때문에
    // 같은 주소/포트로 즉시 재실행할 때 bind()가 실패할 수 있었다.
    // SO_REUSEADDR를 bind() 전에 설정해 해당 주소를 안전하게 다시 사용할 수 있도록 한다.
    int reuse_addr = 1;
    int option_result = setsockopt(
        this->fd,
        SOL_SOCKET,         // 소켓 API 공통 옵션 레벨
        SO_REUSEADDR,       // 종료 후 남은 주소를 재사용하도록 허용
        &reuse_addr,        // 1: 옵션 활성화
        sizeof(reuse_addr)  // 커널에 전달하는 옵션 값의 크기
    );

    // setsockopt()도 예외가 아니라 음수 반환값으로 실패를 알린다.
    if(option_result < 0){
        perror("option_result < 0");
        return;
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

    // 4) 접속 대기 상태로 전환. kBacklog는 커널의 연결 대기 큐 크기.
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

            // 기존에는 recv_result <= 0을 한 분기에서 처리했지만, 오류와 정상 종료를 구분하도록 변경했다.
            // recv_result < 0: 오류. EINTR이면 실제 소켓 장애가 아니므로 recv()를 다시 시도하고,
            // 그 밖의 오류면 클라이언트 fd를 닫고 다음 accept로 복귀한다.
            if(recv_result < 0){
                if(errno == EINTR){
                    continue;
                }

                perror("recv_result < 0...");
                close(accept_result); // 이 클라이언트 전용 fd만 닫음. 리스닝 소켓(this->fd)은 절대 여기서 닫지 않음(닫으면 이후 accept가 전부 실패)
                break;
            }
            else if(recv_result == 0){
                // recv_result == 0: 오류가 아니라 클라이언트가 FIN을 보내 정상적으로 연결을 종료한 경우다.
                close(accept_result); // 이 클라이언트 전용 fd만 닫음. 리스닝 소켓(this->fd)은 절대 여기서 닫지 않음(닫으면 이후 accept가 전부 실패)
                break;
            }
            else if(recv_result > 0) {
                // buffer는 널 종료가 보장되지 않으므로 로그도 recv_result 길이만큼만 출력한다.
                cout << "Sending Client Buffer: ";
                cout.write(buffer, recv_result);
                cout << endl;

                // 기존의 단일 send() 호출은 partial send가 발생하면 나머지 데이터가 유실될 수 있었다.
                // Send_All()이 받은 길이를 모두 전송할 때까지 반복하고, 실패하면 연결을 정리한다.
                // 이전 단일 전송 구현(비교용): send(accept_result, buffer, recv_result, 0);
                bool send_flag = this->Send_All(accept_result, buffer, recv_result);
                if(!send_flag){
                    cout << "Sending Client Buffer Failed: " << accept_result << endl;
                    close(accept_result);
                    break;
                }
            }
        }
    }

    cout << "Blocking Echo Server Stopped...." << endl;
}

bool BlockingEchoServer::Send_All(int client_fd, const char* data, std::size_t length){
    // 지금까지 실제로 전송한 바이트 수. length에 도달하면 전체 전송이 완료된 것이다.
    std::size_t total_sent = 0;

    while (total_sent < length){
        // partial send 이후에는 이미 보낸 범위를 건너뛰고 남은 주소와 길이만 다시 전달한다.
        const char* start_data = data + total_sent;
        std::size_t send_len = length - total_sent;

        cout << "Blocking Echo Server Sending: " << client_fd << "-> ";
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
            perror("send_result > 0");
            return false;
        }
    }

    return true;
}
