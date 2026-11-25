#pragma once

#include <async_logger.h>
#include <game_job_queue.h>
#include <packet_assembler.h>
#include <world.h>

#include <liburing.h>
#include <sys/socket.h>
#include <sys/uio.h>

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

// v10: v9와 같은 게임 로직(틱 루프 + 격자 AOI) 위에 io_uring 네트워크 계층을 올린다.
//
// 스레드 구성
//  - ring 스레드(Server_Run): io_uring에 accept·recv·send를 제출하고 완료(CQE)를 처리한다. 소켓을 만지는 유일한 스레드다.
//  - 틱 스레드(Game_Tick_Loop): v9와 같다. 게임 상태를 혼자 소유하고 소켓은 만지지 않는다.
//  - main 스레드: sigwait로 SIGINT/SIGTERM(그리고 Enter)을 기다렸다가 Stop()을 부른다(v9와 같다).
//
// v9(epoll)와 다른 점 — v8(IOCP)과 닮은 점
//  - epoll은 "읽을/쓸 수 있다"만 알려 주고 recv/send는 직접 불렀다. io_uring은 IOCP처럼 작업을 맡기고(SQE) 끝나면 결과(CQE)를 받는다.
//  - 그래서 커널이 버퍼를 쓰는 동안(작업이 끝날 때까지) 버퍼와 세션이 살아 있어야 하고, 세션을 닫을 때는 진행 중인 작업을 취소하고
//    그 완료가 모두 돌아온 뒤에만 fd를 닫고 세션을 놓는다(v8의 pending I/O 회수와 같은 문제).
//  - 한 루프에서 만든 여러 작업을 시스템 호출 한 번(io_uring_submit_and_wait)으로 제출하고 완료를 한꺼번에 가져온다.

using SendBuffer = std::shared_ptr<const std::vector<char>>;

struct ClientSession{
    ClientSession(int fd_value, std::uint32_t id);

    // fd는 ring 스레드만 쓰고 닫는다. 진행 중인 작업이 모두 끝난 뒤에만 닫는다.
    const int fd;
    const std::uint32_t session_id;
    std::string remote_endpoint = "unknown";

    // ---- ring 스레드만 쓰는 상태(잠금 없음) ----
    PacketAssembler assembler;
    // 고정 파일 표의 칸 번호(V10_FIXED_FILES). -1이면 일반 fd로 제출한다.
    int file_slot = -1;
    std::vector<char> recv_buffer;   // 고정 버퍼 수신 방식에서만 쓴다(8 KiB). provided 방식은 공용 버퍼 묶음을 쓴다
    bool recv_pending = false;       // 걸어 둔 recv가 아직 끝나지 않음(multishot이면 F_MORE가 꺼질 때까지)
    bool recv_paused = false;        // job 큐 수위 때문에 recv를 다시 걸지 않는 중
    bool send_inflight = false;      // 제출한 sendmsg가 아직 끝나지 않음(세션당 하나만)
    // 진행 중인 sendmsg가 가리키는 것들. 완료까지 바꾸거나 놓으면 안 된다.
    std::vector<iovec> send_iov;
    std::vector<SendBuffer> send_in_flight;
    std::size_t send_inflight_bytes = 0;
    msghdr send_message{};

    // ---- 틱 스레드(적재)와 ring 스레드(송신)가 함께 쓰는 송신 상태 ----
    std::mutex send_mutex;
    std::deque<SendBuffer> send_queue;
    std::size_t send_queued_bytes = 0;
    std::size_t send_offset = 0;     // send_queue.front()에서 이미 보낸 바이트
    bool flush_requested = false;

    // 닫기 시작함(한 번만). 틱 스레드도 읽어 닫히는 세션에 더 쌓지 않는다.
    std::atomic<bool> closing = false;
    // 틱 스레드가 ring에 닫기를 이미 요청했는지
    std::atomic<bool> close_requested = false;

    // ---- 틱 스레드만 쓰는 상태(v8·v9와 같다) ----
    std::uint64_t chat_tick = 0;
    std::uint32_t chats_in_tick = 0;
    std::uint64_t last_move_tick = 0;
    std::uint32_t last_input_seq = 0;
    bool moving = false;
};

class UringWorldServer{
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

        // 수신 방식. Fixed: 세션마다 8 KiB 버퍼로 recv를 하나씩 건다(IOCP와 같은 모양).
        // Provided: 공용 버퍼 묶음(provided buffer ring)을 등록하고 multishot recv를 건다. 커널이 데이터가 올 때 버퍼를 골라 쓰므로
        // 유휴 연결은 수신 버퍼를 갖지 않는다.
        enum class RecvMode{
            Fixed,
            Provided,
        };

        // CQE의 user_data에 담는 작업 종류. 포인터 대신 (종류, session_id)를 담아, 세션이 먼저 사라져도 엉뚱한 메모리를 가리키지 않게 한다.
        enum class Op : std::uint8_t{
            Accept = 1,
            Recv = 2,
            Send = 3,
            Wake = 4,
            Cancel = 5,
        };

        const int server_port;
        int listen_fd = -1;
        // 틱 스레드(송신 요청)와 Stop()이 ring을 깨우는 eventfd. 생성자에서 만들고 소멸자에서 닫는다(v9와 같다).
        const int wake_fd;
        std::uint64_t wake_value = 0;   // eventfd read가 채우는 자리(완료까지 살아 있어야 한다)
        std::atomic<ServerState> server_state = ServerState::Creating;

        // ---- io_uring (ring 스레드만) ----
        io_uring ring{};
        bool ring_ready = false;
        const RecvMode recv_mode;
        const unsigned ring_entries;
        // provided buffer ring
        static constexpr int kBufferGroup = 1;
        const unsigned provided_buffer_count;
        const unsigned provided_buffer_size;
        io_uring_buf_ring* buffer_ring = nullptr;
        std::vector<char> provided_buffers;
        // 제출했고 아직 마지막 CQE가 오지 않은 작업 수. 종료할 때 0이 될 때까지 회수한다(v8의 pending_io_count).
        std::size_t pending_ops = 0;
        bool accept_armed = false;
        // 비교용 옵션(V10_SQPOLL, V10_FIXED_FILES). 생성자에서 읽고, 커널이 지원하지 않으면 Create_Ring에서 끈다.
        bool use_sqpoll = false;
        bool use_fixed_files = false;
        unsigned fixed_file_capacity = 0;
        unsigned next_file_slot = 0;
        std::vector<int> free_file_slots;
        bool wake_armed = false;

        // ring 스레드만 쓰는 세션 목록. 키는 session_id(CQE의 user_data에도 넣는다).
        std::unordered_map<std::uint32_t, std::shared_ptr<ClientSession>> sessions;
        std::uint32_t next_session_id = 1;
        std::vector<std::shared_ptr<ClientSession>> paused_sessions;
        std::atomic<std::size_t> paused_count = 0;
        std::vector<std::shared_ptr<ClientSession>> pending_leaves;

        // 틱 스레드 → ring: 보낼 것이 생긴 세션과 닫아 달라는 세션. eventfd로 깨운다.
        std::mutex request_mutex;
        std::vector<std::shared_ptr<ClientSession>> flush_requests;
        std::vector<std::shared_ptr<ClientSession>> close_requests;

        GameJobQueue game_jobs;
        std::thread game_thread;
        const std::chrono::milliseconds tick_period;
        const std::chrono::milliseconds status_interval;

        // 아래는 틱 스레드만 읽고 쓴다(v9와 같다).
        World world;
        const std::uint32_t chats_per_tick;
        const std::uint64_t input_timeout_ticks;
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
        const std::size_t max_enter_leave_per_tick;
        std::deque<GameJob> deferred_jobs;
        std::size_t enter_leave_this_tick = 0;

        const std::size_t job_high_watermark;
        const std::size_t job_low_watermark;

        AsyncLogger logger;
        const LogLevel minimum_log_level;

        // 지표
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
        std::atomic<std::uint64_t> enter_count = 0;        // io_uring_enter 시스템 호출(제출·대기)
        std::atomic<std::uint64_t> cqe_count = 0;
        std::atomic<std::uint64_t> sqe_count = 0;
        std::atomic<std::uint64_t> send_count = 0;
        std::atomic<std::uint64_t> partial_send_count = 0;
        std::atomic<std::uint64_t> send_overflow_count = 0;
        std::atomic<std::uint64_t> cancel_count = 0;
        std::atomic<std::uint64_t> no_buffer_count = 0;    // provided 버퍼가 모자라 recv가 끝난 수(-ENOBUFS)
        std::atomic<std::uint64_t> cq_overflow = 0;
        std::atomic<std::size_t> pending_ops_now = 0;
        std::atomic<std::uint64_t> tick_count = 0;
        std::atomic<std::uint64_t> late_tick_count = 0;
        std::atomic<std::uint64_t> skipped_tick_count = 0;
        std::atomic<std::uint64_t> window_tick_count = 0;
        std::atomic<std::uint64_t> window_tick_us_sum = 0;
        std::atomic<std::uint64_t> window_tick_us_max = 0;
        std::atomic<std::uint64_t> window_sent_bytes = 0;
        std::atomic<std::uint64_t> total_tick_us_max = 0;
        std::array<std::atomic<std::uint64_t>, 5> window_phase_us{};
        std::atomic<std::size_t> player_count = 0;
        std::atomic<std::size_t> max_cell_population = 0;
        std::atomic<std::size_t> deferred_job_count = 0;
        std::atomic<std::uint64_t> deferred_job_total = 0;

        // ---- ring 스레드 ----
        bool Create_Listen_Socket();
        bool Create_Ring();
        bool Setup_Provided_Buffers();
        void Ring_Loop();
        // 쌓인 CQE를 모두 처리하고 소비 표시한다.
        void Drain_Completions();
        io_uring_sqe* Get_Sqe();
        static std::uint64_t Pack(Op op, std::uint32_t session_id);
        int Acquire_File_Slot(int fd);
        void Release_File_Slot(int slot);
        // recv·send SQE의 대상을 고정 파일 칸으로 바꾼다(고정 파일을 쓰는 세션만).
        static void Set_Target(io_uring_sqe* sqe, const ClientSession& session);
        void Arm_Accept();
        void Arm_Wake();
        void Post_Recv(const std::shared_ptr<ClientSession>& session);
        void Post_Send(const std::shared_ptr<ClientSession>& session);
        void Handle_Cqe(const io_uring_cqe* cqe);
        void Handle_Accept(int result, bool more);
        void Handle_Recv(const std::shared_ptr<ClientSession>& session, int result, std::uint32_t flags);
        void Handle_Send(const std::shared_ptr<ClientSession>& session, int result);
        // 받은 바이트를 조립기에 넣고 완성된 패킷을 job 큐에 넣는다. 수위를 넘으면 false(더 받지 않음).
        bool Consume_Received(const std::shared_ptr<ClientSession>& session, const char* data, std::size_t length);
        // 조립기에서 완성된 패킷을 모두 꺼내 job 큐에 넣는다. 오류면 닫고, 수위를 넘으면 멈추고 false.
        bool Push_Assembled(const std::shared_ptr<ClientSession>& session);
        void Recycle_Buffer(std::uint16_t buffer_id);
        void Process_Requests();
        void Pause_Recv(const std::shared_ptr<ClientSession>& session);
        void Resume_Paused_Sessions();
        // 닫기를 시작한다: 진행 중인 작업을 취소하고 Leave를 알린다. 실제로 fd를 닫는 것은 Try_Finish_Close.
        void Begin_Close(const std::shared_ptr<ClientSession>& session, const char* reason);
        // 그 세션의 작업이 모두 끝났으면 fd를 닫고 목록에서 뺀다.
        void Try_Finish_Close(const std::shared_ptr<ClientSession>& session);
        void Submit_Leave(const std::shared_ptr<ClientSession>& session);
        void Retry_Pending_Leaves();
        void Clean_Up();

        // ---- 틱 스레드 → ring ----
        void Wake_Ring();
        void Enqueue_Send(const std::shared_ptr<ClientSession>& session, SendBuffer data);
        void Request_Close(const std::shared_ptr<ClientSession>& session);

        // ---- 틱 스레드(v9와 같은 흐름) ----
        void Game_Tick_Loop();
        void Process_Job(GameJob& job);
        bool Process_Or_Defer(GameJob& job);
        void Handle_Enter(const std::shared_ptr<ClientSession>& session);
        void Handle_Leave(const std::shared_ptr<ClientSession>& session);
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
        static RecvMode Read_Recv_Mode_From_Env();

    public:
        explicit UringWorldServer(int port);
        ~UringWorldServer();
        UringWorldServer(const UringWorldServer&) = delete;
        UringWorldServer& operator=(const UringWorldServer&) = delete;

        void Server_Run();
        void Stop();
};
