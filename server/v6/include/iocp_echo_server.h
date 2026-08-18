#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mswsock.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

struct ClientSession{
    explicit ClientSession(SOCKET socket_value) : client_socket(socket_value){}

    // Session은 연결의 수명과 여러 I/O 작업이 함께 사용하는 송신 상태를 소유한다.
    // IoContext는 이 객체를 shared_ptr로 잡아 completion 전까지 세션이 파괴되지 않게 한다.
    SOCKET client_socket = INVALID_SOCKET;
    // accept 시점에 GetAcceptExSockaddrs로 파싱해 두어, 이후 모든 로그에서
    // socket 핸들 숫자 대신 사람이 읽을 수 있는 IP:port로 클라이언트를 추적할 수 있게 한다.
    std::string remote_endpoint = "unknown";
    // closesocket()과 새 overlapped I/O 게시가 겹치지 않도록 socket 접근을 직렬화한다.
    std::mutex socket_mutex;
    // Recv completion 여러 개가 데이터를 넣더라도 echo 순서는 이 큐에서 보장한다.
    std::mutex send_mutex;
    std::deque<std::vector<char>> send_queue;
    std::size_t send_offset = 0;
    // 세션당 WSASend 하나만 pending으로 두어 큐 앞에서부터 순서대로 전송한다.
    bool send_pending = false;
    std::atomic<bool> closing = false;
};

struct IoContext{
    enum class IoType{
        Accept,
        Recv,
        Send,
    };

    // Context 하나는 정확히 하나의 pending overlapped 작업만 표현한다.
    IoType io_type = IoType::Accept;
    SOCKET accept_socket = INVALID_SOCKET;
    std::shared_ptr<ClientSession> session;
    WSABUF wsa_buffer{};
    // 테스트에서 실제 요청 크기를 줄여도 원래 남은 송신량과 비교할 수 있도록 따로 보관한다.
    std::size_t logical_send_length = 0;
    DWORD flags = 0;
    char buffer[64 * 1024]{};
    // 첫 멤버가 아니므로 completion에서는 CONTAINING_RECORD로 IoContext를 복원한다.
    OVERLAPPED overlapped{};
};

class IocpEchoServer{
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

        std::mutex log_mutex;
        LogLevel minimum_log_level = Read_Log_Level_From_Env();
        std::atomic<std::uint64_t> partial_send_count = 0;

        bool Init_Winsock();
        bool Create_Listen_Socket();
        bool Get_AcceptEx();
        bool Create_Completion_Port();
        void Start_Completion_Workers();

        bool Post_Accept();
        bool Post_Recv(const std::shared_ptr<ClientSession>& session);
        bool Enqueue_Send(const std::shared_ptr<ClientSession>& session, const char* data, std::size_t length);
        bool Post_Next_Send_Locked(const std::shared_ptr<ClientSession>& session);

        void Completion_worker_Loop(unsigned int index);
        void Handle_Accept_Completion(IoContext* context, bool succeeded, unsigned int worker_index);
        void Handle_Recv_Completion(IoContext* context, bool succeeded, DWORD byte_transferred, unsigned int worker_index);
        void Handle_Send_Completion(IoContext* context, bool succeeded, DWORD byte_transferred, unsigned int worker_index);

        void Finish_Context(IoContext* context);
        void Close_Session(const std::shared_ptr<ClientSession>& session);
        void Clean_Up();
        void Proceed_Stop();
        void Logging(LogLevel level, const std::string& message);
        // V6_LOG_LEVEL 환경변수(debug/info/error)로 접속·워커·recv/send 상세 로그 노출 정도를 조절한다.
        static LogLevel Read_Log_Level_From_Env();

    public:
        explicit IocpEchoServer(int port);
        ~IocpEchoServer();

        void Server_Run();
        void Stop();
};
