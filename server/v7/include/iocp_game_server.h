#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mswsock.h>

#include <async_logger.h>
#include <game_job_queue.h>
#include <packet_assembler.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 브로드캐스트 시 같은 패킷을 세션 수만큼 복사하지 않도록 불변 버퍼를 공유한다.
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
    // 세션의 Recv/Send context는 세션이 소유하며 completion 후 다시 사용한다.
    const IoType io_type;
    SOCKET accept_socket = INVALID_SOCKET;
    // pending인 동안 세션 수명을 붙잡는다. completion 처리 시작 시 비워서 순환 참조를 끊는다.
    std::shared_ptr<ClientSession> session;
    WSABUF wsa_buffer{};
    std::size_t logical_send_length = 0;
    DWORD flags = 0;
    std::vector<char> buffer;
    // 첫 멤버가 아니므로 completion에서는 CONTAINING_RECORD로 IoContext를 복원한다.
    OVERLAPPED overlapped{};
};

struct ClientSession{
    ClientSession(SOCKET socket_value, std::uint32_t id);

    SOCKET client_socket = INVALID_SOCKET;
    // socket 핸들은 재사용될 수 있으므로 게임 로직과 클라이언트에는 별도의 증가 id를 사용한다.
    const std::uint32_t session_id;
    std::string remote_endpoint = "unknown";

    // 세션당 WSARecv는 하나만 pending이므로 completion worker 하나만 동시에 접근한다. 락이 필요 없다.
    PacketAssembler assembler;

    // 세션당 Recv와 Send는 각각 최대 하나만 pending이므로 context를 하나씩 두고 재사용한다.
    // I/O마다 64 KiB context를 new/delete하던 v6 구조의 할당 비용을 없앤다.
    const std::unique_ptr<IoContext> recv_context;
    const std::unique_ptr<IoContext> send_context;

    // closesocket()과 새 overlapped I/O 게시가 겹치지 않도록 socket 접근을 직렬화한다.
    std::mutex socket_mutex;
    // logic 스레드(적재·첫 게시)와 completion worker(이어지는 게시)가 함께 쓰는 송신 상태는 send_mutex로 보호한다.
    std::mutex send_mutex;
    std::deque<SendBuffer> send_queue;
    // 메모리 보호용 상한은 메시지 개수가 아니라 아직 보내지 않은 바이트 수로 판단한다.
    std::size_t send_queued_bytes = 0;
    std::size_t send_offset = 0;
    // WSASend가 진행 중이면 true. 세션당 송신 흐름을 하나로 유지해 순서를 보장한다.
    bool send_pending = false;
    std::atomic<bool> closing = false;
};

class IocpGameServer{
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

        // OS가 아직 참조할 수 있는 context 목록. completion worker만 목록에서 제거한다.
        std::unordered_set<IoContext*> pending_contexts;
        std::mutex context_mutex;
        std::condition_variable context_condition;

        // 활성 연결을 소유하며 Stop 시 모든 socket을 닫아 pending I/O 취소를 시작한다.
        std::unordered_set<std::shared_ptr<ClientSession>> sessions;
        std::mutex session_mutex;
        std::atomic<std::uint32_t> next_session_id = 1;

        // network(IOCP worker)와 game logic을 분리하는 큐와 단일 logic 스레드.
        GameJobQueue game_jobs;
        std::thread game_thread;
        // 방 상태는 game logic 스레드만 읽고 쓰므로 mutex 없이 사용한다.
        std::unordered_map<std::uint32_t, std::shared_ptr<ClientSession>> room_members;

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
        std::atomic<std::uint64_t> rejected_job_count = 0;
        std::atomic<std::uint64_t> recv_pause_count = 0;
        std::atomic<std::uint64_t> send_post_count = 0;

        bool Init_Winsock();
        bool Create_Listen_Socket();
        bool Get_AcceptEx();
        bool Create_Completion_Port();
        void Start_Completion_Workers();

        bool Post_Accept();
        bool Post_Recv(const std::shared_ptr<ClientSession>& session);
        bool Enqueue_Send(const std::shared_ptr<ClientSession>& session, const SendBuffer& data);
        bool Post_Next_Send_Locked(const std::shared_ptr<ClientSession>& session);

        void Completion_worker_Loop(unsigned int index);
        void Handle_Accept_Completion(IoContext* context, bool succeeded, unsigned int worker_index);
        void Handle_Recv_Completion(IoContext* context, bool succeeded, DWORD byte_transferred, unsigned int worker_index);
        void Handle_Send_Completion(IoContext* context, bool succeeded, DWORD byte_transferred, unsigned int worker_index);
        bool Submit_Job(GameJob job);
        void Pause_Recv(const std::shared_ptr<ClientSession>& session);
        void Resume_Paused_Sessions();

        void Game_Logic_Loop();
        void Handle_Enter(const std::shared_ptr<ClientSession>& session);
        void Handle_Leave(const std::shared_ptr<ClientSession>& session);
        void Handle_Packet(const std::shared_ptr<ClientSession>& session, const protocol::Packet& packet);
        void Broadcast(const SendBuffer& data);

        void Log_Status();
        void Finish_Context(IoContext* context);
        void Close_Session(const std::shared_ptr<ClientSession>& session);
        void Clean_Up();
        void Proceed_Stop();
        void Logging(LogLevel level, const std::string& message);
        // V7_LOG_LEVEL 환경변수(debug/info/error)로 로그 노출 정도를 조절한다.
        static LogLevel Read_Log_Level_From_Env();
        // 테스트에서 backpressure를 작은 부하로 재현할 수 있도록 수위를 환경변수로 덮어쓸 수 있다.
        static std::size_t Read_Size_From_Env(const char* name, std::size_t default_value);

    public:
        explicit IocpGameServer(int port);
        ~IocpGameServer();

        void Server_Run();
        void Stop();
};
