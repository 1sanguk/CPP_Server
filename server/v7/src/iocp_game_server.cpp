#include <iocp_game_server.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

constexpr int kBacklog = 128;
constexpr unsigned int kAcceptDepth = 16;
constexpr auto kCleanupDiagnosticInterval = std::chrono::seconds(5);
constexpr auto kStatusInterval = std::chrono::seconds(10);
constexpr DWORD kAcceptAddressLength = sizeof(sockaddr_in) + 16;
constexpr std::size_t kIoBufferSize = 64 * 1024;
// completion worker가 logic 스레드보다 빠르게 job을 쌓을 때의 최종 상한. 평소에는 아래 수위로
// Recv를 멈춰 이 상한에 닿지 않게 한다.
constexpr std::size_t kGameJobCapacity = 65536;
constexpr std::size_t kDefaultJobHighWatermark = 8192;
constexpr std::size_t kDefaultJobLowWatermark = 1024;
// 응답을 읽지 않는 클라이언트 때문에 브로드캐스트 버퍼가 무한히 쌓이지 않게 하는 세션별 바이트 상한.
constexpr std::size_t kMaxSendQueueBytes = 4 * 1024 * 1024;

ClientSession::ClientSession(SOCKET socket_value, std::uint32_t id)
    : client_socket(socket_value), session_id(id),
      recv_context(std::make_unique<IoContext>(IoContext::IoType::Recv, kIoBufferSize)),
      send_context(std::make_unique<IoContext>(IoContext::IoType::Send, kIoBufferSize)){}

IocpGameServer::IocpGameServer(int port)
    : server_port(port), game_jobs(kGameJobCapacity),
      job_high_watermark(Read_Size_From_Env("V7_JOB_HIGH_WATERMARK", kDefaultJobHighWatermark)),
      job_low_watermark((std::min)(Read_Size_From_Env("V7_JOB_LOW_WATERMARK", kDefaultJobLowWatermark),
          job_high_watermark / 2)),
      logger(std::cout){
    this->logger.Start();
}

IocpGameServer::~IocpGameServer(){
    this->Proceed_Stop();
    // 종료 과정의 마지막 로그까지 출력한 뒤 writer 스레드를 정리한다.
    this->logger.Stop();
}

IocpGameServer::LogLevel IocpGameServer::Read_Log_Level_From_Env(){
    char buffer[16]{};
    DWORD length = GetEnvironmentVariableA("V7_LOG_LEVEL", buffer, sizeof(buffer));
    if(length == 0 || length >= sizeof(buffer)){
        return LogLevel::Info;
    }
    std::string value(buffer, length);
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if(value == "debug") return LogLevel::Debug;
    if(value == "error") return LogLevel::Error;
    return LogLevel::Info;
}

std::size_t IocpGameServer::Read_Size_From_Env(const char* name, std::size_t default_value){
    char buffer[32]{};
    DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if(length == 0 || length >= sizeof(buffer)){
        return default_value;
    }
    const unsigned long long value = std::strtoull(buffer, nullptr, 10);
    if(value == 0 || value >= kGameJobCapacity){
        return default_value;
    }
    return static_cast<std::size_t>(value);
}

void IocpGameServer::Logging(LogLevel level, const std::string& message){
    if(level < this->minimum_log_level){
        return;
    }
    // 출력은 로거 스레드가 한다. stdout이 막혀도 호출한 스레드는 기다리지 않는다.
    this->logger.Write(message);
}

bool IocpGameServer::Init_Winsock(){
    if(this->winsock_init_state){
        return true;
    }
    int startup_result = WSAStartup(MAKEWORD(2,2), &this->wsaData);
    if(startup_result != 0){
        this->Logging(LogLevel::Error, "[IocpGameServer] WSAStartup failed: " + std::to_string(startup_result));
        return false;
    }
    this->winsock_init_state = true;
    this->Logging(LogLevel::Info, "[IocpGameServer] Winsock initialized");
    return true;
}

bool IocpGameServer::Create_Listen_Socket(){
    if(this->server_port < 0 || this->server_port > 65535){
        this->Logging(LogLevel::Error, "[IocpGameServer] invalid port: " + std::to_string(this->server_port));
        return false;
    }

    this->listen_socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if(this->listen_socket == INVALID_SOCKET){
        this->Logging(LogLevel::Error, "[IocpGameServer] listen socket creation failed: " + std::to_string(WSAGetLastError()));
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<u_short>(this->server_port));

    if(bind(this->listen_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR){
        this->Logging(LogLevel::Error, "[IocpGameServer] bind failed: " + std::to_string(WSAGetLastError()));
        closesocket(this->listen_socket);
        this->listen_socket = INVALID_SOCKET;
        return false;
    }
    if(listen(this->listen_socket, kBacklog) == SOCKET_ERROR){
        this->Logging(LogLevel::Error, "[IocpGameServer] listen failed: " + std::to_string(WSAGetLastError()));
        closesocket(this->listen_socket);
        this->listen_socket = INVALID_SOCKET;
        return false;
    }

    this->Logging(LogLevel::Info, "[IocpGameServer] listening on port " + std::to_string(this->server_port));
    return true;
}

bool IocpGameServer::Get_AcceptEx(){
    GUID accept_guid = WSAID_ACCEPTEX;
    DWORD bytes_returned{};
    int result = WSAIoctl(this->listen_socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &accept_guid, sizeof(accept_guid), &this->lpfn_acceptex, sizeof(this->lpfn_acceptex),
        &bytes_returned, nullptr, nullptr);
    if(result == SOCKET_ERROR){
        this->Logging(LogLevel::Error, "[IocpGameServer] WSAIoctl(AcceptEx) failed: " + std::to_string(WSAGetLastError()));
        return false;
    }

    GUID sockaddrs_guid = WSAID_GETACCEPTEXSOCKADDRS;
    result = WSAIoctl(this->listen_socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &sockaddrs_guid, sizeof(sockaddrs_guid), &this->lpfn_get_accept_ex_sockaddrs, sizeof(this->lpfn_get_accept_ex_sockaddrs),
        &bytes_returned, nullptr, nullptr);
    if(result == SOCKET_ERROR){
        this->Logging(LogLevel::Error, "[IocpGameServer] WSAIoctl(GetAcceptExSockaddrs) failed: " + std::to_string(WSAGetLastError()));
        return false;
    }

    this->Logging(LogLevel::Info, "[IocpGameServer] AcceptEx extension functions loaded");
    return true;
}

bool IocpGameServer::Create_Completion_Port(){
    this->iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if(this->iocp_handle == nullptr){
        this->Logging(LogLevel::Error, "[IocpGameServer] IOCP creation failed: " + std::to_string(GetLastError()));
        return false;
    }
    if(CreateIoCompletionPort(reinterpret_cast<HANDLE>(this->listen_socket), this->iocp_handle, 0, 0) == nullptr){
        this->Logging(LogLevel::Error, "[IocpGameServer] listen IOCP registration failed: " + std::to_string(GetLastError()));
        return false;
    }
    this->Logging(LogLevel::Info, "[IocpGameServer] completion port created");
    return true;
}

void IocpGameServer::Start_Completion_Workers(){
    unsigned int detected = std::thread::hardware_concurrency();
    unsigned int count = detected == 0 ? 1 : detected;
    this->completion_workers.reserve(count);
    for(unsigned int i=0; i<count; ++i){
        this->completion_workers.emplace_back(&IocpGameServer::Completion_worker_Loop, this, i);
    }
    this->Logging(LogLevel::Info,
        "[IocpGameServer] completion workers started, count=" + std::to_string(count));
}

bool IocpGameServer::Post_Accept(){
    // Accept는 세션이 생기기 전 작업이라 context를 매번 만든다. 접속 빈도는 송수신보다 훨씬 낮다.
    auto* context = new IoContext(IoContext::IoType::Accept, 2 * kAcceptAddressLength);
    context->accept_socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if(context->accept_socket == INVALID_SOCKET){
        this->Logging(LogLevel::Error, "[IocpGameServer] accept socket creation failed: " + std::to_string(WSAGetLastError()));
        delete context;
        return false;
    }

    DWORD bytes_received{};
    int error = 0;
    {
        std::lock_guard<std::mutex> lock(this->context_mutex);
        if(this->server_state.load() != ServerState::Running){
            closesocket(context->accept_socket);
            delete context;
            return false;
        }
        this->pending_contexts.emplace(context);
        bool result = this->lpfn_acceptex(this->listen_socket, context->accept_socket, context->buffer.data(), 0,
            kAcceptAddressLength, kAcceptAddressLength, &bytes_received, &context->overlapped);
        if(!result){
            error = WSAGetLastError();
            if(error != WSA_IO_PENDING){
                this->pending_contexts.erase(context);
            }
        }
    }

    if(error != 0 && error != WSA_IO_PENDING){
        this->Logging(LogLevel::Error, "[IocpGameServer] AcceptEx failed: " + std::to_string(error));
        closesocket(context->accept_socket);
        delete context;
        this->context_condition.notify_all();
        return false;
    }
    this->Logging(LogLevel::Debug, "[IocpGameServer] AcceptEx posted");
    return true;
}

bool IocpGameServer::Post_Recv(const std::shared_ptr<ClientSession>& session){
    // 세션의 Recv context는 이전 Recv completion이 끝난 뒤에만 다시 게시되므로 재사용해도 안전하다.
    IoContext* context = session->recv_context.get();

    int error = 0;
    {
        std::lock_guard<std::mutex> socket_lock(session->socket_mutex);
        std::lock_guard<std::mutex> context_lock(this->context_mutex);
        if(this->server_state.load() != ServerState::Running || session->closing.load()){
            return false;
        }
        context->session = session;
        context->overlapped = OVERLAPPED{};
        context->flags = 0;
        context->wsa_buffer.buf = context->buffer.data();
        context->wsa_buffer.len = static_cast<ULONG>(context->buffer.size());
        this->pending_contexts.emplace(context);
        int result = WSARecv(session->client_socket, &context->wsa_buffer, 1, nullptr,
            &context->flags, &context->overlapped, nullptr);
        if(result == SOCKET_ERROR){
            error = WSAGetLastError();
            if(error != WSA_IO_PENDING){
                this->pending_contexts.erase(context);
                context->session.reset();
            }
        }
    }

    if(error != 0 && error != WSA_IO_PENDING){
        this->Logging(LogLevel::Error, "[IocpGameServer] WSARecv failed: " + std::to_string(error));
        this->context_condition.notify_all();
        return false;
    }
    this->Logging(LogLevel::Debug,
        "[IocpGameServer] WSARecv posted, session=" + std::to_string(session->session_id));
    return true;
}

bool IocpGameServer::Post_Next_Send_Locked(const std::shared_ptr<ClientSession>& session){
    // 호출자는 send_mutex를 보유한다. 송신이 멈춰 있던 세션은 logic 스레드(Enqueue_Send)가,
    // 이어지는 송신은 Send completion을 받은 IOCP worker가 게시한다.
    if(session->send_queue.empty() || session->closing.load()){
        session->send_pending = false;
        return !session->closing.load();
    }

    // 큐에 쌓인 여러 패킷을 context 버퍼 하나에 이어 붙여 WSASend 한 번으로 보낸다.
    // 방송이 몰릴 때 패킷마다 시스템 호출을 하던 비용을 줄인다.
    IoContext* context = session->send_context.get();
    const std::size_t capacity = context->buffer.size();
    std::size_t copied = 0;
    std::size_t offset = session->send_offset;
    for(const SendBuffer& data : session->send_queue){
        const std::size_t length = (std::min)(data->size() - offset, capacity - copied);
        std::copy_n(data->data() + offset, length, context->buffer.data() + copied);
        copied += length;
        offset = 0;
        if(copied == capacity){
            break;
        }
    }

    context->logical_send_length = copied;
    context->wsa_buffer.buf = context->buffer.data();
    context->wsa_buffer.len = static_cast<ULONG>(copied);

    int error = 0;
    {
        std::lock_guard<std::mutex> socket_lock(session->socket_mutex);
        std::lock_guard<std::mutex> context_lock(this->context_mutex);
        if(this->server_state.load() != ServerState::Running || session->closing.load()){
            session->send_pending = false;
            return false;
        }
        context->session = session;
        context->overlapped = OVERLAPPED{};
        this->pending_contexts.emplace(context);
        int result = WSASend(session->client_socket, &context->wsa_buffer, 1, nullptr, 0,
            &context->overlapped, nullptr);
        if(result == SOCKET_ERROR){
            error = WSAGetLastError();
            if(error != WSA_IO_PENDING){
                this->pending_contexts.erase(context);
                context->session.reset();
            }
        }
    }

    if(error != 0 && error != WSA_IO_PENDING){
        this->Logging(LogLevel::Error, "[IocpGameServer] WSASend failed: " + std::to_string(error));
        session->send_pending = false;
        this->context_condition.notify_all();
        return false;
    }
    ++this->send_post_count;
    return true;
}

bool IocpGameServer::Enqueue_Send(const std::shared_ptr<ClientSession>& session, const SendBuffer& data){
    // logic 스레드에서 호출된다. 이미 WSASend가 진행 중이면 큐에만 넣는다. 그동안 쌓인 패킷은
    // Send completion 때 한 번에 묶여 나가므로, 방송이 몰릴수록 시스템 호출 수가 줄어든다.
    // 락 안에서는 상태만 정하고 Close_Session은 락을 푼 뒤 호출해 send_mutex 재잠금 데드락을 피한다.
    if(!data || data->empty()){
        return false;
    }
    bool overflow = false;
    bool result = true;
    {
        std::lock_guard<std::mutex> lock(session->send_mutex);
        if(session->closing.load()){
            return false;
        }
        if(session->send_queued_bytes + data->size() > kMaxSendQueueBytes){
            overflow = true;
            result = false;
        }else{
            session->send_queue.emplace_back(data);
            session->send_queued_bytes += data->size();
            if(!session->send_pending){
                session->send_pending = true;
                result = this->Post_Next_Send_Locked(session);
            }
        }
    }
    if(overflow){
        this->Logging(LogLevel::Error,
            "[IocpGameServer] send queue overflow, closing slow client session=" + std::to_string(session->session_id) +
            ", queued_bytes_limit=" + std::to_string(kMaxSendQueueBytes));
    }
    if(!result){
        this->Close_Session(session);
    }
    return result;
}

void IocpGameServer::Finish_Context(IoContext* context){
    {
        std::lock_guard<std::mutex> lock(this->context_mutex);
        this->pending_contexts.erase(context);
    }
    this->context_condition.notify_all();
}

bool IocpGameServer::Submit_Job(GameJob job){
    if(this->game_jobs.Push(std::move(job))){
        return true;
    }
    if(this->server_state.load() == ServerState::Running){
        ++this->rejected_job_count;
    }
    return false;
}

void IocpGameServer::Pause_Recv(const std::shared_ptr<ClientSession>& session){
    // 다음 WSARecv를 게시하지 않으면 서버 쪽 TCP 수신 버퍼가 차고, 클라이언트의 송신이 흐름 제어로 느려진다.
    {
        std::lock_guard<std::mutex> lock(this->paused_mutex);
        this->paused_sessions.push_back(session);
        this->paused_count.store(this->paused_sessions.size());
    }
    ++this->recv_pause_count;
    this->Logging(LogLevel::Debug,
        "[IocpGameServer] job queue above high watermark, recv paused for session=" + std::to_string(session->session_id));
    // logic 스레드가 이미 큐를 비우고 잠들었을 수 있으므로, 넣은 쪽에서도 한 번 재개 조건을 확인한다.
    this->Resume_Paused_Sessions();
}

void IocpGameServer::Resume_Paused_Sessions(){
    if(this->paused_count.load() == 0 || this->game_jobs.Size() > this->job_low_watermark){
        return;
    }
    std::vector<std::shared_ptr<ClientSession>> to_resume;
    {
        std::lock_guard<std::mutex> lock(this->paused_mutex);
        to_resume.swap(this->paused_sessions);
        this->paused_count.store(0);
    }
    for(const auto& session : to_resume){
        if(!this->Post_Recv(session)){
            this->Close_Session(session);
        }
    }
}

void IocpGameServer::Close_Session(const std::shared_ptr<ClientSession>& session){
    // 여러 completion이 동시에 실패해도 실제 close와 목록 제거는 한 번만 수행한다.
    if(!session || session->closing.exchange(true)){
        return;
    }

    SOCKET socket_to_close = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(session->socket_mutex);
        socket_to_close = session->client_socket;
        session->client_socket = INVALID_SOCKET;
    }
    {
        std::lock_guard<std::mutex> lock(session->send_mutex);
        session->send_queue.clear();
        session->send_queued_bytes = 0;
        session->send_offset = 0;
        session->send_pending = false;
    }
    if(socket_to_close != INVALID_SOCKET){
        closesocket(socket_to_close);
    }

    std::size_t remaining_sessions = 0;
    {
        std::lock_guard<std::mutex> lock(this->session_mutex);
        this->sessions.erase(session);
        remaining_sessions = this->sessions.size();
    }

    // closing을 세운 뒤에 Leave를 넣는다. Enter보다 Leave가 먼저 처리되더라도
    // Handle_Enter가 closing을 확인하므로 닫힌 세션이 방에 남지 않는다.
    GameJob leave;
    leave.type = GameJob::JobType::Leave;
    leave.session = session;
    this->Submit_Job(std::move(leave));

    this->Logging(LogLevel::Debug,
        "[IocpGameServer] client disconnected, session=" + std::to_string(session->session_id) +
        ", remote=" + session->remote_endpoint +
        ", active_sessions=" + std::to_string(remaining_sessions));
}

void IocpGameServer::Handle_Accept_Completion(IoContext* context, bool succeeded, unsigned int worker_index){
    SOCKET accepted_socket = context->accept_socket;
    context->accept_socket = INVALID_SOCKET;

    if(this->server_state.load() != ServerState::Running){
        if(accepted_socket != INVALID_SOCKET){
            closesocket(accepted_socket);
        }
        delete context;
        return;
    }

    if(!succeeded){
        if(accepted_socket != INVALID_SOCKET){
            closesocket(accepted_socket);
        }
        delete context;
        this->Post_Accept();
        return;
    }

    if(setsockopt(accepted_socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
        reinterpret_cast<char*>(&this->listen_socket), sizeof(this->listen_socket)) == SOCKET_ERROR){
        this->Logging(LogLevel::Error, "[IocpGameServer] SO_UPDATE_ACCEPT_CONTEXT failed: " + std::to_string(WSAGetLastError()));
        closesocket(accepted_socket);
        delete context;
        this->Post_Accept();
        return;
    }

    if(CreateIoCompletionPort(reinterpret_cast<HANDLE>(accepted_socket), this->iocp_handle, 0, 0) == nullptr){
        this->Logging(LogLevel::Error, "[IocpGameServer] client IOCP registration failed: " + std::to_string(GetLastError()));
        closesocket(accepted_socket);
        delete context;
        this->Post_Accept();
        return;
    }

    std::string remote_endpoint = "unknown";
    if(this->lpfn_get_accept_ex_sockaddrs != nullptr){
        sockaddr* local_addr = nullptr;
        sockaddr* remote_addr = nullptr;
        int local_addr_len = 0;
        int remote_addr_len = 0;
        this->lpfn_get_accept_ex_sockaddrs(context->buffer.data(), 0, kAcceptAddressLength, kAcceptAddressLength,
            &local_addr, &local_addr_len, &remote_addr, &remote_addr_len);
        if(remote_addr != nullptr && remote_addr->sa_family == AF_INET){
            auto* remote_addr_in = reinterpret_cast<sockaddr_in*>(remote_addr);
            char ip_text[INET_ADDRSTRLEN]{};
            if(inet_ntop(AF_INET, &remote_addr_in->sin_addr, ip_text, sizeof(ip_text)) != nullptr){
                remote_endpoint = std::string(ip_text) + ":" + std::to_string(ntohs(remote_addr_in->sin_port));
            }
        }
    }
    delete context;

    auto session = std::make_shared<ClientSession>(accepted_socket, this->next_session_id.fetch_add(1));
    session->remote_endpoint = remote_endpoint;
    std::size_t active_session_count = 0;
    {
        std::lock_guard<std::mutex> lock(this->session_mutex);
        this->sessions.emplace(session);
        active_session_count = this->sessions.size();
    }
    this->Logging(LogLevel::Debug,
        "[IocpGameServer] client connected, worker=" + std::to_string(worker_index) +
        ", session=" + std::to_string(session->session_id) +
        ", remote=" + remote_endpoint +
        ", active_sessions=" + std::to_string(active_session_count));

    this->Post_Accept();

    // Enter는 반드시 첫 Recv 게시 전에 넣는다. 그래야 이 세션의 Packet job이 Enter보다
    // 앞서 logic 스레드에 도착하지 않는다.
    GameJob enter;
    enter.type = GameJob::JobType::Enter;
    enter.session = session;
    if(!this->Submit_Job(std::move(enter))){
        this->Close_Session(session);
        return;
    }
    if(!this->Post_Recv(session)){
        this->Close_Session(session);
    }
}

void IocpGameServer::Handle_Recv_Completion(IoContext* context, bool succeeded, DWORD byte_transferred, unsigned int worker_index){
    // context가 붙잡고 있던 세션 참조를 넘겨받아 세션 ↔ context 순환 참조를 끊는다.
    std::shared_ptr<ClientSession> session = std::move(context->session);
    if(!succeeded || byte_transferred == 0 || this->server_state.load() != ServerState::Running){
        this->Close_Session(session);
        return;
    }

    this->Logging(LogLevel::Debug,
        "[IocpGameServer] recv completed, worker=" + std::to_string(worker_index) +
        ", session=" + std::to_string(session->session_id) +
        ", bytes=" + std::to_string(byte_transferred));

    // 받은 바이트를 조립기에 누적하고, 완성된 패킷만 game job으로 넘긴다.
    // 이 세션의 다음 Recv는 아직 게시 전이므로 조립기와 Recv 버퍼를 쓰는 스레드는 지금 하나뿐이다.
    session->assembler.Append(context->buffer.data(), byte_transferred);

    bool keep_session = true;
    while(auto packet = session->assembler.Next()){
        GameJob job;
        job.type = GameJob::JobType::Packet;
        job.session = session;
        job.packet = std::move(*packet);
        if(!this->Submit_Job(std::move(job))){
            // 수위 기반 일시정지로 보통은 여기까지 오지 않는다. 최종 상한에 닿았을 때만 연결을 끊는다.
            this->Logging(LogLevel::Error,
                "[IocpGameServer] game job queue hard limit reached, closing session=" + std::to_string(session->session_id));
            keep_session = false;
            break;
        }
    }
    if(keep_session && session->assembler.Has_Error()){
        this->Logging(LogLevel::Error,
            "[IocpGameServer] malformed packet header, closing session=" + std::to_string(session->session_id) +
            ", remote=" + session->remote_endpoint);
        keep_session = false;
    }
    if(!keep_session){
        this->Close_Session(session);
        return;
    }

    if(this->game_jobs.Size() >= this->job_high_watermark){
        this->Pause_Recv(session);
        return;
    }
    if(!this->Post_Recv(session)){
        this->Close_Session(session);
    }
}

void IocpGameServer::Handle_Send_Completion(IoContext* context, bool succeeded, DWORD byte_transferred, unsigned int worker_index){
    std::shared_ptr<ClientSession> session = std::move(context->session);
    bool post_result = true;
    {
        std::lock_guard<std::mutex> lock(session->send_mutex);
        if(!succeeded || byte_transferred == 0 || session->closing.load() || session->send_queue.empty()){
            post_result = false;
        }else{
            if(byte_transferred < context->logical_send_length){
                ++this->partial_send_count;
            }
            // 묶어서 보낸 여러 패킷에 걸쳐 완료 바이트만큼 큐를 전진시킨다.
            std::size_t remaining = byte_transferred;
            while(remaining > 0 && !session->send_queue.empty()){
                const std::size_t front_left = session->send_queue.front()->size() - session->send_offset;
                if(remaining >= front_left){
                    remaining -= front_left;
                    session->send_queued_bytes -= session->send_queue.front()->size();
                    session->send_queue.pop_front();
                    session->send_offset = 0;
                }else{
                    session->send_offset += remaining;
                    remaining = 0;
                }
            }
            if(session->send_queue.empty()){
                session->send_pending = false;
            }else{
                post_result = this->Post_Next_Send_Locked(session);
            }
        }
    }
    this->Logging(LogLevel::Debug,
        "[IocpGameServer] send completed, worker=" + std::to_string(worker_index) +
        ", session=" + std::to_string(session->session_id) +
        ", bytes=" + std::to_string(byte_transferred));
    if(!post_result){
        this->Close_Session(session);
    }
}

void IocpGameServer::Completion_worker_Loop(unsigned int index){
    this->Logging(LogLevel::Debug, "[IocpGameServer] worker " + std::to_string(index) +
        " started, thread_id=" + std::to_string(GetCurrentThreadId()));
    while(true){
        DWORD byte_transferred{};
        ULONG_PTR completion_key{};
        LPOVERLAPPED overlapped_ptr{};
        bool succeeded = GetQueuedCompletionStatus(this->iocp_handle, &byte_transferred,
            &completion_key, &overlapped_ptr, INFINITE);
        DWORD completion_error = succeeded ? ERROR_SUCCESS : GetLastError();
        if(overlapped_ptr == nullptr){
            if(!succeeded){
                this->Logging(LogLevel::Error,
                    "[IocpGameServer] GetQueuedCompletionStatus failed without context: " +
                    std::to_string(completion_error));
            }
            break;
        }

        IoContext* context = CONTAINING_RECORD(overlapped_ptr, IoContext, overlapped);
        if(!succeeded){
            // 클라이언트가 RST로 끊는 경우(64, 10054)나 종료 중 취소(995)는 정상적인 흐름이다.
            const bool expected_failure = this->server_state.load() != ServerState::Running ||
                completion_error == ERROR_NETNAME_DELETED || completion_error == WSAECONNRESET ||
                completion_error == ERROR_OPERATION_ABORTED;
            this->Logging(expected_failure ? LogLevel::Debug : LogLevel::Error,
                "[IocpGameServer] overlapped I/O failed, type=" +
                std::to_string(static_cast<int>(context->io_type)) +
                ", error=" + std::to_string(completion_error));
        }
        this->Finish_Context(context);
        switch(context->io_type){
            case IoContext::IoType::Accept:
                this->Handle_Accept_Completion(context, succeeded, index);
                break;
            case IoContext::IoType::Recv:
                this->Handle_Recv_Completion(context, succeeded, byte_transferred, index);
                break;
            case IoContext::IoType::Send:
                this->Handle_Send_Completion(context, succeeded, byte_transferred, index);
                break;
        }
    }
    this->Logging(LogLevel::Debug, "[IocpGameServer] worker " + std::to_string(index) + " stopped");
}

void IocpGameServer::Game_Logic_Loop(){
    this->Logging(LogLevel::Info, "[IocpGameServer] game logic thread started, thread_id=" +
        std::to_string(GetCurrentThreadId()));
    GameJob job;
    while(this->game_jobs.Pop(job)){
        // 종료 중에는 남은 job을 처리하지 않고 버린다. 응답을 보낼 socket이 곧 모두 닫히기 때문이다.
        if(this->server_state.load() != ServerState::Running){
            ++this->discarded_job_count;
            job = GameJob{};
            continue;
        }
        switch(job.type){
            case GameJob::JobType::Enter:
                this->Handle_Enter(job.session);
                break;
            case GameJob::JobType::Leave:
                this->Handle_Leave(job.session);
                break;
            case GameJob::JobType::Packet:
                this->Handle_Packet(job.session, job.packet);
                break;
        }
        ++this->processed_job_count;
        // 다음 Pop까지 세션 참조를 들고 있지 않도록 비운다.
        job = GameJob{};
        // 큐가 낮은 수위 아래로 내려가면 수신을 멈춘 세션들의 Recv를 다시 게시한다.
        this->Resume_Paused_Sessions();
    }
    this->room_members.clear();
    this->Logging(LogLevel::Info, "[IocpGameServer] game logic thread stopped");
}

void IocpGameServer::Broadcast(const SendBuffer& data){
    // Enqueue_Send가 실패하면 Close_Session이 Leave job만 큐에 넣으므로, 순회 중에
    // room_members가 바뀌지 않는다.
    for(const auto& [session_id, member] : this->room_members){
        this->Enqueue_Send(member, data);
    }
}

void IocpGameServer::Handle_Enter(const std::shared_ptr<ClientSession>& session){
    if(session->closing.load()){
        return;
    }
    this->room_members.emplace(session->session_id, session);
    this->Enqueue_Send(session, std::make_shared<const std::vector<char>>(protocol::Make_Welcome(session->session_id)));
    const std::string notice = "Session " + std::to_string(session->session_id) +
        " joined (members=" + std::to_string(this->room_members.size()) + ")";
    this->Broadcast(std::make_shared<const std::vector<char>>(protocol::Make_Notice(notice)));
    this->Logging(LogLevel::Info, "[Game] " + notice + ", remote=" + session->remote_endpoint);
}

void IocpGameServer::Handle_Leave(const std::shared_ptr<ClientSession>& session){
    if(this->room_members.erase(session->session_id) == 0){
        return;
    }
    const std::string notice = "Session " + std::to_string(session->session_id) +
        " left (members=" + std::to_string(this->room_members.size()) + ")";
    this->Broadcast(std::make_shared<const std::vector<char>>(protocol::Make_Notice(notice)));
    this->Logging(LogLevel::Info, "[Game] " + notice);
}

void IocpGameServer::Handle_Packet(const std::shared_ptr<ClientSession>& session, const protocol::Packet& packet){
    // 방에서 이미 나간 세션의 늦게 도착한 패킷은 무시한다.
    if(this->room_members.find(session->session_id) == this->room_members.end()){
        return;
    }
    switch(static_cast<protocol::PacketId>(packet.id)){
        case protocol::PacketId::C_Chat:{
            ++this->chat_count;
            const std::string_view text(packet.body.data(), packet.body.size());
            this->Logging(LogLevel::Debug,
                "[Game] chat from session=" + std::to_string(session->session_id) + ": " + std::string(text));
            this->Broadcast(std::make_shared<const std::vector<char>>(protocol::Make_Chat(session->session_id, text)));
            break;
        }
        default:
            this->Logging(LogLevel::Error,
                "[Game] unknown packet id=" + std::to_string(packet.id) +
                ", closing session=" + std::to_string(session->session_id));
            this->Close_Session(session);
            break;
    }
}

void IocpGameServer::Log_Status(){
    std::size_t active_sessions = 0;
    {
        std::lock_guard<std::mutex> lock(this->session_mutex);
        active_sessions = this->sessions.size();
    }
    std::size_t pending_io = 0;
    {
        std::lock_guard<std::mutex> lock(this->context_mutex);
        pending_io = this->pending_contexts.size();
    }
    this->Logging(LogLevel::Info,
        "[Status] sessions=" + std::to_string(active_sessions) +
        ", pending_io=" + std::to_string(pending_io) +
        ", job_queue=" + std::to_string(this->game_jobs.Size()) +
        ", job_queue_peak=" + std::to_string(this->game_jobs.Peak_Size()) +
        ", jobs_pushed=" + std::to_string(this->game_jobs.Pushed_Count()) +
        ", jobs_processed=" + std::to_string(this->processed_job_count.load()) +
        ", jobs_rejected=" + std::to_string(this->rejected_job_count.load()) +
        ", recv_paused_now=" + std::to_string(this->paused_count.load()) +
        ", recv_pauses=" + std::to_string(this->recv_pause_count.load()) +
        ", wsasend_posts=" + std::to_string(this->send_post_count.load()) +
        ", chats=" + std::to_string(this->chat_count.load()) +
        ", log_dropped=" + std::to_string(this->logger.Dropped_Count()));
}

void IocpGameServer::Clean_Up(){
    {
        std::lock_guard<std::mutex> lock(this->server_mutex);
        ServerState expected = ServerState::Stopping;
        if(!this->server_state.compare_exchange_strong(expected, ServerState::Cleaning)){
            return;
        }
    }
    this->Logging(LogLevel::Info, "[IocpGameServer] cleanup started");

    // 1) game logic 스레드를 먼저 멈춘다. 살아 있으면 정리 도중에도 송신 요청을 계속 만든다.
    //    큐를 닫으면 completion worker의 새 job은 거부되고, logic 스레드는 남은 job을 버리고 끝난다.
    this->game_jobs.Close();
    if(this->game_thread.joinable()){
        this->game_thread.join();
    }

    // 2) 이후는 v6와 같다. client socket을 닫아 pending Recv/Send를 취소한다.
    //    수신을 멈춘 세션도 sessions에 있으므로 함께 닫히고, 일시정지 목록의 참조는 여기서 놓는다.
    std::vector<std::shared_ptr<ClientSession>> sessions_to_close;
    {
        std::lock_guard<std::mutex> lock(this->session_mutex);
        sessions_to_close.assign(this->sessions.begin(), this->sessions.end());
    }
    for(const auto& session : sessions_to_close){
        this->Close_Session(session);
    }
    {
        std::lock_guard<std::mutex> lock(this->paused_mutex);
        this->paused_sessions.clear();
        this->paused_count.store(0);
    }

    {
        std::lock_guard<std::mutex> lock(this->context_mutex);
        for(IoContext* context : this->pending_contexts){
            if(context->io_type == IoContext::IoType::Accept && context->accept_socket != INVALID_SOCKET){
                closesocket(context->accept_socket);
                context->accept_socket = INVALID_SOCKET;
            }
        }
    }

    // 3) OS가 돌려준 completion을 worker가 모두 회수할 때까지 기다린다.
    {
        std::unique_lock<std::mutex> lock(this->context_mutex);
        while(!this->pending_contexts.empty()){
            if(this->context_condition.wait_for(lock, kCleanupDiagnosticInterval) == std::cv_status::timeout){
                this->Logging(LogLevel::Error,
                    "[IocpGameServer] still waiting for " + std::to_string(this->pending_contexts.size()) + " pending I/O context(s)");
            }
        }
    }

    this->Logging(LogLevel::Info,
        "[IocpGameServer] pending I/O drained, partial sends=" + std::to_string(this->partial_send_count.load()) +
        ", wsasend posts=" + std::to_string(this->send_post_count.load()) +
        ", recv pauses=" + std::to_string(this->recv_pause_count.load()) +
        ", jobs processed=" + std::to_string(this->processed_job_count.load()) +
        ", jobs discarded=" + std::to_string(this->discarded_job_count.load()) +
        ", jobs rejected=" + std::to_string(this->rejected_job_count.load()) +
        ", chats=" + std::to_string(this->chat_count.load()));

    // 4) 실제 I/O completion을 모두 처리한 뒤에만 null completion으로 worker를 깨운다.
    if(this->iocp_handle != nullptr){
        for(std::size_t i=0; i<this->completion_workers.size(); ++i){
            if(!PostQueuedCompletionStatus(this->iocp_handle, 0, 0, nullptr)){
                this->Logging(LogLevel::Error, "[IocpGameServer] worker wake failed: " + std::to_string(GetLastError()));
            }
        }
    }
    for(std::thread& worker : this->completion_workers){
        if(worker.joinable()){
            worker.join();
        }
    }
    this->completion_workers.clear();

    if(this->iocp_handle != nullptr){
        CloseHandle(this->iocp_handle);
        this->iocp_handle = nullptr;
    }
    if(this->listen_socket != INVALID_SOCKET){
        closesocket(this->listen_socket);
        this->listen_socket = INVALID_SOCKET;
    }
    if(this->winsock_init_state){
        if(WSACleanup() == 0){
            this->winsock_init_state = false;
        }else{
            this->Logging(LogLevel::Error, "[IocpGameServer] WSACleanup failed: " + std::to_string(WSAGetLastError()));
        }
    }
    this->server_state.store(ServerState::Stopped);
    this->Logging(LogLevel::Info, "[IocpGameServer] server stopped");
}

void IocpGameServer::Server_Run(){
    this->Logging(LogLevel::Info, "[IocpGameServer] server starting, port=" + std::to_string(this->server_port) +
        ", job_high_watermark=" + std::to_string(this->job_high_watermark) +
        ", job_low_watermark=" + std::to_string(this->job_low_watermark));
    {
        std::lock_guard<std::mutex> lock(this->server_mutex);
        ServerState expected = ServerState::Creating;
        if(!this->server_state.compare_exchange_strong(expected, ServerState::Running)){
            return;
        }
    }

    if(!this->Init_Winsock() || !this->Create_Listen_Socket() ||
        !this->Get_AcceptEx() || !this->Create_Completion_Port()){
        this->Proceed_Stop();
        return;
    }

    // logic 스레드를 accept 전에 띄워 첫 Enter job부터 바로 처리한다.
    this->game_thread = std::thread(&IocpGameServer::Game_Logic_Loop, this);
    this->Start_Completion_Workers();

    unsigned int posted_accepts = 0;
    for(unsigned int i=0; i<kAcceptDepth; ++i){
        if(this->Post_Accept()){
            ++posted_accepts;
        }
    }
    if(posted_accepts == 0){
        this->Proceed_Stop();
        return;
    }
    this->Logging(LogLevel::Info, "[IocpGameServer] AcceptEx initial posts=" + std::to_string(posted_accepts));

    // Server_Run 스레드는 Stop 신호를 기다리면서 주기적으로 큐 길이와 처리량을 출력하는 monitor 역할을 한다.
    {
        std::unique_lock<std::mutex> lock(this->server_mutex);
        while(!this->run_cv.wait_for(lock, kStatusInterval, [this]{
            return this->server_state.load() != ServerState::Running;
        })){
            lock.unlock();
            this->Log_Status();
            lock.lock();
        }
    }
    this->Log_Status();
    this->Proceed_Stop();
}

void IocpGameServer::Stop(){
    // Stop은 상태 전환과 알림만 담당하고 리소스 정리는 Clean_Up에게 맡긴다.
    {
        std::lock_guard<std::mutex> lock(this->server_mutex);
        ServerState expected = ServerState::Running;
        if(!this->server_state.compare_exchange_strong(expected, ServerState::Stopping)){
            expected = ServerState::Creating;
            this->server_state.compare_exchange_strong(expected, ServerState::Stopped);
            return;
        }
    }
    this->Logging(LogLevel::Info, "[IocpGameServer] stop requested");
    this->run_cv.notify_all();
}

void IocpGameServer::Proceed_Stop(){
    this->Stop();
    this->Clean_Up();
}
