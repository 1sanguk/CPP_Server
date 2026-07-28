#pragma once
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <cstddef>
#include <unordered_set>

class ThreadPoolEchoServer{
    private:
        // 서버 설정 및 listen 소켓. client fd는 client_queue를 거쳐 worker에게 전달된다.
        int server_port;
        int fd = -1;

        // accept 스레드(생산자)와 worker 스레드(소비자)가 공유하는 작업 큐.
        // std::queue 자체는 thread-safe하지 않으므로 모든 접근을 queue_mutex로 보호한다.
        std::queue<int> client_queue;
        std::mutex queue_mutex;
        std::condition_variable queue_condition;

        // accept된 모든 client fd를 추적한다. 큐에서 대기 중인 fd와 worker가 처리 중인 fd를
        // 모두 포함하며, 등록과 제거는 active_clients_mutex로 보호한다.
        std::mutex active_clients_mutex;
        std::unordered_set<int> active_clients;

        // 서버 시작 시 고정 개수로 생성하며, 접속 수가 늘어도 worker 수는 증가하지 않는다.
        std::vector<std::thread> workers;

        // queue_mutex로 보호되는 종료 상태. true가 되면 대기 중인 worker가 반복문을 빠져나간다.
        bool stopping = false;

        // wait()에서 작업을 기다리는 worker 수. monitor 스레드에서도 읽으므로 atomic으로 관리한다.
        std::atomic<int> sleeping_worker_count{0};

        // 큐에서 client fd를 꺼내 연결 종료까지 처리한 뒤 다시 큐로 돌아가는 worker 본체.
        void Worker_Loop();

        // client fd 하나를 전담하는 blocking recv/send echo 루프.
        void Process_Client(int client_fd);

        // partial send에 대비해 남은 데이터 범위를 모두 보낼 때까지 send()를 반복한다.
        bool Send_All(
            int client_fd,
            const char* data,
            std::size_t length
        );

        // 활성 목록에서 fd를 제거하고 실제 소켓까지 닫는 공통 정리 함수.
        void Close_Client(int client_fd);

        // accept된 client fd를 큐에 넣고 대기 중인 worker 하나를 깨운다.
        // 큐가 가득 찼거나 종료 중이면 false를 반환한다.
        bool Enqueue_Client(int client_fd);

        // stopping은 queue_mutex로 보호되므로 accept 루프에서도 이 함수를 통해 확인한다.
        bool Is_Stopping();

        // worker 상태를 주기적으로 출력하기 위한 별도 관찰용 스레드.
        std::thread monitor_thread;
        void Monitor_Workers();

    public:
        // port만 저장하고 실제 socket/bind/listen은 Server_Run()에서 수행한다.
        ThreadPoolEchoServer(const int port);

        // 종료 상태 설정 → worker 깨우기 → worker/monitor join 순서로 소유 스레드를 정리한다.
        ~ThreadPoolEchoServer();

        // listen 소켓과 고정 worker를 준비한 뒤 accept 결과를 작업 큐에 계속 전달한다.
        void Server_Run();

        void Stop();
};
