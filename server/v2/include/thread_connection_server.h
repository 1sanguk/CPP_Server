#pragma once
#include <cstddef>
#include <mutex>
#include <unordered_set>

class ThreadPerConnectionEchoServer{
    private:
        int server_port;  // 이 서버가 바인딩할 포트 번호
        int fd = -1;      // listen 소켓의 파일 디스크립터. -1이면 아직 socket()이 호출되지 않은 상태(센티널 값)

        // accept()로 받은 client_fd 하나를 전담하는 함수. 스레드마다 하나씩 이 함수가 돌아감.
        // recv/Send_All 루프를 돌다가 연결이 끝나면 Close_Client()로 정리하고 스레드가 종료된다.
        void Process_Client(int client_fd);

        // partial send에 대비해 남은 데이터 범위를 모두 보낼 때까지 send()를 반복한다.
        bool Send_All(
            int client_fd,
            const char* data,
            std::size_t length
        );

        // accept 스레드와 각 client 스레드가 공유하는 활성 client fd 목록.
        // std::unordered_set은 thread-safe하지 않으므로 모든 접근을 같은 mutex로 보호한다.
        std::mutex active_clients_mutex;
        std::unordered_set<int> active_clients;

        // 활성 목록에서 fd를 제거하고 실제 소켓까지 닫는 공통 정리 함수.
        void Close_Client(int client_fd);

    public:
        // port만 저장. 실제 소켓 생성/bind/listen은 Server_Run()에서 수행.
        ThreadPerConnectionEchoServer(const int port);

        // fd가 유효하면(-1이 아니면) close()로 listen 소켓을 정리.
        // 클라이언트별 fd는 각 Process_Client 스레드의 Close_Client()에서 정리한다.
        ~ThreadPerConnectionEchoServer();

        // socket()~listen()으로 서버를 준비시키고, accept 루프를 돌며
        // 접속마다 새 스레드를 Process_Client에 넘기고 detach한다(thread-per-connection).
        void Server_Run();
};
