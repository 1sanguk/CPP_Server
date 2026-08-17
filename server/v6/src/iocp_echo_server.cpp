#include <iocp_echo_server.h>

#include <algorithm>
#include <chrono>
#include <iostream>

constexpr int kBacklog = 128;
constexpr unsigned int kAcceptDepth = 16;
constexpr auto kCleanupDiagnosticInterval = std::chrono::seconds(5);

IocpEchoServer::IocpEchoServer(int port) : server_port(port){}

IocpEchoServer::~IocpEchoServer(){
    // 명시적 Stop() 호출 여부와 관계없이 동일한 종료 절차를 한 번 거친다.
    this->Proceed_Stop();
}

void IocpEchoServer::Logging(LogLevel level, const std::string& message){
    if(level < this->minimum_log_level){
        return;
    }
    std::lock_guard<std::mutex> lock(this->log_mutex);
    std::cout << message << std::endl;
}

bool IocpEchoServer::Init_Winsock(){
    // WSAStartup/WSACleanup 호출 횟수를 맞추기 위해 중복 초기화를 막는다.
    if(this->winsock_init_state){
        return true;
    }
    int startup_result = WSAStartup(MAKEWORD(2,2), &this->wsaData);
    if(startup_result != 0){
        this->Logging(LogLevel::Error, "[IocpEchoServer] WSAStartup failed: " + std::to_string(startup_result));
        return false;
    }
    this->winsock_init_state = true;
    this->Logging(LogLevel::Info, "[IocpEchoServer] Winsock initialized");
    return true;
}

bool IocpEchoServer::Create_Listen_Socket(){
    if(this->server_port < 0 || this->server_port > 65535){
        this->Logging(LogLevel::Error, "[IocpEchoServer] invalid port: " + std::to_string(this->server_port));
        return false;
    }

    // IOCP에 게시할 수 있도록 반드시 overlapped 속성으로 socket을 생성한다.
    this->listen_socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if(this->listen_socket == INVALID_SOCKET){
        this->Logging(LogLevel::Error, "[IocpEchoServer] listen socket creation failed: " + std::to_string(WSAGetLastError()));
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<u_short>(this->server_port));

    if(bind(this->listen_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR){
        this->Logging(LogLevel::Error, "[IocpEchoServer] bind failed: " + std::to_string(WSAGetLastError()));
        closesocket(this->listen_socket);
        this->listen_socket = INVALID_SOCKET;
        return false;
    }
    if(listen(this->listen_socket, kBacklog) == SOCKET_ERROR){
        this->Logging(LogLevel::Error, "[IocpEchoServer] listen failed: " + std::to_string(WSAGetLastError()));
        closesocket(this->listen_socket);
        this->listen_socket = INVALID_SOCKET;
        return false;
    }

    this->Logging(LogLevel::Info, "[IocpEchoServer] listening on port " + std::to_string(this->server_port));
    return true;
}

bool IocpEchoServer::Get_AcceptEx(){
    // AcceptEx는 Winsock 확장 함수라 실행 중인 provider에서 함수 포인터를 얻어야 한다.
    GUID guid = WSAID_ACCEPTEX;
    DWORD bytes_returned{};
    int result = WSAIoctl(this->listen_socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guid, sizeof(guid), &this->lpfn_acceptex, sizeof(this->lpfn_acceptex),
        &bytes_returned, nullptr, nullptr);
    if(result == SOCKET_ERROR){
        this->Logging(LogLevel::Error, "[IocpEchoServer] WSAIoctl failed: " + std::to_string(WSAGetLastError()));
        return false;
    }
    this->Logging(LogLevel::Info, "[IocpEchoServer] AcceptEx function loaded");
    return true;
}

bool IocpEchoServer::Create_Completion_Port(){
    // 빈 completion port를 만든 다음 listen socket을 먼저 연결한다.
    this->iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if(this->iocp_handle == nullptr){
        this->Logging(LogLevel::Error, "[IocpEchoServer] IOCP creation failed: " + std::to_string(GetLastError()));
        return false;
    }
    if(CreateIoCompletionPort(reinterpret_cast<HANDLE>(this->listen_socket), this->iocp_handle, 0, 0) == nullptr){
        this->Logging(LogLevel::Error, "[IocpEchoServer] listen IOCP registration failed: " + std::to_string(GetLastError()));
        return false;
    }
    this->Logging(LogLevel::Info, "[IocpEchoServer] completion port created");
    return true;
}

void IocpEchoServer::Start_Completion_Workers(){
    // completion worker 수는 접속 수와 무관하며 논리 코어 수를 기준으로 고정한다.
    unsigned int detected = std::thread::hardware_concurrency();
    unsigned int count = detected == 0 ? 1 : detected;
    this->completion_workers.reserve(count);
    for(unsigned int i=0; i<count; ++i){
        this->completion_workers.emplace_back(&IocpEchoServer::Completion_worker_Loop, this, i);
    }
    this->Logging(LogLevel::Info,
        "[IocpEchoServer] completion workers started, count=" + std::to_string(count));
}

bool IocpEchoServer::Post_Accept(){
    // Accept 작업마다 accept socket과 OVERLAPPED를 별도로 만든다. 시작 시 여러 개를
    // 선게시하고, 각 completion에서 하나씩 보충해 accept depth를 유지한다.
    auto* context = new IoContext();
    context->io_type = IoContext::IoType::Accept;
    context->accept_socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if(context->accept_socket == INVALID_SOCKET){
        this->Logging(LogLevel::Error, "[IocpEchoServer] accept socket creation failed: " + std::to_string(WSAGetLastError()));
        delete context;
        return false;
    }

    DWORD bytes_received{};
    DWORD address_length = sizeof(sockaddr_in) + 16;
    int error = 0;
    {
        std::lock_guard<std::mutex> lock(this->context_mutex);
        if(this->server_state.load() != ServerState::Running){
            closesocket(context->accept_socket);
            delete context;
            return false;
        }
        this->pending_contexts.emplace(context);
        // 동기 성공이어도 completion packet은 IOCP로 전달되므로 여기서 직접 처리하지 않는다.
        bool result = this->lpfn_acceptex(this->listen_socket, context->accept_socket,
            context->buffer, 0, address_length, address_length,
            &bytes_received, &context->overlapped);
        if(!result){
            error = WSAGetLastError();
            if(error != WSA_IO_PENDING){
                this->pending_contexts.erase(context);
            }
        }
    }

    if(error != 0 && error != WSA_IO_PENDING){
        this->Logging(LogLevel::Error, "[IocpEchoServer] AcceptEx failed: " + std::to_string(error));
        closesocket(context->accept_socket);
        delete context;
        this->context_condition.notify_all();
        return false;
    }
    this->Logging(LogLevel::Debug, "[IocpEchoServer] AcceptEx posted");
    return true;
}

bool IocpEchoServer::Post_Recv(const std::shared_ptr<ClientSession>& session){
    // Recv마다 독립 버퍼를 가진 context를 사용한다. Send context와 분리되어 있으므로
    // 이전 echo 송신이 끝나기 전에도 다음 WSARecv를 게시할 수 있다.
    auto* context = new IoContext();
    context->io_type = IoContext::IoType::Recv;
    context->session = session;
    context->wsa_buffer.buf = context->buffer;
    context->wsa_buffer.len = sizeof(context->buffer);

    int error = 0;
    SOCKET socket_value = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> socket_lock(session->socket_mutex);
        std::lock_guard<std::mutex> context_lock(this->context_mutex);
        if(this->server_state.load() != ServerState::Running || session->closing.load()){
            delete context;
            return false;
        }
        socket_value = session->client_socket;
        this->pending_contexts.emplace(context);
        int result = WSARecv(session->client_socket, &context->wsa_buffer, 1, nullptr,
            &context->flags, &context->overlapped, nullptr);
        if(result == SOCKET_ERROR){
            error = WSAGetLastError();
            if(error != WSA_IO_PENDING){
                this->pending_contexts.erase(context);
            }
        }
    }

    if(error != 0 && error != WSA_IO_PENDING){
        this->Logging(LogLevel::Error, "[IocpEchoServer] WSARecv failed: " + std::to_string(error));
        delete context;
        this->context_condition.notify_all();
        return false;
    }
    this->Logging(LogLevel::Debug,
        "[IocpEchoServer] WSARecv posted, socket=" + std::to_string(socket_value));
    return true;
}

bool IocpEchoServer::Post_Next_Send_Locked(const std::shared_ptr<ClientSession>& session){
    // 호출자는 send_mutex를 보유한다. 큐 맨 앞 데이터의 아직 보내지 않은 범위만 게시한다.
    if(session->send_queue.empty() || session->closing.load()){
        session->send_pending = false;
        return !session->closing.load();
    }

    auto* context = new IoContext();
    context->io_type = IoContext::IoType::Send;
    context->session = session;
    auto& data = session->send_queue.front();
    context->wsa_buffer.buf = data.data() + session->send_offset;
    context->logical_send_length = data.size() - session->send_offset;
    context->wsa_buffer.len = static_cast<ULONG>(context->logical_send_length);
#ifdef V6_FORCE_PARTIAL_SEND_TEST
    // OS 상황에 의존하지 않고 continuation 경로를 반복 검증하는 테스트 전용 제한이다.
    context->wsa_buffer.len = (std::min)(context->wsa_buffer.len, 1024UL);
#endif

    int error = 0;
    SOCKET socket_value = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> socket_lock(session->socket_mutex);
        std::lock_guard<std::mutex> context_lock(this->context_mutex);
        if(this->server_state.load() != ServerState::Running || session->closing.load()){
            delete context;
            session->send_pending = false;
            return false;
        }
        socket_value = session->client_socket;
        this->pending_contexts.emplace(context);
        int result = WSASend(session->client_socket, &context->wsa_buffer, 1, nullptr, 0,
            &context->overlapped, nullptr);
        if(result == SOCKET_ERROR){
            error = WSAGetLastError();
            if(error != WSA_IO_PENDING){
                this->pending_contexts.erase(context);
            }
        }
    }

    if(error != 0 && error != WSA_IO_PENDING){
        this->Logging(LogLevel::Error, "[IocpEchoServer] WSASend failed: " + std::to_string(error));
        delete context;
        session->send_pending = false;
        this->context_condition.notify_all();
        return false;
    }
    this->Logging(LogLevel::Debug,
        "[IocpEchoServer] WSASend posted, socket=" + std::to_string(socket_value) +
        ", requested=" + std::to_string(context->wsa_buffer.len) +
        ", logical_remaining=" + std::to_string(context->logical_send_length));
    return true;
}

bool IocpEchoServer::Enqueue_Send(const std::shared_ptr<ClientSession>& session, const char* data, std::size_t length){
    // Recv가 완료된 순서대로 복사해 둔다. 이미 Send가 pending이면 새 데이터를 큐에만 넣고,
    // pending이 없을 때만 큐의 첫 전송을 시작한다.
    bool result = true;
    {
        std::lock_guard<std::mutex> lock(session->send_mutex);
        if(session->closing.load()){
            return false;
        }
        session->send_queue.emplace_back(data, data + length);
        if(!session->send_pending){
            session->send_pending = true;
            result = this->Post_Next_Send_Locked(session);
        }
    }
    if(!result){
        this->Close_Session(session);
    }
    return result;
}

void IocpEchoServer::Finish_Context(IoContext* context){
    // GQCS가 completion을 반환한 뒤에는 OS가 OVERLAPPED를 더 이상 사용하지 않는다.
    {
        std::lock_guard<std::mutex> lock(this->context_mutex);
        this->pending_contexts.erase(context);
    }
    this->context_condition.notify_all();
}

void IocpEchoServer::Close_Session(const std::shared_ptr<ClientSession>& session){
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
        session->send_offset = 0;
        session->send_pending = false;
    }
    if(socket_to_close != INVALID_SOCKET){
        closesocket(socket_to_close);
        this->Logging(LogLevel::Debug, "[IocpEchoServer] client closed, socket=" + std::to_string(socket_to_close));
    }

    std::lock_guard<std::mutex> lock(this->session_mutex);
    this->sessions.erase(session);
    this->Logging(LogLevel::Debug,
        "[IocpEchoServer] active sessions=" + std::to_string(this->sessions.size()));
}

void IocpEchoServer::Handle_Accept_Completion(IoContext* context, bool succeeded){
    SOCKET accepted_socket = context->accept_socket;
    context->accept_socket = INVALID_SOCKET;

    // 종료 중 취소된 AcceptEx도 실패 completion으로 돌아오므로 조용히 socket만 회수한다.
    if(!succeeded || this->server_state.load() != ServerState::Running){
        if(accepted_socket != INVALID_SOCKET){
            closesocket(accepted_socket);
        }
        delete context;
        return;
    }

    if(setsockopt(accepted_socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
        reinterpret_cast<char*>(&this->listen_socket), sizeof(this->listen_socket)) == SOCKET_ERROR){
        this->Logging(LogLevel::Error, "[IocpEchoServer] SO_UPDATE_ACCEPT_CONTEXT failed: " + std::to_string(WSAGetLastError()));
        closesocket(accepted_socket);
        delete context;
        this->Post_Accept();
        return;
    }

#ifdef V6_FORCE_PARTIAL_SEND_TEST
    int send_buffer_size = 1024;
    setsockopt(accepted_socket, SOL_SOCKET, SO_SNDBUF,
        reinterpret_cast<char*>(&send_buffer_size), sizeof(send_buffer_size));
#endif

    if(CreateIoCompletionPort(reinterpret_cast<HANDLE>(accepted_socket), this->iocp_handle, 0, 0) == nullptr){
        this->Logging(LogLevel::Error, "[IocpEchoServer] client IOCP registration failed: " + std::to_string(GetLastError()));
        closesocket(accepted_socket);
        delete context;
        this->Post_Accept();
        return;
    }

    auto session = std::make_shared<ClientSession>(accepted_socket);
    std::size_t active_session_count = 0;
    {
        std::lock_guard<std::mutex> lock(this->session_mutex);
        this->sessions.emplace(session);
        active_session_count = this->sessions.size();
    }
    this->Logging(LogLevel::Debug,
        "[IocpEchoServer] client accepted, socket=" + std::to_string(accepted_socket) +
        ", active_sessions=" + std::to_string(active_session_count));
    delete context;
    // 완료된 Accept 하나를 즉시 보충한 뒤 새 client의 첫 Recv를 게시한다.
    this->Post_Accept();
    if(!this->Post_Recv(session)){
        this->Close_Session(session);
    }
}

void IocpEchoServer::Handle_Recv_Completion(IoContext* context, bool succeeded, DWORD byte_transferred){
    auto session = context->session;
    if(!succeeded || byte_transferred == 0 || this->server_state.load() != ServerState::Running){
        delete context;
        this->Close_Session(session);
        return;
    }

    SOCKET socket_value = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(session->socket_mutex);
        socket_value = session->client_socket;
    }
    this->Logging(LogLevel::Debug,
        "[IocpEchoServer] recv completed, socket=" + std::to_string(socket_value) +
        ", bytes=" + std::to_string(byte_transferred));
    // 받은 바이트는 송신 큐로 복사되므로 Recv context를 지운 뒤에도 안전하다.
    bool send_result = this->Enqueue_Send(session, context->buffer, byte_transferred);
    delete context;
    if(send_result && !this->Post_Recv(session)){
        this->Close_Session(session);
    }
}

void IocpEchoServer::Handle_Send_Completion(IoContext* context, bool succeeded, DWORD byte_transferred){
    auto session = context->session;
    bool post_result = true;
    SOCKET socket_value = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(session->socket_mutex);
        socket_value = session->client_socket;
    }
    {
        std::lock_guard<std::mutex> lock(session->send_mutex);
        if(!succeeded || byte_transferred == 0 || session->closing.load() || session->send_queue.empty()){
            post_result = false;
        }else{
            if(byte_transferred < context->logical_send_length){
                ++this->partial_send_count;
                this->Logging(LogLevel::Debug,
                    "[IocpEchoServer] partial send completed, socket=" +
                    std::to_string(socket_value) + ", bytes=" +
                    std::to_string(byte_transferred) + ", logical_remaining=" +
                    std::to_string(context->logical_send_length));
            }
            // 완료된 양만큼 offset을 전진시킨다. 메시지를 전부 보냈으면 다음 큐 항목으로 간다.
            session->send_offset += byte_transferred;
            if(session->send_offset >= session->send_queue.front().size()){
                session->send_queue.pop_front();
                session->send_offset = 0;
            }
            if(session->send_queue.empty()){
                session->send_pending = false;
            }else{
                post_result = this->Post_Next_Send_Locked(session);
            }
            this->Logging(LogLevel::Debug,
                "[IocpEchoServer] send completed, socket=" + std::to_string(socket_value) +
                ", bytes=" + std::to_string(byte_transferred) +
                ", queued_messages=" + std::to_string(session->send_queue.size()));
        }
    }
    delete context;
    if(!post_result){
        this->Close_Session(session);
    }
}

void IocpEchoServer::Completion_worker_Loop(unsigned int index){
    this->Logging(LogLevel::Info, "[IocpEchoServer] worker " + std::to_string(index) + " started");
    while(true){
        DWORD byte_transferred{};
        ULONG_PTR completion_key{};
        LPOVERLAPPED overlapped_ptr{};
        bool succeeded = GetQueuedCompletionStatus(this->iocp_handle, &byte_transferred,
            &completion_key, &overlapped_ptr, INFINITE);
        DWORD completion_error = succeeded ? ERROR_SUCCESS : GetLastError();
        // Clean_Up()이 worker 수만큼 넣는 null OVERLAPPED가 유일한 정상 종료 신호다.
        if(overlapped_ptr == nullptr){
            if(!succeeded){
                this->Logging(LogLevel::Error,
                    "[IocpEchoServer] GetQueuedCompletionStatus failed without context: " +
                    std::to_string(completion_error));
            }
            break;
        }

        // OVERLAPPED가 첫 멤버라는 가정 없이 실제 소유 IoContext 주소를 계산한다.
        IoContext* context = CONTAINING_RECORD(overlapped_ptr, IoContext, overlapped);
        if(!succeeded){
            const char* io_type = "Unknown";
            switch(context->io_type){
                case IoContext::IoType::Accept:
                    io_type = "Accept";
                    break;
                case IoContext::IoType::Recv:
                    io_type = "Recv";
                    break;
                case IoContext::IoType::Send:
                    io_type = "Send";
                    break;
            }
            LogLevel level = this->server_state.load() == ServerState::Running
                ? LogLevel::Error : LogLevel::Debug;
            this->Logging(level,
                std::string("[IocpEchoServer] overlapped I/O failed, type=") + io_type +
                ", error=" + std::to_string(completion_error));
        }
        this->Finish_Context(context);
        switch(context->io_type){
            case IoContext::IoType::Accept:
                this->Handle_Accept_Completion(context, succeeded);
                break;
            case IoContext::IoType::Recv:
                this->Handle_Recv_Completion(context, succeeded, byte_transferred);
                break;
            case IoContext::IoType::Send:
                this->Handle_Send_Completion(context, succeeded, byte_transferred);
                break;
        }
    }
    this->Logging(LogLevel::Info, "[IocpEchoServer] worker " + std::to_string(index) + " stopped");
}

void IocpEchoServer::Clean_Up(){
    {
        std::lock_guard<std::mutex> lock(this->server_mutex);
        ServerState expected = ServerState::Stopping;
        if(!this->server_state.compare_exchange_strong(expected, ServerState::Cleaning)){
            return;
        }
    }
    this->Logging(LogLevel::Info, "[IocpEchoServer] cleanup started");

    // client socket을 닫으면 해당 socket의 pending Recv/Send가 취소 completion으로 돌아온다.
    std::vector<std::shared_ptr<ClientSession>> sessions_to_close;
    // 아직 완료되지 않은 AcceptEx는 각 accept socket을 닫아 취소한다.
    {
        std::lock_guard<std::mutex> lock(this->session_mutex);
        sessions_to_close.assign(this->sessions.begin(), this->sessions.end());
    }
    for(const auto& session : sessions_to_close){
        this->Close_Session(session);
    }

    // Context를 강제로 삭제하지 않는다. OS가 반환한 completion을 worker가 모두 회수할
    // 때까지 기다리며, 비정상 지연은 일정 주기로 남은 개수를 출력해 진단한다.
    {
        std::lock_guard<std::mutex> lock(this->context_mutex);
        for(IoContext* context : this->pending_contexts){
            if(context->io_type == IoContext::IoType::Accept && context->accept_socket != INVALID_SOCKET){
                closesocket(context->accept_socket);
                context->accept_socket = INVALID_SOCKET;
            }
        }
    }

    {
        std::unique_lock<std::mutex> lock(this->context_mutex);
        while(!this->pending_contexts.empty()){
            if(this->context_condition.wait_for(lock, kCleanupDiagnosticInterval) == std::cv_status::timeout){
                this->Logging(LogLevel::Error,
                    "[IocpEchoServer] still waiting for " + std::to_string(this->pending_contexts.size()) + " pending I/O context(s)");
            }
        }
    }

    this->Logging(LogLevel::Info,
        "[IocpEchoServer] pending I/O drained, partial sends=" + std::to_string(this->partial_send_count.load()));

    // 실제 I/O completion을 모두 처리한 뒤에만 null completion으로 worker를 깨운다.
    if(this->iocp_handle != nullptr){
        for(std::size_t i=0; i<this->completion_workers.size(); ++i){
            if(!PostQueuedCompletionStatus(this->iocp_handle, 0, 0, nullptr)){
                this->Logging(LogLevel::Error, "[IocpEchoServer] worker wake failed: " + std::to_string(GetLastError()));
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
            this->Logging(LogLevel::Error, "[IocpEchoServer] WSACleanup failed: " + std::to_string(WSAGetLastError()));
        }
    }
    this->server_state.store(ServerState::Stopped);
    this->Logging(LogLevel::Info, "[IocpEchoServer] server stopped");
}

void IocpEchoServer::Server_Run(){
    this->Logging(LogLevel::Info,
        "[IocpEchoServer] server starting, port=" + std::to_string(this->server_port));
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
    this->Logging(LogLevel::Info,
        "[IocpEchoServer] AcceptEx initial posts=" + std::to_string(posted_accepts));

    // 네트워크 처리는 completion worker가 담당한다. Server_Run 스레드는 Stop 신호만 기다린다.
    {
        std::unique_lock<std::mutex> lock(this->server_mutex);
        this->run_cv.wait(lock, [this]{
            return this->server_state.load() != ServerState::Running;
        });
    }
    this->Proceed_Stop();
}

void IocpEchoServer::Stop(){
    SOCKET listen_to_close = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(this->server_mutex);
        ServerState expected = ServerState::Running;
        if(!this->server_state.compare_exchange_strong(expected, ServerState::Stopping)){
            expected = ServerState::Creating;
            this->server_state.compare_exchange_strong(expected, ServerState::Stopped);
            return;
        }
        listen_to_close = this->listen_socket;
        this->listen_socket = INVALID_SOCKET;
    }
    if(listen_to_close != INVALID_SOCKET){
        closesocket(listen_to_close);
    }
    this->Logging(LogLevel::Info, "[IocpEchoServer] stop requested");
    this->run_cv.notify_all();
}

void IocpEchoServer::Proceed_Stop(){
    this->Stop();
    this->Clean_Up();
}
