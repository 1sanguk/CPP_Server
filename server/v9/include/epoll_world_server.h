#pragma once

#include <async_logger.h>
#include <game_job_queue.h>
#include <packet_assembler.h>
#include <world.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// v9: v8의 틱 루프 + 격자 AOI를 Linux epoll 위에 올린다.
//
// 스레드 구성
//  - reactor 스레드(Server_Run): epoll로 accept·recv·send를 모두 한다. 소켓(fd)을 만지는 유일한 스레드다.
//  - 틱 스레드(Game_Tick_Loop): v8과 같다. 게임 상태(World, players)를 혼자 소유하고 소켓은 만지지 않는다.
//  - main 스레드: sigwait로 SIGINT/SIGTERM을 기다렸다가 Stop()을 부른다(v5와 같다).
//
// v8(IOCP)와 다른 점
//  - IOCP는 "다 받았다/다 보냈다"(완료 통지)를 알려 주고, epoll은 "지금 읽을/쓸 수 있다"(준비 통지)만 알려 준다.
//    그래서 recv/send는 reactor가 직접 부르고, EAGAIN과 일부만 보낸 상태를 직접 관리한다.
//  - v8은 틱 스레드가 WSASend 게시를 worker에게 넘겼다. v9는 틱 스레드가 송신 큐에 넣은 뒤 "보낼 것이 생긴 세션"
//    목록을 reactor에 넘기고 eventfd로 깨운다. 실제 writev는 reactor가 한다.
//  - 수신 멈춤(backpressure)은 Recv를 게시하지 않는 대신 그 fd의 EPOLLIN 감시를 뺀다.

using SendBuffer = std::shared_ptr<const std::vector<char>>;

struct ClientSession{
    ClientSession(int fd_value, std::uint32_t id);

    // fd는 reactor만 쓰고 닫는다. 번호가 곧바로 재사용되므로 게임 로직과 epoll 식별자는 session_id를 쓴다.
    const int fd;
    const std::uint32_t session_id;
    std::string remote_endpoint = "unknown";

    // ---- reactor만 쓰는 상태(잠금 없음) ----
    PacketAssembler assembler;
    bool recv_paused = false;      // EPOLLIN 감시를 뺐는지
    bool watching_write = false;   // EPOLLOUT 감시를 켰는지

    // ---- 틱 스레드(적재)와 reactor(송신)가 함께 쓰는 송신 상태 ----
    std::mutex send_mutex;
    std::deque<SendBuffer> send_queue;
    std::size_t send_queued_bytes = 0;
    std::size_t send_offset = 0;     // send_queue.front()에서 이미 보낸 바이트
    bool flush_requested = false;    // reactor의 송신 요청 목록에 이미 들어 있는지

    // 한 번만 닫기 위한 표시. 틱 스레드도 읽어 닫힌 세션에 더 쌓지 않는다.
    std::atomic<bool> closing = false;
    // 틱 스레드가 reactor에 닫기를 이미 요청했는지. 닫힐 때까지 틱마다 다시 요청하거나 더 쌓지 않게 한다.
    std::atomic<bool> close_requested = false;

    // ---- 틱 스레드만 쓰는 상태(v8과 같다) ----
    std::uint64_t chat_tick = 0;
    std::uint32_t chats_in_tick = 0;
    std::uint64_t last_move_tick = 0;
    std::uint32_t last_input_seq = 0;
    bool moving = false;
};

class EpollWorldServer{
    private:
        enum class ServerState{
            Creating,
            Running,
            Stopping,
            Stopped,
        };

        enum class LogLevel{
            Debug,
            Info,
            Error,
        };

        // 송신 결과. v5의 SendState와 같은 뜻이다.
        enum class SendState{
            Completed,   // 큐를 다 비웠다
            Partial,     // 커널 송신 버퍼가 가득 찼다(EAGAIN). EPOLLOUT을 기다린다
            Failed,      // 연결 오류
        };

        const int server_port;
        int listen_fd = -1;
        int epoll_fd = -1;
        // 틱 스레드(송신 요청)와 Stop()이 reactor를 깨우는 eventfd. 생성자에서 만들고 소멸자에서 닫는다.
        // Stop()은 main 스레드에서 불리므로, reactor가 정리 중에 닫으면 닫힌(또는 재사용된) fd에 쓸 수 있다.
        const int wake_fd;
        std::atomic<ServerState> server_state = ServerState::Creating;

        // reactor만 쓰는 세션 목록. 키는 session_id(epoll 이벤트의 data.u64에도 넣는다).
        std::unordered_map<std::uint32_t, std::shared_ptr<ClientSession>> sessions;
        std::uint32_t next_session_id = 1;
        std::vector<std::shared_ptr<ClientSession>> paused_sessions;
        // 틱 스레드가 "큐가 비었으니 수신을 다시 켜도 된다"고 reactor를 깨울지 판단할 때 읽는다.
        std::atomic<std::size_t> paused_count = 0;
        // job 큐가 가득 차 넣지 못한 Leave. 잃으면 플레이어가 월드에 남으므로 reactor가 매 루프 다시 넣는다.
        std::vector<std::shared_ptr<ClientSession>> pending_leaves;
        std::vector<char> recv_buffer;

        // 틱 스레드 → reactor: 보낼 것이 생긴 세션과 닫아 달라는 세션. eventfd로 깨운다.
        std::mutex request_mutex;
        std::vector<std::shared_ptr<ClientSession>> flush_requests;
        std::vector<std::shared_ptr<ClientSession>> close_requests;

        // reactor(생산자)와 틱 루프(소비자)를 잇는 큐와 틱 스레드
        GameJobQueue game_jobs;
        std::thread game_thread;
        const std::chrono::milliseconds tick_period;
        const std::chrono::milliseconds status_interval;

        // 아래는 틱 스레드만 읽고 쓴다(v8과 같다).
        World world;
        const std::uint32_t chats_per_tick;
        const std::uint64_t input_timeout_ticks;
        // 개발용 지정 스폰(V9_SPAWN_X/Y/RADIUS). 셋 다 있으면 무작위 대신 그 원 안에 스폰한다(봇 확인용).
        struct SpawnArea{
            float x = 0.0f;
            float y = 0.0f;
            float radius = 0.0f;
        };
        const std::optional<SpawnArea> spawn_area;
        std::mt19937 spawn_rng{std::random_device{}()};
        std::unordered_map<std::uint32_t, std::shared_ptr<ClientSession>> players;
        std::unordered_map<std::uint32_t, std::vector<char>> outbox;
        std::uint64_t current_tick = 0;
        std::vector<std::shared_ptr<ClientSession>> tick_flushes;
        // 한 틱에 처리할 입장·퇴장 수 상한(V9_MAX_ENTER_LEAVE_PER_TICK). 입장·퇴장 하나는 시야 안의 모두에게 보임/사라짐을
        // 기록하므로, 한 시야 안의 N명이 한꺼번에 들어오거나 나가면 그 틱이 N²에 비례해 길어진다.
        // 상한을 넘으면 그 뒤의 job(패킷 포함)을 순서 그대로 다음 틱으로 미룬다. 패킷만 먼저 처리하면 아직 입장하지 않은
        // 세션의 패킷이 버려진다(v7에서 겪은 문제).
        const std::size_t max_enter_leave_per_tick;
        std::deque<GameJob> deferred_jobs;
        std::size_t enter_leave_this_tick = 0;

        const std::size_t job_high_watermark;
        const std::size_t job_low_watermark;

        AsyncLogger logger;
        const LogLevel minimum_log_level;

        // 지표(여러 스레드가 쓰고 reactor의 Log_Status가 읽는다)
        std::atomic<std::size_t> session_count = 0;
        std::atomic<std::uint64_t> processed_job_count = 0;
        std::atomic<std::uint64_t> discarded_job_count = 0;
        std::atomic<std::uint64_t> rejected_job_count = 0;
        std::atomic<std::uint64_t> chat_count = 0;
        std::atomic<std::uint64_t> dropped_chat_count = 0;
        std::atomic<std::uint64_t> move_input_count = 0;
        std::atomic<std::uint64_t> expired_input_count = 0;
        std::atomic<std::uint64_t> recv_pause_count = 0;
        std::atomic<std::uint64_t> wake_count = 0;
        std::atomic<std::uint64_t> writev_count = 0;
        std::atomic<std::uint64_t> partial_send_count = 0;
        std::atomic<std::uint64_t> send_overflow_count = 0;
        std::atomic<std::uint64_t> tick_count = 0;
        std::atomic<std::uint64_t> late_tick_count = 0;
        std::atomic<std::uint64_t> skipped_tick_count = 0;
        std::atomic<std::uint64_t> window_tick_count = 0;
        std::atomic<std::uint64_t> window_tick_us_sum = 0;
        std::atomic<std::uint64_t> window_tick_us_max = 0;
        std::atomic<std::uint64_t> window_sent_bytes = 0;
        std::atomic<std::uint64_t> total_tick_us_max = 0;
        // 틱 단계별 처리 시간 합(µs): 입력, 이동, 시야 수집, 인코딩, 송신 요청
        std::array<std::atomic<std::uint64_t>, 5> window_phase_us{};
        std::atomic<std::size_t> player_count = 0;
        std::atomic<std::size_t> deferred_job_count = 0;
        std::atomic<std::uint64_t> deferred_job_total = 0;
        std::atomic<std::size_t> max_cell_population = 0;

        // ---- reactor ----
        bool Create_Listen_Socket();
        bool Create_Epoll();
        void Reactor_Loop();
        void Handle_Accept();
        void Handle_Readable(const std::shared_ptr<ClientSession>& session);
        void Handle_Writable(const std::shared_ptr<ClientSession>& session);
        // 송신 큐를 writev로 보낸다. 다 못 보내면 EPOLLOUT을 켜고, 다 보내면 끈다.
        SendState Send_Queued(const std::shared_ptr<ClientSession>& session);
        // 틱 스레드가 넘긴 송신·닫기 요청을 처리한다(eventfd로 깨어났을 때).
        void Process_Requests();
        void Pause_Recv(const std::shared_ptr<ClientSession>& session);
        void Resume_Paused_Sessions();
        // EPOLLIN/EPOLLOUT 감시를 세션 상태(recv_paused, watching_write)에 맞춘다.
        bool Update_Interest(const ClientSession& session);
        void Close_Session(const std::shared_ptr<ClientSession>& session, const char* reason);
        void Clean_Up();

        // ---- 틱 스레드 → reactor ----
        void Wake_Reactor();
        // 세션 송신 큐에 넣는다. reactor에 알려야 하면 tick_flushes에 넣는다(틱 끝에 한 번에 넘긴다).
        void Enqueue_Send(const std::shared_ptr<ClientSession>& session, SendBuffer data);
        void Request_Close(const std::shared_ptr<ClientSession>& session);

        // ---- 틱 스레드(v8과 같다) ----
        void Game_Tick_Loop();
        void Process_Job(GameJob& job);
        void Handle_Enter(const std::shared_ptr<ClientSession>& session);
        void Handle_Leave(const std::shared_ptr<ClientSession>& session);
        // 입장·퇴장 상한을 넘었거나 이미 미룬 job이 있으면 미루고(true), 아니면 처리한다(false).
        bool Process_Or_Defer(GameJob& job);
        void Handle_Packet(const std::shared_ptr<ClientSession>& session, const protocol::Packet& packet);
        void Expire_Idle_Inputs();
        void Flush_Tick_Output();
        void Record_Tick_Time(std::uint64_t elapsed_us);

        void Log_Status();
        void Logging(LogLevel level, const std::string& message);
        static LogLevel Read_Log_Level_From_Env();
        static double Read_Number_From_Env(const char* name, double default_value, double min_value, double max_value);
        static World::Config Read_World_Config_From_Env();
        static std::uint64_t Input_Timeout_Ticks(double timeout_ms, long long tick_ms);
        static std::optional<SpawnArea> Read_Spawn_Area_From_Env();
        void Submit_Leave(const std::shared_ptr<ClientSession>& session);
        void Retry_Pending_Leaves();

    public:
        explicit EpollWorldServer(int port);
        ~EpollWorldServer();
        EpollWorldServer(const EpollWorldServer&) = delete;
        EpollWorldServer& operator=(const EpollWorldServer&) = delete;

        // listen·epoll을 준비하고 틱 스레드를 띄운 뒤 reactor 루프를 돈다(블로킹). Stop()이 불리면 정리하고 돌아온다.
        void Server_Run();
        // 어느 스레드에서 불러도 안전하다. reactor를 깨워 루프를 끝내게 한다.
        void Stop();
};
