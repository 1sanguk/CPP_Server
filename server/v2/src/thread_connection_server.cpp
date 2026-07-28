#include <iostream>
#include <unistd.h>
#include <cstdio>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <thread_connection_server.h>

#include <cerrno>

using std::cout;
using std::endl;
using std::thread;

constexpr int kBacklog = 128;

// port 값만 멤버에 저장. 소켓은 아직 열지 않음(fd는 헤더에서 -1로 기본 초기화됨).
ThreadPerConnectionEchoServer::ThreadPerConnectionEchoServer(int port){
    this->server_port = port;
}

// 객체 소멸 시 자동 호출. Server_Run()이 실제로 소켓을 열었을 때만(fd >= 0) close.
// 이 fd는 listen 소켓 하나뿐이고, 클라이언트별 fd는 각 스레드가 알아서 닫는다.
ThreadPerConnectionEchoServer::~ThreadPerConnectionEchoServer(){
    if(this->fd >= 0){
        close(this->fd);
    }
}

void ThreadPerConnectionEchoServer::Server_Run(){
    cout << "Thread Per Connection Echo Server Running...." << endl;
    // 1) TCP/IPv4 소켓 생성. 실패 시 -1 리턴.
    int socket_result = socket(AF_INET, SOCK_STREAM, 0);
    if(socket_result < 0){
        perror("socket_result <0");
        return;
    }

    this->fd = socket_result;

    // 서버 종료 후 같은 주소/포트로 즉시 재실행할 수 있도록 bind() 전에 주소 재사용을 허용한다.
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
    cout << "Thread Per Connection Echo Server Binding: " << this->fd << endl;
    int bind_result = bind(this->fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if(bind_result < 0){
        perror("bind_result < 0");
        return;
    }

    // 4) 접속 대기 상태로 전환. kBacklog는 대기 큐 크기.
    cout << "Thread Per Connection Echo Server Listening...." << endl;
    int listen_result = listen(this->fd, kBacklog);
    if(listen_result < 0){
        perror("listen_result < 0");
        return;
    }

    // 5) accept 루프: 클라이언트가 붙을 때마다 그 자리에서 처리하지 않고
    // 별도 스레드에 맡기고 곧바로 다음 accept로 돌아간다 (v1의 blocking 방식과의 핵심 차이).
    while(true){
        cout << "Thread Per Connection Echo Server Waiting Client...." << endl;

        sockaddr_in client_addr{};        // output 전용: accept()가 접속한 클라이언트의 주소를 여기에 채워줌
        socklen_t client_len = sizeof(client_addr);

        // 성공 시 listen 소켓(this->fd)과는 별개인 새 fd를 리턴 (이 커넥션 전용 fd).
        int accept_result = accept(this->fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

        if(accept_result < 0){
            perror("accept_result < 0");
            continue; // 이 접속 시도만 실패한 것 — 서버를 죽이지 않고 다음 accept로 재시도
        }

        // accept 스레드와 client 스레드가 공유하는 목록이므로 등록 과정 전체를 mutex로 보호한다.
        {
            std::lock_guard<std::mutex> lock(this->active_clients_mutex);
            this->active_clients.insert(accept_result);

            cout << "[Active Clients] Client Joined fd=" << accept_result << ", count=" << this->active_clients.size() << endl;
        }

        // 이 클라이언트 전용 fd를 새 스레드에 넘기고 즉시 detach.
        // detach하면 accept 스레드는 기다리지 않고 다음 접속으로 돌아가지만 worker를 join할 수 없다.
        // v2에서는 thread-per-connection 구조와 이 수명 관리 한계를 관찰하기 위해 의도적으로 유지한다.
        thread t(&ThreadPerConnectionEchoServer::Process_Client, this, accept_result);
        t.detach();
    }
}

// 클라이언트 fd 하나를 전담하는 함수. 각자 별도 스레드 위에서 실행되므로
// 여기서 쓰는 buffer는 스레드별 스택 지역 변수라 서로 다른 클라이언트끼리 공유되지 않는다.
void ThreadPerConnectionEchoServer::Process_Client(int client_fd){
    char buffer[4096];
    // 이 클라이언트가 연결을 끊을 때까지 반복되는 echo 루프.
    // v1과 달리 이 루프가 다른 클라이언트의 accept를 막지 않는다(각자 자기 스레드에서 돎).
    while(true){
        ssize_t recv_result = recv(client_fd, buffer, sizeof(buffer), 0);

        // 수신 오류 중 EINTR는 실제 연결 장애가 아니므로 다시 recv()를 시도한다.
        // 그 밖의 오류는 활성 목록에서 제거하고 client fd를 닫은 뒤 worker를 종료한다.
        if(recv_result < 0){
            if(errno == EINTR){
                continue;
            }

            perror("recv_result < 0...");
            // close(client_fd); // 이 클라이언트 전용 fd만 닫음. 리스닝 소켓(this->fd)은 절대 여기서 닫지 않음(닫으면 이후 accept가 전부 실패)
            this->Close_Client(client_fd);
            break;
        }
        else if(recv_result == 0){
            // recv_result == 0: 오류가 아니라 클라이언트가 FIN을 보내 정상적으로 연결을 종료한 경우다.
            // close(client_fd); // 이 클라이언트 전용 fd만 닫음. 리스닝 소켓(this->fd)은 절대 여기서 닫지 않음(닫으면 이후 accept가 전부 실패)
            this->Close_Client(client_fd);
            break;
        }
        else if(recv_result > 0) {
            // buffer는 널 종료가 보장되지 않으므로, 로그 출력용으로는
            // recv_result 길이만큼만 잘라서 std::string으로 감싼다.
            std::string s(buffer, recv_result);
            cout << "Sending " << client_fd << " Client Buffer: " << s << endl;

            // 이전 단일 전송 구현(비교용): send(client_fd, buffer, recv_result, 0);
            // 현재는 partial send에 대비해 Send_All()이 남은 범위를 반복 전송한다.
            bool send_flag = this->Send_All(client_fd, buffer, recv_result);
            if(!send_flag){
                cout << "Sending Client Buffer Failed: " << client_fd << endl;
                this->Close_Client(client_fd);
                break;
            }
        }
    }
}

bool ThreadPerConnectionEchoServer::Send_All(int client_fd, const char* data, std::size_t length){
    // 지금까지 실제로 전송한 바이트 수. length에 도달하면 전체 전송이 완료된 것이다.
    std::size_t total_sent = 0;

    while (total_sent < length){
        // partial send 이후에는 이미 보낸 범위를 건너뛰고 남은 주소와 길이만 다시 전달한다.
        const char* start_data = data + total_sent;
        std::size_t send_len = length - total_sent;

        cout << "Thread Per Connection Echo Server Sending: " << client_fd << "-> ";
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

void ThreadPerConnectionEchoServer::Close_Client(int client_fd){
    // erase와 close를 같은 임계 영역에서 처리해 fd 번호가 재사용되는 사이
    // 이전 worker가 새 연결의 active_clients 기록을 지우는 race를 막는다.
    std::lock_guard<std::mutex> lock(this->active_clients_mutex);
    this->active_clients.erase(client_fd);
    cout << "[Active Clients] Client Leaving fd=" << client_fd << ", count=" << this->active_clients.size() << endl;
    close(client_fd);

}
