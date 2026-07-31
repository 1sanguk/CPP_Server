#pragma once

#include <string>
#include <mutex>
#include <unordered_set>
#include <atomic>

class EpollEchoServer{
    private:
        // 서버 설정과 epoll에서 감시할 핵심 fd들.
        // listen_fd는 새 접속, epoll_fd는 감시 목록, stop_event_fd는 종료 알림을 담당한다.
        int server_port;
        int listen_fd = -1;
        int epoll_fd = -1;
        int stop_event_fd = -1;

        // v4는 단일 이벤트 루프지만, main thread의 Stop()과 server thread의 자원 정리가 겹칠 수 있다.
        // server_state는 atomic으로 관리하고, client_fds와 fd 정리는 queue_mutex로 보호한다.
        std::mutex queue_mutex;
        // 현재 epoll에 등록되어 있고 아직 닫히지 않은 client fd 목록.
        std::unordered_set<int> client_fds{};
        
        // stop/client/listen/epoll fd를 한 번만 닫는 최종 정리 함수.
        // Stopping -> Cleaning 전이에 성공한 호출자만 실제 fd close를 수행한다.
        void CleanUp();

        // main thread의 Stop()과 server thread의 Server_Run()/CleanUp()이 공유하는 서버 생명주기 상태.
        // Created -> Running -> Stopping -> Cleaning -> Stopped 흐름으로 관리한다.
        enum class ServerState{
            Created,
            Running,
            Stopping,
            Cleaning,
            Stopped,
        };

        std::atomic<ServerState> server_state{ServerState::Created};

    public:
        // port만 저장하고 실제 socket/bind/listen/epoll 생성은 Server_Run()에서 수행한다.
        EpollEchoServer(int port);

        // 명시적 Stop() 여부와 관계없이 남은 fd를 정리한다.
        ~EpollEchoServer();

        // listen socket을 만들고 epoll 이벤트 루프에서 accept/recv/send/stop 이벤트를 처리한다.
        void Server_Run();

        // main thread에서 호출하는 종료 요청 함수.
        // Running -> Stopping 전이에 성공한 경우 eventfd에 write해 epoll_wait()를 깨운다.
        void Stop();

        // blocking send 기반 echo 전송 helper. partial send와 EINTR 재시도를 처리한다.
        bool SendAll(int client_fd, const char* data, std::size_t length);

        // client fd를 epoll 감시 목록과 추적 set에서 제거한 뒤 close한다.
        void DeleteClientFd(int client_fd);
};
