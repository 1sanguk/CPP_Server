#pragma once

#include <queue>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <string>
#include <unordered_map>

// reactor가 recv()로 받은 뒤, 워커에게 넘기는 작업 단위.
struct Job{
    int client_fd;      // 이 데이터가 어느 커넥션에서 온 것인지
    std::string data;   // 아직 세션 송신 버퍼에 합쳐지지 않은 원본 수신 바이트
};

// 워커 한 명이 갖는 전용 작업 큐 묶음. 워커마다 이걸 하나씩 가지고 있어서
// (sticky routing) 같은 커넥션의 job은 항상 같은 워커가 순서대로 처리한다.
struct Worker{
    std::mutex worker_mutex;             // job_queue만 보호한다.
    std::queue<Job> job_queue;           // 이 워커에게 배정된 job들
    std::condition_variable job_condition; // job 도착/종료 신호로 이 워커를 깨움
};

class EpollThreadPoolServer{
    private:
        int server_port;

        int listen_fd = -1;      // accept 전용 리스닝 소켓
        int epoll_fd = -1;       // listen_fd/client fd/stop_event_fd를 함께 감시하는 epoll 인스턴스
        int stop_event_fd = -1;  // Stop()이 write하면 epoll_wait()를 깨우는 eventfd
        std::mutex fd_mutex;     // stop_event_fd를 읽고 쓰는 구간을 보호

        std::unordered_set<int> client_fds{}; // 현재 추적 중인(연결된) client fd 집합

        // client_fd -> 아직 못 보낸 송신 데이터. 워커가 append하고 reactor가 비운다.
        std::unordered_map<int, std::string> send_buffers{};
        std::mutex send_mutex; // send_buffers를 보호

        const int kWorkerCounts = 4; // 워커 스레드 개수(고정). workers 벡터 크기와 항상 같아야 함.

        std::vector<Worker> workers{kWorkerCounts}; // 워커별 job 큐 묶음 (kWorkerCounts개)
        std::vector<std::thread> threads{};         // 위 workers를 실행하는 실제 OS 스레드들

        std::mutex active_client_mutex; // client_fds를 보호

        std::atomic<int> sleeping_worker_count{0}; // 현재 wait()에서 잠들어 있는 워커 수 (모니터 로그용)

        void Clean_Up(); // Stop() 이후 모든 스레드/fd/버퍼를 정리
        void Enqueue_Job(int client_fd, const char* data, std::size_t length); // reactor -> 워커로 job 전달
        void Process_Job(const Job& job); // 워커가 job 하나를 세션 버퍼에 반영
        void Worker_Loop(int index); // 워커 스레드의 메인 루프. index는 자기 담당 workers[index]를 가리킴

        std::thread monitor_thread;
        // monitor는 특정 워커의 job 알림과 무관하게 "30초 주기 로그 또는 종료 신호"만
        // 기다리므로, 워커의 job_condition을 빌려 쓰지 않고 전용 쌍을 따로 둔다.
        std::mutex monitor_mutex;
        std::condition_variable monitor_condition;
        void Monitor_Workers(); // 모니터 스레드의 메인 루프

        std::mutex log_mutex; // 여러 스레드의 로그 출력이 한 줄 단위로 섞이지 않도록 보호
        void Logging(const std::string& msg); // 모든 로그 출력이 거치는 단일 지점

        // 서버 생명주기 상태. Created -> Running -> Stopping -> Cleaning -> Stopped
        // 순서로만 전이하며, compare_exchange로 중복 시작/종료를 막는다.
        enum class ServerState{
            Created,
            Running,
            Stopping,
            Cleaning,
            Stopped,
        };

        std::atomic<ServerState> server_state{ServerState::Created};

        // Send_All() 한 번 호출한 결과.
        enum class SendState{
            Completed, // data를 전부 보냄
            Partial,   // 일부만 보내고 커널 송신 버퍼가 찼음(EAGAIN, 에러 아님)
            Failed,    // 복구 불가능한 실제 오류
        };

        // client_fd에 data를 최대한 보낸다. non-blocking 소켓이라 한 번에 다 못 보낼 수
        // 있고, 그 경우 이미 보낸 만큼은 data에서 지우고 Partial을 반환한다.
        SendState Send_All(int client_fd, std::string& data);

    public:
        EpollThreadPoolServer(int port);
        ~EpollThreadPoolServer(); // Stop()과 Clean_Up()을 재사용해 안전하게 정리

        void Server_Run(); // listen/epoll 준비, 워커·모니터 스레드 생성 후 이벤트 루프 실행 (블로킹)
        void Stop();        // 종료 요청 (여러 번 호출해도 안전, 어느 스레드에서 불러도 됨)
        void Proceed_Stop(); // Stop() + Clean_Up()을 순서대로 실행하는 편의 함수

        // 연결 하나를 안전하게 정리(목록 제거 + epoll 등록 해제 + close). 같은 fd로
        // 두 번 호출돼도 실제 정리는 한 번만 일어난다 (fd 재사용 시 이중 close 방지).
        void Delete_Client_Fd(int client_fd);

};
