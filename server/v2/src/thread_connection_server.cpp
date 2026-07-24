#include <iostream>
#include <unistd.h>
#include <cstdio>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <thread_connection_server.h>

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

        cout << accept_result << " Client Joined...." << endl;

        // 이 클라이언트 전용 fd를 새 스레드에 넘기고 즉시 detach.
        // detach하면 메인 스레드는 이 스레드의 join을 기다리지 않고 바로 다음 accept로 넘어간다.
        // (스레드 수명/종료 관리를 포기하는 대신 구현이 단순해짐 — 트레이드오프)
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

        if(recv_result <= 0){
            // recv_result == 0: 클라이언트가 정상적으로 연결을 종료(FIN)했다는 뜻
            // recv_result < 0: 에러(errno) — 두 경우 모두 이 fd를 닫고 스레드를 종료한다.
            cout << "Closing " << client_fd << " Client Connection..." << endl;
            close(client_fd);
            break;
        }
        else if(recv_result > 0) {
            // buffer는 널 종료가 보장되지 않으므로, 로그 출력용으로는
            // recv_result 길이만큼만 잘라서 std::string으로 감싼다.
            std::string s(buffer, recv_result);
            cout << "Sending " << client_fd << " Client Buffer: " << s << endl;

            // 실제 전송은 buffer를 recv_result 길이만큼만 그대로 돌려보냄.
            send(client_fd, buffer, recv_result, 0);
        }
    }
}