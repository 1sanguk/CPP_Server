#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mswsock.h>

#include <async_logger.h>
#include <game_job_queue.h>
#include <packet_assembler.h>
#include <world.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 한 번 만든 송신 데이터는 바꾸지 않고, WSASend가 끝날 때까지 shared_ptr로 수명을 붙잡는다.
using SendBuffer = std::shared_ptr<const std::vector<char>>;

struct ClientSession;

struct IoContext{
    enum class IoType{
        Accept,
        Recv,
        Send,
    };

    IoContext(IoType type, std::size_t buffer_size) : io_type(type), buffer(buffer_size){}

    // Context 하나는 한 번에 정확히 하나의 pending 작업만 표현한다.
    const IoType io_type;
    SOCKET accept_socket = INVALID_SOCKET;
    // pending인 동안 세션 수명을 붙잡는다. completion 처리 시작 시 비워서 순환 참조를 끊는다.
    std::shared_ptr<ClientSession> session;
    WSABUF wsa_buffer{};
    DWORD flags = 0;
    // Accept 주소와 Recv 데이터를 받는 버퍼. Send context는 버퍼를 갖지 않는다.
    std::vector<char> buffer;

    // Send 전용: 송신 큐의 버퍼들을 복사하지 않고 WSABUF 배열로 가리킨다(scatter-gather).
    // in_flight는 커널이 읽는 동안 그 버퍼들이 해제되지 않도록 completion까지 붙잡아 둔다.
    std::vector<WSABUF> send_buffers;
    std::vector<SendBuffer> in_flight;
    std::size_t logical_send_length = 0;

    // 첫 멤버가 아니므로 completion에서는 CONTAINING_RECORD로 IoContext를 복원한다.
    OVERLAPPED overlapped{};
};

// 틱 루프가 worker에게 넘기는 "이 세션들의 WSASend를 게시해 달라"는 묶음. PostQueuedCompletionStatus로 전달한다.
struct SendPostBatch{
    std::vector<std::shared_ptr<ClientSession>> sessions;
    OVERLAPPED overlapped{};
};

struct ClientSession{
    ClientSession(SOCKET socket_value, std::uint32_t id);

    SOCKET client_socket = INVALID_SOCKET;
    // socket 핸들은 재사용될 수 있으므로 게임 로직과 클라이언트에는 별도의 증가 id를 사용한다(월드의 플레이어 id).
    const std::uint32_t session_id;
    std::string remote_endpoint = "unknown";

    // 세션당 WSARecv는 하나만 pending이므로 completion worker 하나만 동시에 접근한다. 락이 필요 없다.
    PacketAssembler assembler;

    // v8: 수신 버퍼는 최대 패킷 크기(8 KiB)로 줄이고, 송신 context는 버퍼 없이 WSABUF 배열만 가진다.
    // v7은 수신·송신 각각 64 KiB를 세션마다 고정해 연결당 약 127 KB를 썼다.
    const std::unique_ptr<IoContext> recv_context;
    const std::unique_ptr<IoContext> send_context;

    // closesocket()과 새 overlapped I/O 게시가 겹치지 않도록 socket 접근을 직렬화한다.
    std::mutex socket_mutex;
    // 틱 루프(적재·첫 게시)와 completion worker(이어지는 게시)가 함께 쓰는 송신 상태는 send_mutex로 보호한다.
    std::mutex send_mutex;
    std::deque<SendBuffer> send_queue;
    std::size_t send_queued_bytes = 0;
    std::size_t send_offset = 0;
    bool send_pending = false;
    std::atomic<bool> closing = false;

    // 틱 루프만 읽고 쓰는 틱당 채팅 수 집계(V8_CHATS_PER_TICK)
    std::uint64_t chat_tick = 0;
    std::uint32_t chats_in_tick = 0;

    // 틱 루프만 읽고 쓰는 입력 만료 상태(V8_INPUT_TIMEOUT_MS). 마지막 C_Move가 온 틱, 그 입력 순번,
    // 마지막으로 0이 아닌 속도를 요청했는지(이미 멈춘 플레이어는 다시 멈출 필요가 없다).
    std::uint64_t last_move_tick = 0;
    std::uint32_t last_input_seq = 0;
    bool moving = false;
};

class IocpWorldServer{
    private:
        enum class ServerState{
            Creating,
            Running,
            Stopping,
            Cleaning,
            Stopped,
        };

        enum class LogLevel{
            Debug,
            Info,
            Error,
        };

        int server_port;
        SOCKET listen_socket = INVALID_SOCKET;
        HANDLE iocp_handle = nullptr;
        std::vector<std::thread> completion_workers;
        std::mutex server_mutex;
        std::condition_variable run_cv;
        std::atomic<ServerState> server_state = ServerState::Creating;

        LPFN_ACCEPTEX lpfn_acceptex = nullptr;
        LPFN_GETACCEPTEXSOCKADDRS lpfn_get_accept_ex_sockaddrs = nullptr;
        bool winsock_init_state = false;
        WSADATA wsaData{};

        // v8: 목록에는 종료 시 accept socket을 닫아야 하는 Accept context만 둔다.
        // 모든 pending I/O(Accept/Recv/Send/송신 요청)의 개수는 pending_io_count로 센다.
        std::unordered_set<IoContext*> pending_contexts;
        std::atomic<std::size_t> pending_io_count = 0;
        std::mutex context_mutex;
        std::condition_variable context_condition;

        std::unordered_set<std::shared_ptr<ClientSession>> sessions;
        std::mutex session_mutex;
        std::atomic<std::uint32_t> next_session_id = 1;

        // network(IOCP worker)와 틱 루프를 잇는 큐와 틱 루프 스레드
        GameJobQueue game_jobs;
        std::thread game_thread;
        const std::chrono::milliseconds tick_period;
        const std::chrono::milliseconds status_interval;

        // 아래 상태는 틱 루프 스레드만 읽고 쓰므로 mutex 없이 사용한다.
        World world;
        const std::uint32_t chats_per_tick;
        // 입력 만료: 이만큼의 틱 동안 C_Move가 없으면 그 플레이어의 속도를 0으로 만든다. 0이면 끈다.
        // 모바일 앱이 정지 패킷을 못 보내고 멈추거나 끊겨도 캐릭터가 계속 걷지 않게 하는 안전장치다.
        const std::uint64_t input_timeout_ticks;
        // 개발용 지정 스폰(V8_SPAWN_X/Y/RADIUS). 셋 다 있으면 무작위 스폰 대신 그 원 안에 스폰한다.
        struct SpawnArea{
            float x = 0.0f;
            float y = 0.0f;
            float radius = 0.0f;
        };
        const std::optional<SpawnArea> spawn_area;
        std::mt19937 spawn_rng{std::random_device{}()};
        std::unordered_map<std::uint32_t, std::shared_ptr<ClientSession>> players;
        // 이번 틱에 세션별로 보낼 바이트. 틱이 끝날 때 세션마다 WSASend 요청 하나로 내보낸다.
        std::unordered_map<std::uint32_t, std::vector<char>> outbox;
        // 지금 처리 중인 틱 번호. 세션별 틱당 채팅 수를 셀 때 쓴다.
        std::uint64_t current_tick = 0;
        std::vector<std::shared_ptr<ClientSession>> send_posts;

        // job 큐가 높은 수위를 넘으면 다음 WSARecv를 미뤄 TCP 흐름 제어로 클라이언트 송신을 늦춘다.
        const std::size_t job_high_watermark;
        const std::size_t job_low_watermark;
        std::vector<std::shared_ptr<ClientSession>> paused_sessions;
        std::mutex paused_mutex;
        std::atomic<std::size_t> paused_count = 0;

        AsyncLogger logger;
        LogLevel minimum_log_level = Read_Log_Level_From_Env();
        std::atomic<std::uint64_t> partial_send_count = 0;
        std::atomic<std::uint64_t> processed_job_count = 0;
        std::atomic<std::uint64_t> discarded_job_count = 0;
        std::atomic<std::uint64_t> chat_count = 0;
        std::atomic<std::uint64_t> move_input_count = 0;
        std::atomic<std::uint64_t> expired_input_count = 0;
        std::atomic<std::uint64_t> dropped_chat_count = 0;
        std::atomic<std::uint64_t> rejected_job_count = 0;
        std::atomic<std::uint64_t> recv_pause_count = 0;
        std::atomic<std::uint64_t> send_post_count = 0;
        std::atomic<std::uint64_t> send_batch_count = 0;

        // 틱 지표. 틱 루프가 쓰고 monitor(Server_Run)가 Log_Status에서 읽는다.
        std::atomic<std::uint64_t> tick_count = 0;
        std::atomic<std::uint64_t> late_tick_count = 0;
        std::atomic<std::uint64_t> skipped_tick_count = 0;
        std::atomic<std::uint64_t> window_tick_count = 0;      // 지난 Log_Status 이후 틱 수
        std::atomic<std::uint64_t> window_tick_us_sum = 0;     // 그 틱들의 처리 시간 합(µs)
        std::atomic<std::uint64_t> window_tick_us_max = 0;
        std::atomic<std::uint64_t> window_sent_bytes = 0;      // 그 틱들이 만든 송신 바이트
        std::atomic<std::uint64_t> total_tick_us_max = 0;
        // 틱 단계별 처리 시간 합(µs): 입력 처리, 월드 이동, 시야 변경 수집, 패킷 인코딩, 송신 요청
        std::array<std::atomic<std::uint64_t>, 5> window_phase_us{};
        std::atomic<std::size_t> player_count = 0;
        std::atomic<std::size_t> max_cell_population = 0;

        bool Init_Winsock();
        bool Create_Listen_Socket();
        bool Get_AcceptEx();
        bool Create_Completion_Port();
        void Start_Completion_Workers();

        bool Post_Accept();
        bool Post_Recv(const std::shared_ptr<ClientSession>& session);
        // 큐에 넣고, WSASend 게시가 필요하면 posts_to_dispatch에 세션을 추가한다(게시는 Dispatch_Send_Posts가 맡긴다).
        bool Enqueue_Send(const std::shared_ptr<ClientSession>& session, const SendBuffer& data,
            std::vector<std::shared_ptr<ClientSession>>& posts_to_dispatch);
        bool Post_Next_Send_Locked(const std::shared_ptr<ClientSession>& session);
        // 틱 루프용: WSASend 게시가 필요한 세션들을 묶음으로 나눠 IOCP worker에게 넘긴다.
        void Dispatch_Send_Posts(std::vector<std::shared_ptr<ClientSession>>& pending_posts);
        void Handle_Send_Post_Batch(SendPostBatch* batch);

        void Completion_worker_Loop(unsigned int index);
        void Handle_Accept_Completion(IoContext* context, bool succeeded, unsigned int worker_index);
        void Handle_Recv_Completion(IoContext* context, bool succeeded, DWORD byte_transferred, unsigned int worker_index);
        void Handle_Send_Completion(IoContext* context, bool succeeded, DWORD byte_transferred, unsigned int worker_index);
        bool Submit_Job(GameJob job);
        void Pause_Recv(const std::shared_ptr<ClientSession>& session);
        void Resume_Paused_Sessions();

        // 틱 루프: 대기 → 입력 처리 → 월드 갱신 → 상태 전송
        void Game_Tick_Loop();
        void Process_Job(GameJob& job);
        void Handle_Enter(const std::shared_ptr<ClientSession>& session);
        void Handle_Leave(const std::shared_ptr<ClientSession>& session);
        void Handle_Packet(const std::shared_ptr<ClientSession>& session, const protocol::Packet& packet);
        void Flush_Tick_Output();
        void Record_Tick_Time(std::uint64_t elapsed_us);

        void Log_Status();
        // Recv/Send pending 수 증감. 전역 잠금 없이 원자 카운터만 바꾼다.
        void Track_Context(IoContext* context);
        void Untrack_Context(IoContext* context);
        void Release_Io();
        void Finish_Context(IoContext* context);
        void Close_Session(const std::shared_ptr<ClientSession>& session);
        void Clean_Up();
        void Proceed_Stop();
        void Logging(LogLevel level, const std::string& message);
        // V8_LOG_LEVEL 환경변수(debug/info/error)로 로그 노출 정도를 조절한다.
        static LogLevel Read_Log_Level_From_Env();
        // 테스트와 측정에서 틱 주기, 셀 크기, 수위 등을 환경변수로 덮어쓸 수 있다. 범위 밖이면 기본값을 쓴다.
        static double Read_Number_From_Env(const char* name, double default_value, double min_value, double max_value);
        static World::Config Read_World_Config_From_Env();
        static std::optional<SpawnArea> Read_Spawn_Area_From_Env();
        // 입력 만료 시간(ms)을 틱 수로 바꾼다. 0이면 0(끔), 그 밖에는 최소 1틱.
        static std::uint64_t Input_Timeout_Ticks(double timeout_ms, long long tick_ms);
        // 입력 만료가 지난 플레이어를 멈춘다(틱 루프에서 입력 처리 뒤, 월드 이동 전에 호출).
        void Expire_Idle_Inputs();

    public:
        explicit IocpWorldServer(int port);
        ~IocpWorldServer();

        void Server_Run();
        void Stop();
};
