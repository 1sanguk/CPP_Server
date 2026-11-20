#include <iocp_world_server.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include <tick_scheduler.h>

constexpr int kBacklog = 512;
constexpr unsigned int kAcceptDepth = 64;
constexpr auto kCleanupDiagnosticInterval = std::chrono::seconds(5);
constexpr DWORD kAcceptAddressLength = sizeof(sockaddr_in) + 16;
// 수신 버퍼는 최대 패킷 크기와 같게 둔다. 작은 이동·채팅 패킷 위주라 한 번에 64 KiB를 받을 일이 드물다.
constexpr std::size_t kRecvBufferSize = protocol::kMaxPacketSize;
// WSASend 한 번에 묶는 송신 버퍼 수와 바이트의 상한
constexpr std::size_t kMaxSendBuffersPerPost = 64;
constexpr std::size_t kMaxSendBytesPerPost = 256 * 1024;
// completion worker가 틱 루프보다 빠르게 job을 쌓을 때의 최종 상한. 평소에는 아래 수위로
// Recv를 멈춰 이 상한에 닿지 않게 한다.
constexpr std::size_t kGameJobCapacity = 65536;
constexpr std::size_t kDefaultJobHighWatermark = 8192;
constexpr std::size_t kDefaultJobLowWatermark = 1024;
// 응답을 읽지 않는 클라이언트 때문에 송신 버퍼가 무한히 쌓이지 않게 하는 세션별 바이트 상한.
constexpr std::size_t kMaxSendQueueBytes = 4 * 1024 * 1024;
constexpr int kDefaultTickMs = 50;
// 틱 루프가 worker에게 WSASend 게시 묶음을 넘기는 완료 통지의 key. 실제 I/O completion은 key 0을 쓴다.
constexpr ULONG_PTR kSendBatchKey = 1;
// 세션이 적을 때 묶음을 너무 잘게 나누지 않기 위한 묶음당 최소 세션 수
constexpr std::size_t kMinSendPostsPerBatch = 32;
// 틱 단계별 시간 이름. [Status]의 phase_avg_us에 이 순서로 찍는다.
constexpr const char* kTickPhaseNames[] = {"input", "step", "collect", "encode", "send"};

namespace{
std::uint64_t Elapsed_Us(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to){
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
}
}           // 20Hz
constexpr int kDefaultStatusIntervalMs = 10000;
// 세션 하나가 한 틱에 보낼 수 있는 채팅 수(20Hz면 초당 80개)
constexpr int kDefaultChatsPerTick = 4;

ClientSession::ClientSession(SOCKET socket_value, std::uint32_t id)
    : client_socket(socket_value), session_id(id),
      recv_context(std::make_unique<IoContext>(IoContext::IoType::Recv, kRecvBufferSize)),
      send_context(std::make_unique<IoContext>(IoContext::IoType::Send, 0)){}

IocpWorldServer::IocpWorldServer(int port)
    : server_port(port), game_jobs(kGameJobCapacity),
      tick_period(static_cast<long long>(Read_Number_From_Env("V8_TICK_MS", kDefaultTickMs, 1, 1000))),
      status_interval(static_cast<long long>(
          Read_Number_From_Env("V8_STATUS_INTERVAL_MS", kDefaultStatusIntervalMs, 100, 3600000))),
      world(Read_World_Config_From_Env()),
      chats_per_tick(static_cast<std::uint32_t>(Read_Number_From_Env("V8_CHATS_PER_TICK", kDefaultChatsPerTick, 1, 1000000))),
      job_high_watermark(static_cast<std::size_t>(Read_Number_From_Env("V8_JOB_HIGH_WATERMARK",
          static_cast<double>(kDefaultJobHighWatermark), 2, static_cast<double>(kGameJobCapacity - 1)))),
      job_low_watermark((std::min)(static_cast<std::size_t>(Read_Number_From_Env("V8_JOB_LOW_WATERMARK",
          static_cast<double>(kDefaultJobLowWatermark), 1, static_cast<double>(kGameJobCapacity - 1))),
          job_high_watermark / 2)),
      logger(std::cout){
    this->logger.Start();
}

IocpWorldServer::~IocpWorldServer(){
    this->Proceed_Stop();
    // 종료 과정의 마지막 로그까지 출력한 뒤 writer 스레드를 정리한다.
    this->logger.Stop();
}

IocpWorldServer::LogLevel IocpWorldServer::Read_Log_Level_From_Env(){
    char buffer[16]{};
    DWORD length = GetEnvironmentVariableA("V8_LOG_LEVEL", buffer, sizeof(buffer));
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

double IocpWorldServer::Read_Number_From_Env(const char* name, double default_value, double min_value, double max_value){
    char buffer[32]{};
    DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if(length == 0 || length >= sizeof(buffer)){
        return default_value;
    }
    char* end = nullptr;
    const double value = std::strtod(buffer, &end);
    if(end == buffer || !(value >= min_value && value <= max_value)){
        return default_value;
    }
    return value;
}

World::Config IocpWorldServer::Read_World_Config_From_Env(){
    World::Config config;
    config.world_size = static_cast<float>(Read_Number_From_Env("V8_WORLD_SIZE", config.world_size, 10, 100000));
    // V8_CELL_SIZE를 월드 크기 이상으로 주면 셀이 하나가 되어 AOI를 끈 것(전체 방송)과 같다. 비교 측정에 쓴다.
    config.cell_size = static_cast<float>(Read_Number_From_Env("V8_CELL_SIZE", config.cell_size, 1, 100000));
    config.max_speed = static_cast<float>(Read_Number_From_Env("V8_MAX_SPEED", config.max_speed, 0, 100000));
    config.seed = static_cast<std::uint32_t>(Read_Number_From_Env("V8_SEED", config.seed, 0, 4294967295.0));
    return config;
}

void IocpWorldServer::Logging(LogLevel level, const std::string& message){
    if(level < this->minimum_log_level){
        return;
    }
    // 출력은 로거 스레드가 한다. stdout이 막혀도 호출한 스레드는 기다리지 않는다.
    this->logger.Write(message);
}

bool IocpWorldServer::Init_Winsock(){
    if(this->winsock_init_state){
        return true;
    }
    int startup_result = WSAStartup(MAKEWORD(2,2), &this->wsaData);
    if(startup_result != 0){
        this->Logging(LogLevel::Error, "[IocpWorldServer] WSAStartup failed: " + std::to_string(startup_result));
        return false;
    }
    this->winsock_init_state = true;
    this->Logging(LogLevel::Info, "[IocpWorldServer] Winsock initialized");
    return true;
}

bool IocpWorldServer::Create_Listen_Socket(){
    if(this->server_port < 0 || this->server_port > 65535){
        this->Logging(LogLevel::Error, "[IocpWorldServer] invalid port: " + std::to_string(this->server_port));
        return false;
    }

    this->listen_socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if(this->listen_socket == INVALID_SOCKET){
        this->Logging(LogLevel::Error, "[IocpWorldServer] listen socket creation failed: " + std::to_string(WSAGetLastError()));
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<u_short>(this->server_port));

    if(bind(this->listen_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR){
        this->Logging(LogLevel::Error, "[IocpWorldServer] bind failed: " + std::to_string(WSAGetLastError()));
        closesocket(this->listen_socket);
        this->listen_socket = INVALID_SOCKET;
        return false;
    }
    if(listen(this->listen_socket, kBacklog) == SOCKET_ERROR){
        this->Logging(LogLevel::Error, "[IocpWorldServer] listen failed: " + std::to_string(WSAGetLastError()));
        closesocket(this->listen_socket);
        this->listen_socket = INVALID_SOCKET;
        return false;
    }

    this->Logging(LogLevel::Info, "[IocpWorldServer] listening on port " + std::to_string(this->server_port));
    return true;
}

bool IocpWorldServer::Get_AcceptEx(){
    GUID accept_guid = WSAID_ACCEPTEX;
    DWORD bytes_returned{};
    int result = WSAIoctl(this->listen_socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &accept_guid, sizeof(accept_guid), &this->lpfn_acceptex, sizeof(this->lpfn_acceptex),
        &bytes_returned, nullptr, nullptr);
    if(result == SOCKET_ERROR){
        this->Logging(LogLevel::Error, "[IocpWorldServer] WSAIoctl(AcceptEx) failed: " + std::to_string(WSAGetLastError()));
        return false;
    }

    GUID sockaddrs_guid = WSAID_GETACCEPTEXSOCKADDRS;
    result = WSAIoctl(this->listen_socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &sockaddrs_guid, sizeof(sockaddrs_guid), &this->lpfn_get_accept_ex_sockaddrs, sizeof(this->lpfn_get_accept_ex_sockaddrs),
        &bytes_returned, nullptr, nullptr);
    if(result == SOCKET_ERROR){
        this->Logging(LogLevel::Error, "[IocpWorldServer] WSAIoctl(GetAcceptExSockaddrs) failed: " + std::to_string(WSAGetLastError()));
        return false;
    }

    this->Logging(LogLevel::Info, "[IocpWorldServer] AcceptEx extension functions loaded");
    return true;
}

bool IocpWorldServer::Create_Completion_Port(){
    this->iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if(this->iocp_handle == nullptr){
        this->Logging(LogLevel::Error, "[IocpWorldServer] IOCP creation failed: " + std::to_string(GetLastError()));
        return false;
    }
    if(CreateIoCompletionPort(reinterpret_cast<HANDLE>(this->listen_socket), this->iocp_handle, 0, 0) == nullptr){
        this->Logging(LogLevel::Error, "[IocpWorldServer] listen IOCP registration failed: " + std::to_string(GetLastError()));
        return false;
    }
    this->Logging(LogLevel::Info, "[IocpWorldServer] completion port created");
    return true;
}

void IocpWorldServer::Start_Completion_Workers(){
    unsigned int detected = std::thread::hardware_concurrency();
    unsigned int count = detected == 0 ? 1 : detected;
    this->completion_workers.reserve(count);
    for(unsigned int i=0; i<count; ++i){
        this->completion_workers.emplace_back(&IocpWorldServer::Completion_worker_Loop, this, i);
    }
    this->Logging(LogLevel::Info,
        "[IocpWorldServer] completion workers started, count=" + std::to_string(count));
}

bool IocpWorldServer::Post_Accept(){
    // Accept는 세션이 생기기 전 작업이라 context를 매번 만든다. 접속 빈도는 송수신보다 훨씬 낮다.
    auto* context = new IoContext(IoContext::IoType::Accept, 2 * kAcceptAddressLength);
    context->accept_socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if(context->accept_socket == INVALID_SOCKET){
        this->Logging(LogLevel::Error, "[IocpWorldServer] accept socket creation failed: " + std::to_string(WSAGetLastError()));
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
        ++this->pending_io_count;
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
        this->Logging(LogLevel::Error, "[IocpWorldServer] AcceptEx failed: " + std::to_string(error));
        closesocket(context->accept_socket);
        delete context;
        this->Release_Io();
        return false;
    }
    this->Logging(LogLevel::Debug, "[IocpWorldServer] AcceptEx posted");
    return true;
}

bool IocpWorldServer::Post_Recv(const std::shared_ptr<ClientSession>& session){
    // 세션의 Recv context는 이전 Recv completion이 끝난 뒤에만 다시 게시되므로 재사용해도 안전하다.
    IoContext* context = session->recv_context.get();

    int error = 0;
    {
        // socket_mutex만 시스템 호출 동안 잡는다. Close_Session도 socket_mutex를 거쳐 socket을 닫으므로,
        // 아래에서 closing을 확인한 뒤에는 이 WSARecv가 끝날 때까지 socket이 닫히지 않는다.
        std::lock_guard<std::mutex> socket_lock(session->socket_mutex);
        if(this->server_state.load() != ServerState::Running || session->closing.load()){
            return false;
        }
        context->session = session;
        context->overlapped = OVERLAPPED{};
        context->flags = 0;
        context->wsa_buffer.buf = context->buffer.data();
        context->wsa_buffer.len = static_cast<ULONG>(context->buffer.size());
        this->Track_Context(context);
        int result = WSARecv(session->client_socket, &context->wsa_buffer, 1, nullptr,
            &context->flags, &context->overlapped, nullptr);
        if(result == SOCKET_ERROR){
            error = WSAGetLastError();
            if(error != WSA_IO_PENDING){
                context->session.reset();
                this->Untrack_Context(context);
            }
        }
    }

    if(error != 0 && error != WSA_IO_PENDING){
        this->Logging(LogLevel::Error, "[IocpWorldServer] WSARecv failed: " + std::to_string(error));
        return false;
    }
    this->Logging(LogLevel::Debug,
        "[IocpWorldServer] WSARecv posted, session=" + std::to_string(session->session_id));
    return true;
}

bool IocpWorldServer::Post_Next_Send_Locked(const std::shared_ptr<ClientSession>& session){
    // 호출자는 send_mutex를 보유한다. 송신이 멈춰 있던 세션은 틱 루프(Enqueue_Send)가,
    // 이어지는 송신은 Send completion을 받은 IOCP worker가 게시한다.
    if(session->send_queue.empty() || session->closing.load()){
        session->send_pending = false;
        return !session->closing.load();
    }

    // v7은 큐의 패킷들을 세션마다 고정한 64 KiB 버퍼에 복사해 보냈다. v8은 복사하지 않고 WSABUF 배열이
    // 큐의 버퍼를 직접 가리키게 한다(scatter-gather). 송신 버퍼를 세션마다 둘 필요가 없어 연결당 메모리가 줄고,
    // 복사 비용도 없어진다. 대신 커널이 읽는 동안 버퍼가 살아 있도록 in_flight에 shared_ptr를 붙잡아 둔다.
    IoContext* context = session->send_context.get();
    context->send_buffers.clear();
    context->in_flight.clear();
    std::size_t total = 0;
    std::size_t offset = session->send_offset;
    for(const SendBuffer& data : session->send_queue){
        if(context->send_buffers.size() == kMaxSendBuffersPerPost || total >= kMaxSendBytesPerPost){
            break;
        }
        WSABUF wsa_buffer{};
        wsa_buffer.buf = const_cast<char*>(data->data() + offset);
        wsa_buffer.len = static_cast<ULONG>(data->size() - offset);
        context->send_buffers.push_back(wsa_buffer);
        context->in_flight.push_back(data);
        total += data->size() - offset;
        offset = 0;
    }
    context->logical_send_length = total;

    int error = 0;
    {
        // v7은 WSASend를 부르는 동안 서버 전체가 함께 쓰는 context_mutex까지 잡았다. 그러면 틱 루프의 송신과
        // 모든 worker의 Recv 게시가 한 줄로 서게 된다. v8은 Recv/Send의 pending 수를 원자 카운터로만 센다.
        std::lock_guard<std::mutex> socket_lock(session->socket_mutex);
        if(this->server_state.load() != ServerState::Running || session->closing.load()){
            session->send_pending = false;
            return false;
        }
        context->session = session;
        context->overlapped = OVERLAPPED{};
        this->Track_Context(context);
        int result = WSASend(session->client_socket, context->send_buffers.data(),
            static_cast<DWORD>(context->send_buffers.size()), nullptr, 0, &context->overlapped, nullptr);
        if(result == SOCKET_ERROR){
            error = WSAGetLastError();
            if(error != WSA_IO_PENDING){
                context->session.reset();
                context->in_flight.clear();
                this->Untrack_Context(context);
            }
        }
    }

    if(error != 0 && error != WSA_IO_PENDING){
        this->Logging(LogLevel::Error, "[IocpWorldServer] WSASend failed: " + std::to_string(error));
        session->send_pending = false;
        return false;
    }
    ++this->send_post_count;
    return true;
}

void IocpWorldServer::Dispatch_Send_Posts(std::vector<std::shared_ptr<ClientSession>>& pending_posts){
    // 틱 루프가 이번 틱에 WSASend 게시가 필요해진 세션들을 worker 수만큼 묶어 IOCP에 넘긴다.
    // 로컬 연결의 WSASend는 호출 안에서 상대 버퍼까지 복사해 세션당 수십 µs가 걸린다. 틱 스레드 혼자 수천 번
    // 부르면 틱이 주기를 넘으므로 worker 여럿이 나눠 부르게 한다. 세션마다 완료 통지를 넣으면 그 호출 자체가
    // 비용이 되므로(v7에서 확인), 묶음 하나에 완료 통지 하나만 쓴다.
    if(pending_posts.empty()){
        return;
    }
    const std::size_t worker_count = (std::max)(std::size_t{1}, this->completion_workers.size());
    const std::size_t batch_count = (std::min)(worker_count, (pending_posts.size() + kMinSendPostsPerBatch - 1) / kMinSendPostsPerBatch);
    const std::size_t per_batch = (pending_posts.size() + batch_count - 1) / batch_count;
    for(std::size_t begin = 0; begin < pending_posts.size(); begin += per_batch){
        const std::size_t end = (std::min)(pending_posts.size(), begin + per_batch);
        auto* batch = new SendPostBatch();
        batch->sessions.assign(std::make_move_iterator(pending_posts.begin() + static_cast<std::ptrdiff_t>(begin)),
            std::make_move_iterator(pending_posts.begin() + static_cast<std::ptrdiff_t>(end)));
        ++this->pending_io_count;
        ++this->send_batch_count;
        if(!PostQueuedCompletionStatus(this->iocp_handle, 0, kSendBatchKey, &batch->overlapped)){
            this->Logging(LogLevel::Error, "[IocpWorldServer] send batch post failed: " + std::to_string(GetLastError()));
            // 넘기지 못한 묶음은 틱 스레드가 직접 처리한다.
            this->Handle_Send_Post_Batch(batch);
            this->Release_Io();
        }
    }
    pending_posts.clear();
}

void IocpWorldServer::Handle_Send_Post_Batch(SendPostBatch* batch){
    for(const auto& session : batch->sessions){
        bool result = true;
        {
            std::lock_guard<std::mutex> lock(session->send_mutex);
            // 그사이 세션이 닫혔다면 큐가 비어 있고, Post_Next_Send_Locked가 send_pending을 내린 뒤 false를 반환한다.
            result = this->Post_Next_Send_Locked(session);
        }
        if(!result){
            this->Close_Session(session);
        }
    }
    delete batch;
}

bool IocpWorldServer::Enqueue_Send(const std::shared_ptr<ClientSession>& session, const SendBuffer& data,
    std::vector<std::shared_ptr<ClientSession>>& posts_to_dispatch){
    // 틱 루프에서 호출된다. 큐 적재는 여기서 해 세션별 송신 순서를 지킨다. 이미 WSASend가 진행 중이면
    // 큐에만 넣고(Send completion이 이어서 보낸다), 아니면 게시가 필요한 세션 목록에 넣어 틱 끝에 worker에게 넘긴다.
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
                posts_to_dispatch.push_back(session);
            }
        }
    }
    if(overflow){
        this->Logging(LogLevel::Error,
            "[IocpWorldServer] send queue overflow, closing slow client session=" + std::to_string(session->session_id) +
            ", queued_bytes_limit=" + std::to_string(kMaxSendQueueBytes));
    }
    if(!result){
        this->Close_Session(session);
    }
    return result;
}

void IocpWorldServer::Track_Context(IoContext*){
    // Recv/Send context는 세션이 소유하므로 종료 시 하나씩 찾아 닫을 필요가 없다. 개수만 센다.
    ++this->pending_io_count;
}

void IocpWorldServer::Untrack_Context(IoContext*){
    this->Release_Io();
}

void IocpWorldServer::Release_Io(){
    // 마지막 pending I/O가 끝났을 때만 잠그고 깨운다. 잠근 뒤 알리므로 Clean_Up이 조건을 확인하고
    // 잠들기 직전 사이에 알림이 지나가 버리는(lost wakeup) 일이 없다.
    if(this->pending_io_count.fetch_sub(1) == 1){
        std::lock_guard<std::mutex> lock(this->context_mutex);
        this->context_condition.notify_all();
    }
}

void IocpWorldServer::Finish_Context(IoContext* context){
    if(context->io_type == IoContext::IoType::Accept){
        std::lock_guard<std::mutex> lock(this->context_mutex);
        this->pending_contexts.erase(context);
    }
    this->Release_Io();
}

bool IocpWorldServer::Submit_Job(GameJob job){
    if(this->game_jobs.Push(std::move(job))){
        return true;
    }
    if(this->server_state.load() == ServerState::Running){
        ++this->rejected_job_count;
    }
    return false;
}

void IocpWorldServer::Pause_Recv(const std::shared_ptr<ClientSession>& session){
    // 다음 WSARecv를 게시하지 않으면 서버 쪽 TCP 수신 버퍼가 차고, 클라이언트의 송신이 흐름 제어로 느려진다.
    {
        std::lock_guard<std::mutex> lock(this->paused_mutex);
        this->paused_sessions.push_back(session);
        this->paused_count.store(this->paused_sessions.size());
    }
    ++this->recv_pause_count;
    this->Logging(LogLevel::Debug,
        "[IocpWorldServer] job queue above high watermark, recv paused for session=" + std::to_string(session->session_id));
    // 틱 루프가 이미 큐를 비우고 다음 틱까지 잠들었을 수 있으므로, 넣은 쪽에서도 한 번 재개 조건을 확인한다.
    this->Resume_Paused_Sessions();
}

void IocpWorldServer::Resume_Paused_Sessions(){
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

void IocpWorldServer::Close_Session(const std::shared_ptr<ClientSession>& session){
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
        "[IocpWorldServer] client disconnected, session=" + std::to_string(session->session_id) +
        ", remote=" + session->remote_endpoint +
        ", active_sessions=" + std::to_string(remaining_sessions));
}

void IocpWorldServer::Handle_Accept_Completion(IoContext* context, bool succeeded, unsigned int worker_index){
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
        this->Logging(LogLevel::Error, "[IocpWorldServer] SO_UPDATE_ACCEPT_CONTEXT failed: " + std::to_string(WSAGetLastError()));
        closesocket(accepted_socket);
        delete context;
        this->Post_Accept();
        return;
    }

    if(CreateIoCompletionPort(reinterpret_cast<HANDLE>(accepted_socket), this->iocp_handle, 0, 0) == nullptr){
        this->Logging(LogLevel::Error, "[IocpWorldServer] client IOCP registration failed: " + std::to_string(GetLastError()));
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
        "[IocpWorldServer] client connected, worker=" + std::to_string(worker_index) +
        ", session=" + std::to_string(session->session_id) +
        ", remote=" + remote_endpoint +
        ", active_sessions=" + std::to_string(active_session_count));

    this->Post_Accept();

    // Enter는 반드시 첫 Recv 게시 전에 넣는다. 그래야 이 세션의 Packet job이 Enter보다
    // 앞서 틱 루프에 도착하지 않는다.
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

void IocpWorldServer::Handle_Recv_Completion(IoContext* context, bool succeeded, DWORD byte_transferred, unsigned int worker_index){
    // context가 붙잡고 있던 세션 참조를 넘겨받아 세션 ↔ context 순환 참조를 끊는다.
    std::shared_ptr<ClientSession> session = std::move(context->session);
    if(!succeeded || byte_transferred == 0 || this->server_state.load() != ServerState::Running){
        this->Close_Session(session);
        return;
    }

    this->Logging(LogLevel::Debug,
        "[IocpWorldServer] recv completed, worker=" + std::to_string(worker_index) +
        ", session=" + std::to_string(session->session_id) +
        ", bytes=" + std::to_string(byte_transferred));

    // 받은 바이트를 조립기에 누적하고, 완성된 패킷만 game job으로 넘긴다.
    // 이 세션의 다음 Recv는 아직 게시 전이므로 조립기와 Recv 버퍼를 쓰는 스레드는 지금 하나뿐이다.
    session->assembler.Append(context->buffer.data(), byte_transferred);

    // v8: 이번 Recv에서 완성된 패킷을 모두 모은 뒤 큐에 한 번의 잠금으로 넣는다.
    // v7은 패킷마다 Push해 잠금을 반복했다. 이 벡터는 worker 스레드마다 하나씩 두고 재사용한다.
    thread_local std::vector<GameJob> batch;
    batch.clear();
    while(auto packet = session->assembler.Next()){
        GameJob job;
        job.type = GameJob::JobType::Packet;
        job.session = session;
        job.packet = std::move(*packet);
        batch.push_back(std::move(job));
    }

    bool keep_session = true;
    if(!batch.empty()){
        const std::size_t batch_size = batch.size();
        if(!this->game_jobs.Push_Batch(batch)){
            if(this->server_state.load() == ServerState::Running){
                this->rejected_job_count += batch_size;
            }
            // 수위 기반 일시정지로 보통은 여기까지 오지 않는다. 최종 상한에 닿았을 때만 연결을 끊는다.
            this->Logging(LogLevel::Error,
                "[IocpWorldServer] game job queue hard limit reached, closing session=" + std::to_string(session->session_id));
            batch.clear();
            keep_session = false;
        }
    }
    if(keep_session && session->assembler.Has_Error()){
        this->Logging(LogLevel::Error,
            "[IocpWorldServer] malformed packet header, closing session=" + std::to_string(session->session_id) +
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

void IocpWorldServer::Handle_Send_Completion(IoContext* context, bool succeeded, DWORD byte_transferred, unsigned int worker_index){
    std::shared_ptr<ClientSession> session = std::move(context->session);
    bool post_result = true;
    {
        std::lock_guard<std::mutex> lock(session->send_mutex);
        // 커널이 다 읽었으므로 붙잡아 두었던 버퍼를 놓는다. 큐에서 아직 안 빠진 버퍼는 큐가 계속 소유한다.
        context->in_flight.clear();
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
        "[IocpWorldServer] send completed, worker=" + std::to_string(worker_index) +
        ", session=" + std::to_string(session->session_id) +
        ", bytes=" + std::to_string(byte_transferred));
    if(!post_result){
        this->Close_Session(session);
    }
}

void IocpWorldServer::Completion_worker_Loop(unsigned int index){
    this->Logging(LogLevel::Debug, "[IocpWorldServer] worker " + std::to_string(index) +
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
                    "[IocpWorldServer] GetQueuedCompletionStatus failed without context: " +
                    std::to_string(completion_error));
            }
            break;
        }

        if(completion_key == kSendBatchKey){
            // 틱 루프가 넘긴 WSASend 게시 묶음. I/O completion이 아니므로 IoContext가 아니다.
            this->Handle_Send_Post_Batch(CONTAINING_RECORD(overlapped_ptr, SendPostBatch, overlapped));
            this->Release_Io();
            continue;
        }

        IoContext* context = CONTAINING_RECORD(overlapped_ptr, IoContext, overlapped);
        if(!succeeded){
            // 클라이언트가 RST로 끊는 경우(64, 10054)나 종료 중 취소(995)는 정상적인 흐름이다.
            const bool expected_failure = this->server_state.load() != ServerState::Running ||
                completion_error == ERROR_NETNAME_DELETED || completion_error == WSAECONNRESET ||
                completion_error == ERROR_OPERATION_ABORTED;
            this->Logging(expected_failure ? LogLevel::Debug : LogLevel::Error,
                "[IocpWorldServer] overlapped I/O failed, type=" +
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
    this->Logging(LogLevel::Debug, "[IocpWorldServer] worker " + std::to_string(index) + " stopped");
}

void IocpWorldServer::Game_Tick_Loop(){
    this->Logging(LogLevel::Info, "[IocpWorldServer] tick loop started, thread_id=" +
        std::to_string(GetCurrentThreadId()) + ", period_ms=" + std::to_string(this->tick_period.count()));
    TickScheduler scheduler(this->tick_period, std::chrono::steady_clock::now());
    const float period_seconds = std::chrono::duration<float>(this->tick_period).count();
    std::deque<GameJob> jobs;

    // 다음 틱 시작 시각까지 자다가, 그동안 쌓인 job을 한꺼번에 가져온다. 큐가 닫히면 끝난다.
    while(this->game_jobs.Drain_Until(jobs, scheduler.Next_Deadline())){
        const auto tick_start = std::chrono::steady_clock::now();
        // 처리 시간이 주기를 넘어 시작 시각을 놓쳤다면 놓친 틱은 건너뛰고, 그만큼 dt를 늘린다.
        const std::uint64_t periods = scheduler.Begin_Tick(tick_start);
        this->current_tick = scheduler.Tick_Count();

        // 종료 중에는 남은 job을 처리하지 않고 버린다. 응답을 보낼 socket이 곧 모두 닫히기 때문이다.
        if(this->server_state.load() != ServerState::Running){
            this->discarded_job_count += jobs.size();
            jobs.clear();
            continue;
        }

        // 1) 입력 처리: 이번 틱 동안 도착한 입장·퇴장·패킷을 도착 순서대로 적용한다.
        //    같은 플레이어의 이동 입력이 여러 번 왔다면 속도 상태를 덮어쓰므로 마지막 입력만 남는다.
        for(GameJob& job : jobs){
            this->Process_Job(job);
        }
        this->processed_job_count += jobs.size();
        // 다음 틱까지 세션 참조를 들고 있지 않도록 비운다.
        jobs.clear();
        // 큐를 비웠으므로 수신을 멈춘 세션들의 Recv를 다시 게시한다.
        this->Resume_Paused_Sessions();
        const auto input_end = std::chrono::steady_clock::now();

        // 2) 월드 갱신: 모든 플레이어를 dt만큼 움직이고 셀이 바뀐 플레이어의 시야 진입/이탈을 기록한다.
        this->world.Step(period_seconds * static_cast<float>(periods));
        const auto step_end = std::chrono::steady_clock::now();
        this->window_phase_us[0] += Elapsed_Us(tick_start, input_end);
        this->window_phase_us[1] += Elapsed_Us(input_end, step_end);

        // 3) 상태 전송: 세션마다 이번 틱의 변경을 버퍼 하나로 묶어 보낸다.
        this->Flush_Tick_Output();

        const auto tick_end = std::chrono::steady_clock::now();
        scheduler.End_Tick(tick_end);
        this->tick_count.store(scheduler.Tick_Count());
        this->late_tick_count.store(scheduler.Late_Count());
        this->skipped_tick_count.store(scheduler.Skipped_Count());
        this->Record_Tick_Time(Elapsed_Us(tick_start, tick_end));
    }
    this->players.clear();
    this->outbox.clear();
    this->Logging(LogLevel::Info, "[IocpWorldServer] tick loop stopped");
}

void IocpWorldServer::Process_Job(GameJob& job){
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
}

void IocpWorldServer::Record_Tick_Time(std::uint64_t elapsed_us){
    ++this->window_tick_count;
    this->window_tick_us_sum += elapsed_us;
    const auto update_max = [elapsed_us](std::atomic<std::uint64_t>& target){
        std::uint64_t current = target.load();
        while(elapsed_us > current && !target.compare_exchange_weak(current, elapsed_us)){}
    };
    update_max(this->window_tick_us_max);
    update_max(this->total_tick_us_max);
}

void IocpWorldServer::Handle_Enter(const std::shared_ptr<ClientSession>& session){
    if(session->closing.load()){
        return;
    }
    const auto state = this->world.Enter(session->session_id);
    if(!state){
        this->Close_Session(session);
        return;
    }
    this->players.emplace(session->session_id, session);

    // 입장 패킷은 이번 틱의 다른 패킷보다 먼저 나가도록 세션 버퍼 맨 앞에 쓴다.
    // 주변 플레이어 목록(S_Spawn)은 틱 끝의 Flush에서 월드가 계산한 시야 변경으로 함께 나간다.
    const World::Config& config = this->world.Get_Config();
    protocol::Append_Enter_World(this->outbox[session->session_id],
        protocol::EnterWorldInfo{session->session_id, state->x, state->y, config.world_size, config.cell_size});
    this->Logging(LogLevel::Debug,
        "[Game] player " + std::to_string(session->session_id) + " entered, remote=" + session->remote_endpoint +
        ", players=" + std::to_string(this->world.Player_Count()));
}

void IocpWorldServer::Handle_Leave(const std::shared_ptr<ClientSession>& session){
    if(this->players.erase(session->session_id) == 0){
        return;
    }
    // 주변 플레이어에게는 이번 틱 끝에 S_Despawn이 나간다.
    this->world.Leave(session->session_id);
    this->outbox.erase(session->session_id);
    this->Logging(LogLevel::Debug,
        "[Game] player " + std::to_string(session->session_id) + " left, players=" +
        std::to_string(this->world.Player_Count()));
}

void IocpWorldServer::Handle_Packet(const std::shared_ptr<ClientSession>& session, const protocol::Packet& packet){
    // 이미 월드에서 나간 세션의 늦게 도착한 패킷은 무시한다.
    if(!this->players.contains(session->session_id)){
        return;
    }
    switch(static_cast<protocol::PacketId>(packet.id)){
        case protocol::PacketId::C_Move:{
            const auto input = protocol::Parse_Move(packet.body);
            if(!input){
                this->Logging(LogLevel::Error,
                    "[Game] malformed C_Move, closing session=" + std::to_string(session->session_id));
                this->Close_Session(session);
                return;
            }
            ++this->move_input_count;
            // 클라이언트는 속도만 요청한다. 실제 위치는 틱마다 서버가 계산한다(서버 권위).
            // 입력 순번은 S_SelfState로 돌려주어 클라이언트가 예측 이동을 서버 결과로 보정할 수 있게 한다.
            this->world.Set_Velocity(session->session_id, input->velocity_x, input->velocity_y, input->input_seq);
            break;
        }
        case protocol::PacketId::C_Chat:{
            // 세션별 틱당 채팅 수 상한. 틱 끝에 한 번에 돌려주는 구조라, 상한이 없으면 한 틱에 몰아 보낸 채팅이
            // 그대로 발신자 자신의 송신 큐로 돌아와 상한(4 MiB)을 넘긴다. 넘친 채팅은 버리고 개수만 센다.
            if(session->chat_tick != this->current_tick){
                session->chat_tick = this->current_tick;
                session->chats_in_tick = 0;
            }
            if(++session->chats_in_tick > this->chats_per_tick){
                ++this->dropped_chat_count;
                break;
            }
            ++this->chat_count;
            const std::string_view text(packet.body.data(), packet.body.size());
            // 시야 안의 플레이어와 자신에게만 보낸다. v7의 방 전체 방송과 달리 받는 사람 수가 주변 인원으로 제한된다.
            const std::vector<char> chat = protocol::Make_Chat(session->session_id, text);
            auto append_to = [this, &chat](std::uint32_t receiver){
                auto& out = this->outbox[receiver];
                out.insert(out.end(), chat.begin(), chat.end());
            };
            append_to(session->session_id);
            for(std::uint32_t receiver : this->world.Visible_Ids(session->session_id)){
                append_to(receiver);
            }
            this->Logging(LogLevel::Debug,
                "[Game] chat from player=" + std::to_string(session->session_id) + ": " + std::string(text));
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

void IocpWorldServer::Flush_Tick_Output(){
    const auto build_start = std::chrono::steady_clock::now();
    const std::span<const ObserverUpdate> updates = this->world.Collect_Updates();
    const auto collect_end = std::chrono::steady_clock::now();
    const float world_size = this->world.Get_Config().world_size;
    for(const ObserverUpdate& update : updates){
        auto& out = this->outbox[update.observer_id];
        // 클라이언트가 그대로 적용하면 되도록 self → despawn → spawn → move 순서로 쓴다.
        if(update.has_self){
            protocol::Append_Self_State(out, update.self);
        }
        protocol::Append_Despawns(out, update.despawns);
        protocol::Append_Entity_States(out, protocol::PacketId::S_Spawn, update.spawns, world_size);
        protocol::Append_Entity_States(out, protocol::PacketId::S_MoveBatch, update.moves, world_size);
    }

    const auto send_start = std::chrono::steady_clock::now();
    std::uint64_t tick_bytes = 0;
    for(auto& [player_id, out] : this->outbox){
        if(out.empty()){
            continue;
        }
        auto found = this->players.find(player_id);
        if(found == this->players.end()){
            continue;
        }
        tick_bytes += out.size();
        // Enqueue_Send가 실패하면 Close_Session은 Leave job만 큐에 넣으므로 순회 중에 players가 바뀌지 않는다.
        this->Enqueue_Send(found->second, std::make_shared<const std::vector<char>>(std::move(out)), this->send_posts);
    }
    this->outbox.clear();
    this->Dispatch_Send_Posts(this->send_posts);
    const auto send_end = std::chrono::steady_clock::now();
    this->window_phase_us[2] += Elapsed_Us(build_start, collect_end);
    this->window_phase_us[3] += Elapsed_Us(collect_end, send_start);
    this->window_phase_us[4] += Elapsed_Us(send_start, send_end);
    this->window_sent_bytes += tick_bytes;
    this->player_count.store(this->world.Player_Count());
    this->max_cell_population.store(this->world.Max_Cell_Population());
}

void IocpWorldServer::Log_Status(){
    std::size_t active_sessions = 0;
    {
        std::lock_guard<std::mutex> lock(this->session_mutex);
        active_sessions = this->sessions.size();
    }
    const std::size_t pending_io = this->pending_io_count.load();
    // 지난 출력 이후 구간의 틱 지표를 꺼내고 0으로 되돌린다.
    const std::uint64_t ticks = this->window_tick_count.exchange(0);
    const std::uint64_t tick_us_sum = this->window_tick_us_sum.exchange(0);
    const std::uint64_t tick_us_max = this->window_tick_us_max.exchange(0);
    const std::uint64_t sent_bytes = this->window_sent_bytes.exchange(0);
    std::string phase_text = "phase_avg_us=";
    for(std::size_t i = 0; i < this->window_phase_us.size(); ++i){
        const std::uint64_t phase_sum = this->window_phase_us[i].exchange(0);
        phase_text += std::string(i == 0 ? "" : "/") + kTickPhaseNames[i] + ":" +
            std::to_string(ticks == 0 ? 0 : phase_sum / ticks);
    }
    char tick_text[160]{};
    std::snprintf(tick_text, sizeof(tick_text), "tick_avg_ms=%.3f, tick_max_ms=%.3f, bytes_per_tick=%llu",
        ticks == 0 ? 0.0 : static_cast<double>(tick_us_sum) / static_cast<double>(ticks) / 1000.0,
        static_cast<double>(tick_us_max) / 1000.0,
        static_cast<unsigned long long>(ticks == 0 ? 0 : sent_bytes / ticks));

    this->Logging(LogLevel::Info,
        "[Status] sessions=" + std::to_string(active_sessions) +
        ", players=" + std::to_string(this->player_count.load()) +
        ", pending_io=" + std::to_string(pending_io) +
        ", ticks=" + std::to_string(this->tick_count.load()) +
        ", " + tick_text +
        ", " + phase_text +
        ", late_ticks=" + std::to_string(this->late_tick_count.load()) +
        ", skipped_ticks=" + std::to_string(this->skipped_tick_count.load()) +
        ", max_cell_population=" + std::to_string(this->max_cell_population.load()) +
        ", job_queue_peak=" + std::to_string(this->game_jobs.Peak_Size()) +
        ", jobs_pushed=" + std::to_string(this->game_jobs.Pushed_Count()) +
        ", job_push_calls=" + std::to_string(this->game_jobs.Push_Call_Count()) +
        ", jobs_processed=" + std::to_string(this->processed_job_count.load()) +
        ", jobs_rejected=" + std::to_string(this->rejected_job_count.load()) +
        ", recv_paused_now=" + std::to_string(this->paused_count.load()) +
        ", recv_pauses=" + std::to_string(this->recv_pause_count.load()) +
        ", wsasend_posts=" + std::to_string(this->send_post_count.load()) +
        ", send_batches=" + std::to_string(this->send_batch_count.load()) +
        ", moves=" + std::to_string(this->move_input_count.load()) +
        ", chats=" + std::to_string(this->chat_count.load()) +
        ", chats_dropped=" + std::to_string(this->dropped_chat_count.load()) +
        ", log_dropped=" + std::to_string(this->logger.Dropped_Count()));
}

void IocpWorldServer::Clean_Up(){
    {
        std::lock_guard<std::mutex> lock(this->server_mutex);
        ServerState expected = ServerState::Stopping;
        if(!this->server_state.compare_exchange_strong(expected, ServerState::Cleaning)){
            return;
        }
    }
    this->Logging(LogLevel::Info, "[IocpWorldServer] cleanup started");

    // 1) 틱 루프를 먼저 멈춘다. 살아 있으면 정리 도중에도 송신 요청을 계속 만든다.
    //    큐를 닫으면 completion worker의 새 job은 거부되고, 틱 루프는 다음 틱을 기다리지 않고 바로 끝난다.
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
        while(this->pending_io_count.load() != 0){
            if(this->context_condition.wait_for(lock, kCleanupDiagnosticInterval) == std::cv_status::timeout){
                this->Logging(LogLevel::Error,
                    "[IocpWorldServer] still waiting for " + std::to_string(this->pending_io_count.load()) + " pending I/O context(s)");
            }
        }
    }

    this->Logging(LogLevel::Info,
        "[IocpWorldServer] pending I/O drained, partial sends=" + std::to_string(this->partial_send_count.load()) +
        ", wsasend posts=" + std::to_string(this->send_post_count.load()) +
        ", recv pauses=" + std::to_string(this->recv_pause_count.load()) +
        ", jobs processed=" + std::to_string(this->processed_job_count.load()) +
        ", jobs discarded=" + std::to_string(this->discarded_job_count.load()) +
        ", jobs rejected=" + std::to_string(this->rejected_job_count.load()) +
        ", ticks=" + std::to_string(this->tick_count.load()) +
        ", late ticks=" + std::to_string(this->late_tick_count.load()) +
        ", skipped ticks=" + std::to_string(this->skipped_tick_count.load()) +
        ", max tick us=" + std::to_string(this->total_tick_us_max.load()) +
        ", moves=" + std::to_string(this->move_input_count.load()) +
        ", chats=" + std::to_string(this->chat_count.load()));

    // 4) 실제 I/O completion을 모두 처리한 뒤에만 null completion으로 worker를 깨운다.
    if(this->iocp_handle != nullptr){
        for(std::size_t i=0; i<this->completion_workers.size(); ++i){
            if(!PostQueuedCompletionStatus(this->iocp_handle, 0, 0, nullptr)){
                this->Logging(LogLevel::Error, "[IocpWorldServer] worker wake failed: " + std::to_string(GetLastError()));
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
            this->Logging(LogLevel::Error, "[IocpWorldServer] WSACleanup failed: " + std::to_string(WSAGetLastError()));
        }
    }
    this->server_state.store(ServerState::Stopped);
    this->Logging(LogLevel::Info, "[IocpWorldServer] server stopped");
}

void IocpWorldServer::Server_Run(){
    const World::Config& config = this->world.Get_Config();
    this->Logging(LogLevel::Info, "[IocpWorldServer] server starting, port=" + std::to_string(this->server_port) +
        ", tick_ms=" + std::to_string(this->tick_period.count()) +
        ", world_size=" + std::to_string(static_cast<int>(config.world_size)) +
        ", cell_size=" + std::to_string(static_cast<int>(config.cell_size)) +
        ", grid=" + std::to_string(this->world.Columns()) + "x" + std::to_string(this->world.Columns()) +
        ", max_speed=" + std::to_string(static_cast<int>(config.max_speed)) +
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

    // 틱 루프를 accept 전에 띄워 첫 Enter job부터 바로 처리한다.
    this->game_thread = std::thread(&IocpWorldServer::Game_Tick_Loop, this);
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
    this->Logging(LogLevel::Info, "[IocpWorldServer] AcceptEx initial posts=" + std::to_string(posted_accepts));

    // Server_Run 스레드는 Stop 신호를 기다리면서 주기적으로 큐 길이와 처리량을 출력하는 monitor 역할을 한다.
    {
        std::unique_lock<std::mutex> lock(this->server_mutex);
        while(!this->run_cv.wait_for(lock, this->status_interval, [this]{
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

void IocpWorldServer::Stop(){
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
    this->Logging(LogLevel::Info, "[IocpWorldServer] stop requested");
    this->run_cv.notify_all();
}

void IocpWorldServer::Proceed_Stop(){
    this->Stop();
    this->Clean_Up();
}
