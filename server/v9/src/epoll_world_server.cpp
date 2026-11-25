#include <epoll_world_server.h>

#include <tick_scheduler.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>

namespace{

constexpr int kBacklog = 512;
constexpr int kMaxEvents = 1024;
// 한 번의 recv로 읽는 크기. LT 모드라 다 못 읽어도 다음 epoll_wait가 다시 알려 준다.
constexpr std::size_t kRecvBufferSize = 64 * 1024;
// 한 fd를 한 번 깨어났을 때 읽는 최대 횟수. 한 연결이 reactor를 독차지하지 않게 한다.
constexpr int kMaxReadsPerEvent = 4;
// writev 한 번에 묶는 버퍼 수와 바이트(v8의 WSASend 묶음과 같은 값)
constexpr std::size_t kMaxIovPerSend = 64;
constexpr std::size_t kMaxBytesPerSend = 256 * 1024;
constexpr std::size_t kGameJobCapacity = 65536;
constexpr std::size_t kDefaultJobHighWatermark = 8192;
constexpr std::size_t kDefaultJobLowWatermark = 1024;
constexpr std::size_t kMaxSendQueueBytes = 4 * 1024 * 1024;
constexpr int kDefaultTickMs = 50;
constexpr int kDefaultStatusIntervalMs = 10000;
constexpr int kDefaultChatsPerTick = 4;
constexpr int kDefaultInputTimeoutMs = 500;
// 한 틱에 처리할 입장·퇴장 수. 시야 안 1,000명(셀 하나)일 때 하나가 수십 µs라, 64면 입장·퇴장 처리가 틱당 수 ms 안쪽이다.
// 평소(2,000명이 퍼져 있음)에는 한 틱에 이만큼 몰리지 않아 미뤄지지 않는다.
constexpr int kDefaultMaxEnterLeavePerTick = 64;
constexpr const char* kTickPhaseNames[] = {"input", "step", "collect", "encode", "send"};

// epoll 이벤트의 data.u64. 세션은 session_id(1부터)를 넣고, 특별한 fd는 맨 끝 값을 쓴다.
constexpr std::uint64_t kListenKey = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kWakeKey = std::numeric_limits<std::uint64_t>::max() - 1;

std::uint64_t Elapsed_Us(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to){
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
}

std::string Errno_Text(int error){
    return std::to_string(error) + " (" + std::strerror(error) + ")";
}

}

ClientSession::ClientSession(int fd_value, std::uint32_t id) : fd(fd_value), session_id(id){}

EpollWorldServer::EpollWorldServer(int port)
    : server_port(port), wake_fd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      recv_buffer(kRecvBufferSize), game_jobs(kGameJobCapacity),
      tick_period(static_cast<long long>(Read_Number_From_Env("V9_TICK_MS", kDefaultTickMs, 1, 1000))),
      status_interval(static_cast<long long>(
          Read_Number_From_Env("V9_STATUS_INTERVAL_MS", kDefaultStatusIntervalMs, 100, 3600000))),
      world(Read_World_Config_From_Env()),
      chats_per_tick(static_cast<std::uint32_t>(Read_Number_From_Env("V9_CHATS_PER_TICK", kDefaultChatsPerTick, 1, 1000000))),
      input_timeout_ticks(Input_Timeout_Ticks(
          Read_Number_From_Env("V9_INPUT_TIMEOUT_MS", kDefaultInputTimeoutMs, 0, 600000), tick_period.count())),
      spawn_area(Read_Spawn_Area_From_Env()),
      max_enter_leave_per_tick(static_cast<std::size_t>(Read_Number_From_Env("V9_MAX_ENTER_LEAVE_PER_TICK",
          kDefaultMaxEnterLeavePerTick, 1, 1000000))),
      job_high_watermark(static_cast<std::size_t>(Read_Number_From_Env("V9_JOB_HIGH_WATERMARK",
          static_cast<double>(kDefaultJobHighWatermark), 2, static_cast<double>(kGameJobCapacity - 1)))),
      job_low_watermark((std::min)(static_cast<std::size_t>(Read_Number_From_Env("V9_JOB_LOW_WATERMARK",
          static_cast<double>(kDefaultJobLowWatermark), 1, static_cast<double>(kGameJobCapacity - 1))),
          job_high_watermark / 2)),
      logger(std::cout),
      minimum_log_level(Read_Log_Level_From_Env()){
    this->logger.Start();
}

EpollWorldServer::~EpollWorldServer(){
    // Server_Run이 정리까지 마치고 돌아온다. 여기서는 마지막 로그를 내보내고 writer 스레드를 정리한다.
    // wake_fd는 Stop()을 부를 수 있는 모든 스레드가 끝난 뒤(소멸자)에만 닫는다.
    if(this->wake_fd >= 0){
        close(this->wake_fd);
    }
    this->logger.Stop();
}

// ---------------------------------------------------------------- 설정

EpollWorldServer::LogLevel EpollWorldServer::Read_Log_Level_From_Env(){
    const char* raw = std::getenv("V9_LOG_LEVEL");
    if(raw == nullptr){
        return LogLevel::Info;
    }
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if(value == "debug") return LogLevel::Debug;
    if(value == "error") return LogLevel::Error;
    return LogLevel::Info;
}

double EpollWorldServer::Read_Number_From_Env(const char* name, double default_value, double min_value, double max_value){
    const char* raw = std::getenv(name);
    if(raw == nullptr || *raw == '\0'){
        return default_value;
    }
    char* end = nullptr;
    const double value = std::strtod(raw, &end);
    if(end == raw || !(value >= min_value && value <= max_value)){
        return default_value;
    }
    return value;
}

World::Config EpollWorldServer::Read_World_Config_From_Env(){
    World::Config config;
    config.world_size = static_cast<float>(Read_Number_From_Env("V9_WORLD_SIZE", config.world_size, 10, 100000));
    // V9_CELL_SIZE를 월드 크기 이상으로 주면 셀이 하나가 되어 AOI를 끈 것(전체 방송)과 같다.
    config.cell_size = static_cast<float>(Read_Number_From_Env("V9_CELL_SIZE", config.cell_size, 1, 100000));
    config.max_speed = static_cast<float>(Read_Number_From_Env("V9_MAX_SPEED", config.max_speed, 0, 100000));
    config.seed = static_cast<std::uint32_t>(Read_Number_From_Env("V9_SEED", config.seed, 0, 4294967295.0));
    return config;
}

std::optional<EpollWorldServer::SpawnArea> EpollWorldServer::Read_Spawn_Area_From_Env(){
    const double x = Read_Number_From_Env("V9_SPAWN_X", -1, 0, 100000);
    const double y = Read_Number_From_Env("V9_SPAWN_Y", -1, 0, 100000);
    const double radius = Read_Number_From_Env("V9_SPAWN_RADIUS", -1, 0, 100000);
    if(x < 0 || y < 0 || radius < 0){
        return std::nullopt;
    }
    return SpawnArea{static_cast<float>(x), static_cast<float>(y), static_cast<float>(radius)};
}

std::uint64_t EpollWorldServer::Input_Timeout_Ticks(double timeout_ms, long long tick_ms){
    if(!(timeout_ms > 0.0) || tick_ms <= 0){
        return 0;
    }
    const auto ticks = static_cast<std::uint64_t>(std::ceil(timeout_ms / static_cast<double>(tick_ms)));
    return (std::max)(std::uint64_t{1}, ticks);
}

void EpollWorldServer::Logging(LogLevel level, const std::string& message){
    if(level < this->minimum_log_level){
        return;
    }
    this->logger.Write(message);
}

// ---------------------------------------------------------------- reactor 준비

bool EpollWorldServer::Create_Listen_Socket(){
    // SOCK_NONBLOCK: accept를 EAGAIN이 날 때까지 반복해 backlog를 한 번에 비운다.
    this->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if(this->listen_fd < 0){
        this->Logging(LogLevel::Error, "[EpollWorldServer] socket failed: " + Errno_Text(errno));
        return false;
    }
    // 서버를 껐다 바로 켤 때 TIME_WAIT 때문에 bind가 실패하지 않게 한다.
    int reuse = 1;
    setsockopt(this->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(this->server_port));
    if(bind(this->listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0){
        this->Logging(LogLevel::Error, "[EpollWorldServer] bind failed: " + Errno_Text(errno));
        return false;
    }
    if(listen(this->listen_fd, kBacklog) < 0){
        this->Logging(LogLevel::Error, "[EpollWorldServer] listen failed: " + Errno_Text(errno));
        return false;
    }
    return true;
}

bool EpollWorldServer::Create_Epoll(){
    this->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if(this->epoll_fd < 0){
        this->Logging(LogLevel::Error, "[EpollWorldServer] epoll_create1 failed: " + Errno_Text(errno));
        return false;
    }
    // 틱 스레드의 송신 요청과 Stop()이 write하면 epoll_wait가 깨어난다(v5의 stop_event_fd를 넓힌 것).
    // eventfd는 생성자에서 이미 만들었다. 실패했으면 여기서 알린다.
    if(this->wake_fd < 0){
        this->Logging(LogLevel::Error, "[EpollWorldServer] eventfd failed: " + Errno_Text(errno));
        return false;
    }
    epoll_event listen_event{};
    listen_event.events = EPOLLIN;
    listen_event.data.u64 = kListenKey;
    epoll_event wake_event{};
    wake_event.events = EPOLLIN;
    wake_event.data.u64 = kWakeKey;
    if(epoll_ctl(this->epoll_fd, EPOLL_CTL_ADD, this->listen_fd, &listen_event) < 0 ||
        epoll_ctl(this->epoll_fd, EPOLL_CTL_ADD, this->wake_fd, &wake_event) < 0){
        this->Logging(LogLevel::Error, "[EpollWorldServer] epoll_ctl(ADD) failed: " + Errno_Text(errno));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- reactor 루프

void EpollWorldServer::Server_Run(){
    const World::Config& config = this->world.Get_Config();
    this->Logging(LogLevel::Info, "[EpollWorldServer] server starting, port=" + std::to_string(this->server_port) +
        ", tick_ms=" + std::to_string(this->tick_period.count()) +
        ", world_size=" + std::to_string(static_cast<int>(config.world_size)) +
        ", cell_size=" + std::to_string(static_cast<int>(config.cell_size)) +
        ", grid=" + std::to_string(this->world.Columns()) + "x" + std::to_string(this->world.Columns()) +
        ", max_speed=" + std::to_string(static_cast<int>(config.max_speed)) +
        ", input_timeout_ticks=" + std::to_string(this->input_timeout_ticks) +
        (this->spawn_area
            ? ", spawn=(" + std::to_string(static_cast<int>(this->spawn_area->x)) + "," +
              std::to_string(static_cast<int>(this->spawn_area->y)) + ") r=" +
              std::to_string(static_cast<int>(this->spawn_area->radius))
            : std::string(", spawn=random")) +
        ", job_high_watermark=" + std::to_string(this->job_high_watermark) +
        ", job_low_watermark=" + std::to_string(this->job_low_watermark));

    ServerState expected = ServerState::Creating;
    if(!this->server_state.compare_exchange_strong(expected, ServerState::Running)){
        return;
    }
    if(!this->Create_Listen_Socket() || !this->Create_Epoll()){
        this->server_state.store(ServerState::Stopping);
        this->Clean_Up();
        return;
    }
    // 틱 루프를 accept 전에 띄워 첫 Enter job부터 바로 처리한다.
    this->game_thread = std::thread(&EpollWorldServer::Game_Tick_Loop, this);
    this->Logging(LogLevel::Info, "[EpollWorldServer] listening, reactor thread runs epoll loop");
    // 스레드 이름: /proc/<pid>/task/<tid>/comm에 보여 스레드별 CPU 사용률을 잴 수 있다(tools/run_load.sh).
    pthread_setname_np(pthread_self(), "v9-reactor");

    this->Reactor_Loop();
    this->Log_Status();
    this->Clean_Up();
}

void EpollWorldServer::Stop(){
    ServerState expected = ServerState::Running;
    if(!this->server_state.compare_exchange_strong(expected, ServerState::Stopping)){
        expected = ServerState::Creating;
        this->server_state.compare_exchange_strong(expected, ServerState::Stopped);
        return;
    }
    this->Logging(LogLevel::Info, "[EpollWorldServer] stop requested");
    this->Wake_Reactor();
}

void EpollWorldServer::Reactor_Loop(){
    std::vector<epoll_event> events(kMaxEvents);
    auto next_status = std::chrono::steady_clock::now() + this->status_interval;

    while(this->server_state.load() == ServerState::Running){
        // 다음 상태 출력 시각까지만 기다린다. 이벤트가 오면 그 전에 깨어난다.
        const auto now = std::chrono::steady_clock::now();
        const auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(next_status - now).count();
        const int timeout = static_cast<int>((std::max)(0LL, static_cast<long long>(wait_ms)));
        const int count = epoll_wait(this->epoll_fd, events.data(), static_cast<int>(events.size()), timeout);
        ++this->epoll_wait_count;
        if(count < 0){
            if(errno == EINTR){
                continue;
            }
            this->Logging(LogLevel::Error, "[EpollWorldServer] epoll_wait failed: " + Errno_Text(errno));
            break;
        }

        for(int i = 0; i < count; ++i){
            const std::uint64_t key = events[i].data.u64;
            const std::uint32_t flags = events[i].events;
            if(key == kListenKey){
                this->Handle_Accept();
                continue;
            }
            if(key == kWakeKey){
                // eventfd 값을 비워야 다음 write 전까지 다시 깨어나지 않는다(LT).
                std::uint64_t value = 0;
                while(read(this->wake_fd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value))){}
                ++this->wake_count;
                this->Process_Requests();
                continue;
            }
            // 이미 닫은 세션의 이벤트가 같은 묶음에 남아 있을 수 있다. 키가 session_id라 재사용된 fd와 섞이지 않는다.
            auto found = this->sessions.find(static_cast<std::uint32_t>(key));
            if(found == this->sessions.end()){
                continue;
            }
            const std::shared_ptr<ClientSession> session = found->second;
            // 오류나 끊김도 recv가 0이나 오류를 돌려주므로 읽기 경로에서 함께 정리된다.
            if(flags & (EPOLLIN | EPOLLHUP | EPOLLERR)){
                this->Handle_Readable(session);
            }
            if((flags & EPOLLOUT) && !session->closing.load()){
                this->Handle_Writable(session);
            }
        }

        this->Retry_Pending_Leaves();
        this->Resume_Paused_Sessions();
        if(std::chrono::steady_clock::now() >= next_status){
            this->Log_Status();
            next_status += this->status_interval;
        }
    }
}

void EpollWorldServer::Handle_Accept(){
    while(true){
        sockaddr_in remote{};
        socklen_t remote_length = sizeof(remote);
        // accept4: 새 fd를 처음부터 non-blocking으로 받는다(v5는 accept 뒤 fcntl을 따로 불렀다).
        const int client_fd = accept4(this->listen_fd, reinterpret_cast<sockaddr*>(&remote), &remote_length,
            SOCK_NONBLOCK | SOCK_CLOEXEC);
        if(client_fd < 0){
            if(errno == EINTR || errno == ECONNABORTED){
                continue;
            }
            if(errno != EAGAIN && errno != EWOULDBLOCK){
                // EMFILE(fd 부족) 등. listen fd는 LT라 다음 루프에서 다시 알려 준다.
                this->Logging(LogLevel::Error, "[EpollWorldServer] accept4 failed: " + Errno_Text(errno));
            }
            return;
        }
        // 작은 이동 패킷을 모아 보내느라 늦어지지 않게 Nagle을 끈다. 묶어 보내기는 틱 단위로 서버가 직접 한다.
        int no_delay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));

        auto session = std::make_shared<ClientSession>(client_fd, this->next_session_id++);
        char ip_text[INET_ADDRSTRLEN]{};
        if(inet_ntop(AF_INET, &remote.sin_addr, ip_text, sizeof(ip_text)) != nullptr){
            session->remote_endpoint = std::string(ip_text) + ":" + std::to_string(ntohs(remote.sin_port));
        }
        this->sessions.emplace(session->session_id, session);
        ++this->session_count;

        // Enter는 epoll 등록(첫 수신) 전에 넣는다. 그래야 이 세션의 Packet job이 Enter보다 먼저 틱에 도착하지 않는다.
        GameJob enter;
        enter.type = GameJob::JobType::Enter;
        enter.session = session;
        if(!this->game_jobs.Push(std::move(enter))){
            ++this->rejected_job_count;
            this->Close_Session(session, "job queue full on accept");
            continue;
        }
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.u64 = session->session_id;
        if(epoll_ctl(this->epoll_fd, EPOLL_CTL_ADD, client_fd, &event) < 0){
            this->Logging(LogLevel::Error, "[EpollWorldServer] epoll_ctl(ADD client) failed: " + Errno_Text(errno));
            this->Close_Session(session, "epoll add failed");
            continue;
        }
        this->Logging(LogLevel::Debug, "[EpollWorldServer] client connected, session=" +
            std::to_string(session->session_id) + ", fd=" + std::to_string(client_fd) +
            ", remote=" + session->remote_endpoint);
    }
}

void EpollWorldServer::Handle_Readable(const std::shared_ptr<ClientSession>& session){
    // 읽기 전에 수위를 먼저 본다. 한 번의 epoll_wait 묶음에서 앞의 fd들이 큐를 높은 수위 위로 올렸다면, 뒤의 fd는 읽지 않고
    // 바로 멈춘다. 보완 전에는 묶음의 fd를 모두 읽은 뒤에 판단해, 수위 200에서 큐가 2,239까지 올라갔다.
    // 끊김·오류(EPOLLHUP/EPOLLERR)는 읽어야 알 수 있으므로 멈춘 상태에서도 아래로 내려가 recv가 0이나 오류를 돌려준다.
    if(!session->recv_paused && this->game_jobs.Size() >= this->job_high_watermark){
        this->Pause_Recv(session);
        return;
    }
    // LT 모드: 몇 번만 읽고 돌아가도 남은 데이터가 있으면 다음 epoll_wait가 다시 알려 준다.
    for(int reads = 0; reads < kMaxReadsPerEvent; ++reads){
        const ssize_t received = recv(session->fd, this->recv_buffer.data(), this->recv_buffer.size(), 0);
        ++this->recv_call_count;
        if(received > 0){
            session->assembler.Append(this->recv_buffer.data(), static_cast<std::size_t>(received));
            if(static_cast<std::size_t>(received) < this->recv_buffer.size()){
                break;   // 커널 수신 버퍼를 다 비웠다
            }
            continue;
        }
        if(received == 0){
            this->Close_Session(session, "peer closed");
            return;
        }
        if(errno == EINTR){
            continue;
        }
        if(errno == EAGAIN || errno == EWOULDBLOCK){
            break;
        }
        this->Close_Session(session, "recv error");
        return;
    }

    // 이번에 완성된 패킷을 모아 한 번의 잠금으로 넣는다(v8 Push_Batch와 같다).
    thread_local std::vector<GameJob> batch;
    batch.clear();
    while(auto packet = session->assembler.Next()){
        GameJob job;
        job.type = GameJob::JobType::Packet;
        job.session = session;
        job.packet = std::move(*packet);
        batch.push_back(std::move(job));
    }
    if(!batch.empty()){
        const std::size_t batch_size = batch.size();
        if(!this->game_jobs.Push_Batch(batch)){
            this->rejected_job_count += batch_size;
            batch.clear();
            this->Close_Session(session, "game job queue hard limit");
            return;
        }
    }
    if(session->assembler.Has_Error()){
        this->Close_Session(session, "malformed packet header");
        return;
    }
    if(this->game_jobs.Size() >= this->job_high_watermark){
        this->Pause_Recv(session);
    }
}

void EpollWorldServer::Handle_Writable(const std::shared_ptr<ClientSession>& session){
    if(this->Send_Queued(session) == SendState::Failed){
        this->Close_Session(session, "send error");
    }
}

EpollWorldServer::SendState EpollWorldServer::Send_Queued(const std::shared_ptr<ClientSession>& session){
    SendState state = SendState::Completed;
    {
        std::lock_guard<std::mutex> lock(session->send_mutex);
        // 큐의 버퍼들을 복사하지 않고 iovec 배열로 가리켜 한 번에 보낸다(v8의 WSABUF scatter-gather와 같다).
        std::array<iovec, kMaxIovPerSend> iov{};
        while(!session->send_queue.empty()){
            std::size_t iov_count = 0;
            std::size_t total = 0;
            std::size_t offset = session->send_offset;
            for(const SendBuffer& data : session->send_queue){
                if(iov_count == kMaxIovPerSend || total >= kMaxBytesPerSend){
                    break;
                }
                iov[iov_count].iov_base = const_cast<char*>(data->data() + offset);
                iov[iov_count].iov_len = data->size() - offset;
                total += data->size() - offset;
                offset = 0;
                ++iov_count;
            }
            msghdr message{};
            message.msg_iov = iov.data();
            message.msg_iovlen = iov_count;
            // MSG_NOSIGNAL: 상대가 이미 닫은 연결에 쓰면 SIGPIPE로 프로세스가 죽는 대신 EPIPE를 받는다.
            const ssize_t sent = sendmsg(session->fd, &message, MSG_NOSIGNAL);
            if(sent < 0){
                if(errno == EINTR){
                    continue;
                }
                state = (errno == EAGAIN || errno == EWOULDBLOCK) ? SendState::Partial : SendState::Failed;
                break;
            }
            ++this->writev_count;
            // 보낸 바이트만큼 큐를 앞으로 당긴다(v8 Handle_Send_Completion과 같은 계산).
            std::size_t remaining = static_cast<std::size_t>(sent);
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
            if(static_cast<std::size_t>(sent) < total){
                // 커널 송신 버퍼가 찼다. 다시 불러도 EAGAIN이므로 EPOLLOUT을 기다린다.
                state = SendState::Partial;
                break;
            }
        }
    }
    if(state == SendState::Failed){
        return state;
    }
    // 남았으면 EPOLLOUT을 켜고, 다 보냈으면 끈다. 계속 켜 두면 LT라 쓸 것이 없어도 매번 깨어난다.
    const bool want_write = state == SendState::Partial;
    if(want_write){
        ++this->partial_send_count;
    }
    if(session->watching_write != want_write){
        session->watching_write = want_write;
        if(!this->Update_Interest(*session)){
            return SendState::Failed;
        }
    }
    return state;
}

void EpollWorldServer::Process_Requests(){
    std::vector<std::shared_ptr<ClientSession>> flushes;
    std::vector<std::shared_ptr<ClientSession>> closes;
    {
        std::lock_guard<std::mutex> lock(this->request_mutex);
        flushes.swap(this->flush_requests);
        closes.swap(this->close_requests);
    }
    for(const auto& session : flushes){
        {
            // 보내기 전에 표시를 내린다. 그 사이 틱이 더 쌓으면 다시 요청하므로 데이터가 남지 않는다.
            std::lock_guard<std::mutex> lock(session->send_mutex);
            session->flush_requested = false;
        }
        // 이미 EPOLLOUT을 기다리는 중이면 커널 버퍼가 비었을 때 그쪽에서 보낸다.
        if(session->closing.load() || session->watching_write){
            continue;
        }
        if(this->Send_Queued(session) == SendState::Failed){
            this->Close_Session(session, "send error");
        }
    }
    for(const auto& session : closes){
        this->Close_Session(session, "send queue overflow");
    }
}

bool EpollWorldServer::Update_Interest(const ClientSession& session){
    epoll_event event{};
    event.events = (session.recv_paused ? 0u : static_cast<std::uint32_t>(EPOLLIN)) |
        (session.watching_write ? static_cast<std::uint32_t>(EPOLLOUT) : 0u);
    event.data.u64 = session.session_id;
    if(epoll_ctl(this->epoll_fd, EPOLL_CTL_MOD, session.fd, &event) < 0){
        this->Logging(LogLevel::Error, "[EpollWorldServer] epoll_ctl(MOD) failed: " + Errno_Text(errno));
        return false;
    }
    return true;
}

void EpollWorldServer::Pause_Recv(const std::shared_ptr<ClientSession>& session){
    // EPOLLIN을 빼면 이 fd를 읽지 않으므로 커널 수신 버퍼가 차고, TCP 흐름 제어로 클라이언트 송신이 느려진다.
    if(session->recv_paused){
        return;
    }
    session->recv_paused = true;
    if(!this->Update_Interest(*session)){
        this->Close_Session(session, "epoll mod failed");
        return;
    }
    this->paused_sessions.push_back(session);
    this->paused_count.store(this->paused_sessions.size());
    ++this->recv_pause_count;
    this->Logging(LogLevel::Debug,
        "[EpollWorldServer] job queue above high watermark, recv paused for session=" + std::to_string(session->session_id));
}

void EpollWorldServer::Resume_Paused_Sessions(){
    if(this->paused_sessions.empty() || this->game_jobs.Size() > this->job_low_watermark){
        return;
    }
    std::vector<std::shared_ptr<ClientSession>> to_resume;
    to_resume.swap(this->paused_sessions);
    this->paused_count.store(0);
    for(const auto& session : to_resume){
        if(session->closing.load()){
            continue;
        }
        // 멈춘 동안 커널 버퍼에 쌓인 데이터는 EPOLLIN을 다시 켜면 LT라 바로 알려 준다.
        session->recv_paused = false;
        if(!this->Update_Interest(*session)){
            this->Close_Session(session, "epoll mod failed");
        }
    }
}

void EpollWorldServer::Close_Session(const std::shared_ptr<ClientSession>& session, const char* reason){
    // reactor만 부른다. 같은 세션을 두 번 닫지 않는다.
    if(!session || session->closing.exchange(true)){
        return;
    }
    this->sessions.erase(session->session_id);
    --this->session_count;
    // close 전에 epoll에서 뺀다. close만 해도 빠지지만 fd가 다른 곳에 복제돼 있으면 남을 수 있어 명시한다.
    epoll_ctl(this->epoll_fd, EPOLL_CTL_DEL, session->fd, nullptr);
    close(session->fd);
    {
        std::lock_guard<std::mutex> lock(session->send_mutex);
        session->send_queue.clear();
        session->send_queued_bytes = 0;
        session->send_offset = 0;
    }
    this->Logging(LogLevel::Debug, "[EpollWorldServer] client disconnected, session=" +
        std::to_string(session->session_id) + ", reason=" + reason +
        ", active_sessions=" + std::to_string(this->sessions.size()));
    // closing을 세운 뒤에 Leave를 넣는다. Handle_Enter가 closing을 보므로 닫힌 세션이 월드에 남지 않는다.
    this->Submit_Leave(session);
}

void EpollWorldServer::Submit_Leave(const std::shared_ptr<ClientSession>& session){
    if(this->server_state.load() != ServerState::Running){
        return;   // 종료 중에는 틱 루프가 곧 끝나므로 알릴 필요가 없다
    }
    GameJob leave;
    leave.type = GameJob::JobType::Leave;
    leave.session = session;
    if(!this->game_jobs.Push(std::move(leave))){
        this->pending_leaves.push_back(session);
    }
}

void EpollWorldServer::Retry_Pending_Leaves(){
    if(this->pending_leaves.empty()){
        return;
    }
    std::vector<std::shared_ptr<ClientSession>> retry;
    retry.swap(this->pending_leaves);
    for(const auto& session : retry){
        this->Submit_Leave(session);
    }
}

void EpollWorldServer::Clean_Up(){
    this->Logging(LogLevel::Info, "[EpollWorldServer] cleanup started");
    // 1) 틱 루프를 먼저 멈춘다. 살아 있으면 정리 도중에도 송신 큐에 계속 쌓는다.
    this->game_jobs.Close();
    if(this->game_thread.joinable()){
        this->game_thread.join();
    }
    // 2) 남은 송신을 한 번 더 보내 본다(best-effort, v5와 같다). 그 뒤 모든 fd를 닫는다.
    std::size_t flushed = 0;
    for(auto& [id, session] : this->sessions){
        if(this->Send_Queued(session) == SendState::Completed){
            ++flushed;
        }
        session->closing.store(true);
        close(session->fd);
    }
    const std::size_t closed = this->sessions.size();
    this->sessions.clear();
    this->paused_sessions.clear();
    this->pending_leaves.clear();
    this->session_count.store(0);
    {
        std::lock_guard<std::mutex> lock(this->request_mutex);
        this->flush_requests.clear();
        this->close_requests.clear();
    }
    // wake_fd는 소멸자에서 닫는다(이 시점에도 main 스레드가 Stop()에서 쓸 수 있다).
    for(int* fd : {&this->listen_fd, &this->epoll_fd}){
        if(*fd >= 0){
            close(*fd);
            *fd = -1;
        }
    }
    this->server_state.store(ServerState::Stopped);
    this->Logging(LogLevel::Info, "[EpollWorldServer] server stopped, closed_sessions=" + std::to_string(closed) +
        ", flushed=" + std::to_string(flushed) +
        ", jobs_processed=" + std::to_string(this->processed_job_count.load()) +
        ", jobs_discarded=" + std::to_string(this->discarded_job_count.load()) +
        ", ticks=" + std::to_string(this->tick_count.load()) +
        ", late_ticks=" + std::to_string(this->late_tick_count.load()) +
        ", max_tick_us=" + std::to_string(this->total_tick_us_max.load()));
}

// ---------------------------------------------------------------- 틱 스레드 → reactor

void EpollWorldServer::Wake_Reactor(){
    // eventfd는 8바이트 카운터다. 여러 번 써도 reactor는 한 번 깨어나 모아 처리한다.
    const std::uint64_t one = 1;
    if(this->wake_fd >= 0){
        [[maybe_unused]] const ssize_t written = write(this->wake_fd, &one, sizeof(one));
    }
}

void EpollWorldServer::Enqueue_Send(const std::shared_ptr<ClientSession>& session, SendBuffer data){
    if(!data || data->empty()){
        return;
    }
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(session->send_mutex);
        // 닫혔거나 이미 닫기를 요청한 세션에는 더 쌓지 않는다.
        if(session->closing.load() || session->close_requested.load()){
            return;
        }
        if(session->send_queued_bytes + data->size() > kMaxSendQueueBytes){
            overflow = true;
        }else{
            session->send_queued_bytes += data->size();
            session->send_queue.push_back(std::move(data));
            // reactor에 이미 알린 세션이면 다시 알리지 않는다. 한 틱 끝에 목록을 한 번에 넘긴다.
            if(!session->flush_requested){
                session->flush_requested = true;
                this->tick_flushes.push_back(session);
            }
        }
    }
    if(overflow){
        // 응답을 읽지 않는 느린 클라이언트. 틱 스레드는 fd를 닫을 수 없으므로 reactor에 닫기를 부탁한다.
        ++this->send_overflow_count;
        this->Logging(LogLevel::Error, "[EpollWorldServer] send queue overflow, closing slow client session=" +
            std::to_string(session->session_id));
        this->Request_Close(session);
    }
}

void EpollWorldServer::Request_Close(const std::shared_ptr<ClientSession>& session){
    // 한 번만 요청한다. reactor가 실제로 닫기 전까지 틱이 여러 번 돌아도 요청과 로그가 쌓이지 않는다.
    if(session->close_requested.exchange(true)){
        return;
    }
    {
        std::lock_guard<std::mutex> lock(this->request_mutex);
        this->close_requests.push_back(session);
    }
    this->Wake_Reactor();
}

// ---------------------------------------------------------------- 틱 스레드 (v8과 같은 흐름)

void EpollWorldServer::Game_Tick_Loop(){
    this->Logging(LogLevel::Info, "[EpollWorldServer] tick loop started, period_ms=" + std::to_string(this->tick_period.count()));
    pthread_setname_np(pthread_self(), "v9-tick");
    TickScheduler scheduler(this->tick_period, std::chrono::steady_clock::now());
    const float period_seconds = std::chrono::duration<float>(this->tick_period).count();
    std::deque<GameJob> jobs;

    while(this->game_jobs.Drain_Until(jobs, scheduler.Next_Deadline())){
        const auto tick_start = std::chrono::steady_clock::now();
        const std::uint64_t periods = scheduler.Begin_Tick(tick_start);
        this->current_tick = scheduler.Tick_Count();

        if(this->server_state.load() != ServerState::Running){
            this->discarded_job_count += jobs.size();
            jobs.clear();
            continue;
        }

        // 1) 입력 처리. 지난 틱에서 미룬 job을 먼저(도착 순서대로) 처리하고, 입장·퇴장 상한에 닿으면 남은 것은 다시 미룬다.
        this->enter_leave_this_tick = 0;
        std::size_t processed = 0;
        while(!this->deferred_jobs.empty()){
            GameJob& job = this->deferred_jobs.front();
            const bool membership = job.type != GameJob::JobType::Packet;
            if(membership && this->enter_leave_this_tick >= this->max_enter_leave_per_tick){
                break;
            }
            this->Process_Job(job);
            this->enter_leave_this_tick += membership ? 1 : 0;
            this->deferred_jobs.pop_front();
            ++processed;
        }
        for(GameJob& job : jobs){
            if(!this->Process_Or_Defer(job)){
                ++processed;
            }
        }
        this->deferred_job_count.store(this->deferred_jobs.size());
        // 처리한 job만 센다(미룬 job은 처리하는 틱에서 센다).
        this->processed_job_count += processed;
        jobs.clear();
        // 큐를 비웠으니 수신을 멈춘 세션이 있으면 reactor를 깨워 다시 켜게 한다(v8은 틱 스레드가 직접 Recv를 게시했다).
        if(this->paused_count.load() != 0 && this->game_jobs.Size() <= this->job_low_watermark){
            this->Wake_Reactor();
        }
        this->Expire_Idle_Inputs();
        const auto input_end = std::chrono::steady_clock::now();

        // 2) 월드 갱신
        this->world.Step(period_seconds * static_cast<float>(periods));
        const auto step_end = std::chrono::steady_clock::now();
        this->window_phase_us[0] += Elapsed_Us(tick_start, input_end);
        this->window_phase_us[1] += Elapsed_Us(input_end, step_end);

        // 3) 상태 전송
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
    this->tick_flushes.clear();
    this->deferred_jobs.clear();
    this->Logging(LogLevel::Info, "[EpollWorldServer] tick loop stopped");
}

bool EpollWorldServer::Process_Or_Defer(GameJob& job){
    const bool membership = job.type != GameJob::JobType::Packet;
    // 이미 미룬 job이 있으면 순서를 지키기 위해 그 뒤에 붙인다(같은 세션의 입장 → 패킷 → 퇴장 순서가 바뀌지 않게).
    if(!this->deferred_jobs.empty() || (membership && this->enter_leave_this_tick >= this->max_enter_leave_per_tick)){
        this->deferred_jobs.push_back(std::move(job));
        ++this->deferred_job_total;
        return true;
    }
    this->Process_Job(job);
    this->enter_leave_this_tick += membership ? 1 : 0;
    return false;
}

void EpollWorldServer::Process_Job(GameJob& job){
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

void EpollWorldServer::Record_Tick_Time(std::uint64_t elapsed_us){
    ++this->window_tick_count;
    this->window_tick_us_sum += elapsed_us;
    const auto update_max = [elapsed_us](std::atomic<std::uint64_t>& target){
        std::uint64_t current = target.load();
        while(elapsed_us > current && !target.compare_exchange_weak(current, elapsed_us)){}
    };
    update_max(this->window_tick_us_max);
    update_max(this->total_tick_us_max);
}

void EpollWorldServer::Handle_Enter(const std::shared_ptr<ClientSession>& session){
    if(session->closing.load()){
        return;
    }
    std::optional<protocol::EntityState> state;
    if(this->spawn_area){
        // 원 안의 임의 위치(면적에 고르게 퍼지도록 반지름은 제곱근으로 뽑는다)
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const float angle = unit(this->spawn_rng) * 6.2831853f;
        const float distance = this->spawn_area->radius * std::sqrt(unit(this->spawn_rng));
        state = this->world.Enter_At(session->session_id,
            this->spawn_area->x + distance * std::cos(angle), this->spawn_area->y + distance * std::sin(angle));
    }else{
        state = this->world.Enter(session->session_id);
    }
    if(!state){
        this->Request_Close(session);
        return;
    }
    this->players.emplace(session->session_id, session);
    const World::Config& config = this->world.Get_Config();
    protocol::Append_Enter_World(this->outbox[session->session_id],
        protocol::EnterWorldInfo{session->session_id, state->x, state->y, config.world_size, config.cell_size});
    this->Logging(LogLevel::Debug, "[Game] player " + std::to_string(session->session_id) + " entered, players=" +
        std::to_string(this->world.Player_Count()));
}

void EpollWorldServer::Handle_Leave(const std::shared_ptr<ClientSession>& session){
    if(this->players.erase(session->session_id) == 0){
        return;
    }
    this->world.Leave(session->session_id);
    this->outbox.erase(session->session_id);
    this->Logging(LogLevel::Debug, "[Game] player " + std::to_string(session->session_id) + " left, players=" +
        std::to_string(this->world.Player_Count()));
}

void EpollWorldServer::Handle_Packet(const std::shared_ptr<ClientSession>& session, const protocol::Packet& packet){
    // 닫기를 요청한 세션(잘못된 패킷, 송신 큐 초과)의 남은 패킷은 처리하지 않는다.
    if(!this->players.contains(session->session_id) || session->closing.load() || session->close_requested.load()){
        return;
    }
    switch(static_cast<protocol::PacketId>(packet.id)){
        case protocol::PacketId::C_Move:{
            const auto input = protocol::Parse_Move(packet.body);
            if(!input){
                this->Logging(LogLevel::Error, "[Game] malformed C_Move, closing session=" + std::to_string(session->session_id));
                this->Request_Close(session);
                return;
            }
            ++this->move_input_count;
            this->world.Set_Velocity(session->session_id, input->velocity_x, input->velocity_y, input->input_seq);
            session->last_move_tick = this->current_tick;
            session->last_input_seq = input->input_seq;
            session->moving = input->velocity_x != 0.0f || input->velocity_y != 0.0f;
            break;
        }
        case protocol::PacketId::C_Chat:{
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
            const std::vector<char> chat = protocol::Make_Chat(session->session_id, text);
            auto append_to = [this, &chat](std::uint32_t receiver){
                auto& out = this->outbox[receiver];
                out.insert(out.end(), chat.begin(), chat.end());
            };
            append_to(session->session_id);
            for(std::uint32_t receiver : this->world.Visible_Ids(session->session_id)){
                append_to(receiver);
            }
            break;
        }
        default:
            this->Logging(LogLevel::Error, "[Game] unknown packet id=" + std::to_string(packet.id) +
                ", closing session=" + std::to_string(session->session_id));
            this->Request_Close(session);
            break;
    }
}

void EpollWorldServer::Expire_Idle_Inputs(){
    if(this->input_timeout_ticks == 0){
        return;
    }
    for(auto& [id, session] : this->players){
        if(!session->moving || this->current_tick - session->last_move_tick <= this->input_timeout_ticks){
            continue;
        }
        this->world.Set_Velocity(id, 0.0f, 0.0f, session->last_input_seq);
        session->moving = false;
        ++this->expired_input_count;
    }
}

void EpollWorldServer::Flush_Tick_Output(){
    const auto build_start = std::chrono::steady_clock::now();
    const std::span<const ObserverUpdate> updates = this->world.Collect_Updates();
    const auto collect_end = std::chrono::steady_clock::now();
    const float world_size = this->world.Get_Config().world_size;
    for(const ObserverUpdate& update : updates){
        auto& out = this->outbox[update.observer_id];
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
        this->Enqueue_Send(found->second, std::make_shared<const std::vector<char>>(std::move(out)));
    }
    this->outbox.clear();
    // 이번 틱에 보낼 것이 생긴 세션을 한 번에 넘기고 reactor를 한 번만 깨운다.
    if(!this->tick_flushes.empty()){
        {
            std::lock_guard<std::mutex> lock(this->request_mutex);
            this->flush_requests.insert(this->flush_requests.end(),
                std::make_move_iterator(this->tick_flushes.begin()), std::make_move_iterator(this->tick_flushes.end()));
        }
        this->tick_flushes.clear();
        this->Wake_Reactor();
    }
    const auto send_end = std::chrono::steady_clock::now();
    this->window_phase_us[2] += Elapsed_Us(build_start, collect_end);
    this->window_phase_us[3] += Elapsed_Us(collect_end, send_start);
    this->window_phase_us[4] += Elapsed_Us(send_start, send_end);
    this->window_sent_bytes += tick_bytes;
    this->player_count.store(this->world.Player_Count());
    this->max_cell_population.store(this->world.Max_Cell_Population());
}

void EpollWorldServer::Log_Status(){
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
        "[Status] sessions=" + std::to_string(this->session_count.load()) +
        ", players=" + std::to_string(this->player_count.load()) +
        ", jobs_deferred_now=" + std::to_string(this->deferred_job_count.load()) +
        ", jobs_deferred=" + std::to_string(this->deferred_job_total.load()) +
        ", ticks=" + std::to_string(this->tick_count.load()) +
        ", " + tick_text +
        ", " + phase_text +
        ", late_ticks=" + std::to_string(this->late_tick_count.load()) +
        ", skipped_ticks=" + std::to_string(this->skipped_tick_count.load()) +
        ", max_cell_population=" + std::to_string(this->max_cell_population.load()) +
        ", job_queue_peak=" + std::to_string(this->game_jobs.Peak_Size()) +
        ", job_queue_window_peak=" + std::to_string(this->game_jobs.Take_Window_Peak()) +
        ", jobs_pushed=" + std::to_string(this->game_jobs.Pushed_Count()) +
        ", job_push_calls=" + std::to_string(this->game_jobs.Push_Call_Count()) +
        ", jobs_processed=" + std::to_string(this->processed_job_count.load()) +
        ", jobs_rejected=" + std::to_string(this->rejected_job_count.load()) +
        ", recv_paused_now=" + std::to_string(this->paused_count.load()) +
        ", recv_pauses=" + std::to_string(this->recv_pause_count.load()) +
        ", wakes=" + std::to_string(this->wake_count.load()) +
        ", sendmsg_calls=" + std::to_string(this->writev_count.load()) +
        ", epoll_waits=" + std::to_string(this->epoll_wait_count.load()) +
        ", recv_calls=" + std::to_string(this->recv_call_count.load()) +
        ", partial_sends=" + std::to_string(this->partial_send_count.load()) +
        ", send_overflows=" + std::to_string(this->send_overflow_count.load()) +
        ", moves=" + std::to_string(this->move_input_count.load()) +
        ", inputs_expired=" + std::to_string(this->expired_input_count.load()) +
        ", chats=" + std::to_string(this->chat_count.load()) +
        ", chats_dropped=" + std::to_string(this->dropped_chat_count.load()) +
        ", log_dropped=" + std::to_string(this->logger.Dropped_Count()));
}
