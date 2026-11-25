#include <uring_world_server.h>

#include <tick_scheduler.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

namespace{

constexpr int kBacklog = 512;
// 고정 버퍼 수신 방식의 세션별 버퍼(v8과 같은 최대 패킷 크기)
constexpr std::size_t kFixedRecvBufferSize = 8 * 1024;
// sendmsg 한 번에 묶는 버퍼 수와 바이트(v8·v9와 같은 값)
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
constexpr int kDefaultMaxEnterLeavePerTick = 64;
// 제출 큐 크기. 완료 큐는 이 4배로 잡는다(Create_Ring).
constexpr int kDefaultRingEntries = 4096;
// provided buffer ring: 묶음 크기는 2의 거듭제곱이어야 한다. 1,024 x 4 KiB = 4 MiB를 모든 연결이 함께 쓴다.
// 보완(2026-11-25): 처음에는 1,024 x 4 KiB였다. 연결 2,000개가 수신을 멈췄다 한꺼번에 재개하면, 완료를 처리해 버퍼를 돌려주기
// 전에 커널이 연결 수만큼 버퍼를 원해 바닥났다(-ENOBUFS). 이 프로토콜의 패킷은 대부분 16바이트(이동), 채팅도 256바이트 이하라
// 같은 4 MiB를 4,096 x 1 KiB로 쪼갰다. 한 번에 1 KiB만 받아도 나머지는 다음 완료로 이어서 오고 조립기가 붙인다.
constexpr int kDefaultProvidedBufferCount = 4096;
constexpr int kDefaultProvidedBufferSize = 1024;
constexpr const char* kTickPhaseNames[] = {"input", "step", "collect", "encode", "send"};

std::uint64_t Elapsed_Us(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to){
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
}

std::string Errno_Text(int error){
    return std::to_string(error) + " (" + std::strerror(error) + ")";
}

unsigned Round_Up_Power_Of_Two(unsigned value){
    unsigned result = 1;
    while(result < value) result <<= 1;
    return result;
}

}

ClientSession::ClientSession(int fd_value, std::uint32_t id) : fd(fd_value), session_id(id){}

UringWorldServer::UringWorldServer(int port)
    : server_port(port), wake_fd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      recv_mode(Read_Recv_Mode_From_Env()),
      ring_entries(static_cast<unsigned>(Read_Number_From_Env("V10_RING_ENTRIES", kDefaultRingEntries, 64, 32768))),
      provided_buffer_count(Round_Up_Power_Of_Two(static_cast<unsigned>(
          Read_Number_From_Env("V10_BUF_COUNT", kDefaultProvidedBufferCount, 16, 32768)))),
      provided_buffer_size(static_cast<unsigned>(Read_Number_From_Env("V10_BUF_SIZE", kDefaultProvidedBufferSize, 256, 65536))),
      game_jobs(kGameJobCapacity),
      tick_period(static_cast<long long>(Read_Number_From_Env("V10_TICK_MS", kDefaultTickMs, 1, 1000))),
      status_interval(static_cast<long long>(
          Read_Number_From_Env("V10_STATUS_INTERVAL_MS", kDefaultStatusIntervalMs, 100, 3600000))),
      world(Read_World_Config_From_Env()),
      chats_per_tick(static_cast<std::uint32_t>(Read_Number_From_Env("V10_CHATS_PER_TICK", kDefaultChatsPerTick, 1, 1000000))),
      input_timeout_ticks(Input_Timeout_Ticks(
          Read_Number_From_Env("V10_INPUT_TIMEOUT_MS", kDefaultInputTimeoutMs, 0, 600000), tick_period.count())),
      spawn_area(Read_Spawn_Area_From_Env()),
      max_enter_leave_per_tick(static_cast<std::size_t>(Read_Number_From_Env("V10_MAX_ENTER_LEAVE_PER_TICK",
          kDefaultMaxEnterLeavePerTick, 1, 1000000))),
      job_high_watermark(static_cast<std::size_t>(Read_Number_From_Env("V10_JOB_HIGH_WATERMARK",
          static_cast<double>(kDefaultJobHighWatermark), 2, static_cast<double>(kGameJobCapacity - 1)))),
      job_low_watermark((std::min)(static_cast<std::size_t>(Read_Number_From_Env("V10_JOB_LOW_WATERMARK",
          static_cast<double>(kDefaultJobLowWatermark), 1, static_cast<double>(kGameJobCapacity - 1))),
          job_high_watermark / 2)),
      logger(std::cout),
      minimum_log_level(Read_Log_Level_From_Env()){
    this->use_sqpoll = Read_Number_From_Env("V10_SQPOLL", 0, 0, 1) != 0;
    this->use_fixed_files = Read_Number_From_Env("V10_FIXED_FILES", 0, 0, 1) != 0;
    this->logger.Start();
}

UringWorldServer::~UringWorldServer(){
    // wake_fd는 Stop()을 부를 수 있는 모든 스레드가 끝난 뒤(소멸자)에만 닫는다(v9와 같다).
    if(this->wake_fd >= 0){
        close(this->wake_fd);
    }
    this->logger.Stop();
}

UringWorldServer::RecvMode UringWorldServer::Read_Recv_Mode_From_Env(){
    const char* raw = std::getenv("V10_RECV_MODE");
    if(raw != nullptr && std::string_view(raw) == "fixed"){
        return RecvMode::Fixed;
    }
    return RecvMode::Provided;
}

// ---------------------------------------------------------------- 설정

UringWorldServer::LogLevel UringWorldServer::Read_Log_Level_From_Env(){
    const char* raw = std::getenv("V10_LOG_LEVEL");
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

double UringWorldServer::Read_Number_From_Env(const char* name, double default_value, double min_value, double max_value){
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

World::Config UringWorldServer::Read_World_Config_From_Env(){
    World::Config config;
    config.world_size = static_cast<float>(Read_Number_From_Env("V10_WORLD_SIZE", config.world_size, 10, 100000));
    // V9_CELL_SIZE를 월드 크기 이상으로 주면 셀이 하나가 되어 AOI를 끈 것(전체 방송)과 같다.
    config.cell_size = static_cast<float>(Read_Number_From_Env("V10_CELL_SIZE", config.cell_size, 1, 100000));
    config.max_speed = static_cast<float>(Read_Number_From_Env("V10_MAX_SPEED", config.max_speed, 0, 100000));
    config.seed = static_cast<std::uint32_t>(Read_Number_From_Env("V10_SEED", config.seed, 0, 4294967295.0));
    return config;
}

std::optional<UringWorldServer::SpawnArea> UringWorldServer::Read_Spawn_Area_From_Env(){
    const double x = Read_Number_From_Env("V10_SPAWN_X", -1, 0, 100000);
    const double y = Read_Number_From_Env("V10_SPAWN_Y", -1, 0, 100000);
    const double radius = Read_Number_From_Env("V10_SPAWN_RADIUS", -1, 0, 100000);
    if(x < 0 || y < 0 || radius < 0){
        return std::nullopt;
    }
    return SpawnArea{static_cast<float>(x), static_cast<float>(y), static_cast<float>(radius)};
}

std::uint64_t UringWorldServer::Input_Timeout_Ticks(double timeout_ms, long long tick_ms){
    if(!(timeout_ms > 0.0) || tick_ms <= 0){
        return 0;
    }
    const auto ticks = static_cast<std::uint64_t>(std::ceil(timeout_ms / static_cast<double>(tick_ms)));
    return (std::max)(std::uint64_t{1}, ticks);
}

void UringWorldServer::Logging(LogLevel level, const std::string& message){
    if(level < this->minimum_log_level){
        return;
    }
    this->logger.Write(message);
}

// ---------------------------------------------------------------- ring 준비

bool UringWorldServer::Create_Listen_Socket(){
    // io_uring이 accept를 대신 기다리므로 listen 소켓은 blocking이어도 된다(epoll처럼 EAGAIN을 다룰 필요가 없다).
    this->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(this->listen_fd < 0){
        this->Logging(LogLevel::Error, "[UringWorldServer] socket failed: " + Errno_Text(errno));
        return false;
    }
    int reuse = 1;
    setsockopt(this->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(this->server_port));
    if(bind(this->listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0){
        this->Logging(LogLevel::Error, "[UringWorldServer] bind failed: " + Errno_Text(errno));
        return false;
    }
    if(listen(this->listen_fd, kBacklog) < 0){
        this->Logging(LogLevel::Error, "[UringWorldServer] listen failed: " + Errno_Text(errno));
        return false;
    }
    return true;
}

bool UringWorldServer::Create_Ring(){
    if(this->wake_fd < 0){
        this->Logging(LogLevel::Error, "[UringWorldServer] eventfd failed");
        return false;
    }
    // SINGLE_ISSUER: 이 ring에는 ring 스레드만 제출한다고 커널에 알린다. DEFER_TASKRUN: 완료 처리를 다른 시점에 끼어들지 않고
    // 우리가 기다릴 때(io_uring_enter) 몰아서 한다. CQSIZE: 완료 큐를 제출 큐보다 크게 잡아 완료가 몰려도 넘치지 않게 한다.
    // ring은 이 스레드에서 만든다(SINGLE_ISSUER는 만든 스레드를 제출자로 본다).
    io_uring_params params{};
    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CQSIZE;
    if(this->use_sqpoll){
        // SQPOLL: 커널 스레드(iou-sqp)가 제출 큐를 계속 읽어, 제출에 시스템 호출이 필요 없다. 그 대신 그 스레드가 코어를 쓴다.
        // 일정 시간(sq_thread_idle) 할 일이 없으면 잠들고, 그때는 liburing이 깨우는 시스템 호출을 대신 부른다.
        // DEFER_TASKRUN과는 같이 쓸 수 없다(완료 처리를 제출 스레드가 몰아서 하는 방식이라).
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = static_cast<unsigned>(Read_Number_From_Env("V10_SQPOLL_IDLE_MS", 1000, 1, 60000));
    }else if(Read_Number_From_Env("V10_DEFER_TASKRUN", 1, 0, 1) != 0){
        params.flags |= IORING_SETUP_DEFER_TASKRUN;
    }
    params.cq_entries = this->ring_entries * 4;
    int result = io_uring_queue_init_params(this->ring_entries, &this->ring, &params);
    if(result < 0){
        // 오래된 커널: 최적화 플래그 없이 다시 만든다.
        this->Logging(LogLevel::Info, "[UringWorldServer] SINGLE_ISSUER/DEFER_TASKRUN not available (" +
            Errno_Text(-result) + "), retrying without them");
        params = io_uring_params{};
        params.flags = IORING_SETUP_CQSIZE;
        params.cq_entries = this->ring_entries * 4;
        result = io_uring_queue_init_params(this->ring_entries, &this->ring, &params);
        this->use_sqpoll = false;
    }
    if(result < 0){
        this->Logging(LogLevel::Error, "[UringWorldServer] io_uring_queue_init failed: " + Errno_Text(-result));
        return false;
    }
    this->ring_ready = true;
    this->Logging(LogLevel::Info, "[UringWorldServer] ring created, sq=" + std::to_string(params.sq_entries) +
        ", cq=" + std::to_string(params.cq_entries) +
        ", flags=" + std::to_string(params.flags) +
        ", nodrop=" + std::string((params.features & IORING_FEAT_NODROP) ? "yes" : "no"));
    if(this->recv_mode == RecvMode::Provided && !this->Setup_Provided_Buffers()){
        return false;
    }
    if(this->use_fixed_files){
        // 고정 파일: 빈 파일 표를 ring에 등록해 두고, 접속마다 fd를 빈 칸에 넣는다. 이후 recv·send·취소는 fd 대신 칸 번호와
        // IOSQE_FIXED_FILE로 제출해, 작업마다 커널이 fd를 찾아 참조를 세는 비용을 줄인다. 표 크기는 열 수 있는 fd 수를 넘을 수 없다.
        rlimit limit{};
        getrlimit(RLIMIT_NOFILE, &limit);
        const unsigned table = static_cast<unsigned>((std::min)(static_cast<rlim_t>(65536), limit.rlim_cur));
        const int registered = io_uring_register_files_sparse(&this->ring, table);
        if(registered < 0){
            this->Logging(LogLevel::Error, "[UringWorldServer] register_files_sparse failed: " + Errno_Text(-registered) +
                ", fixed files disabled");
            this->use_fixed_files = false;
        }else{
            this->fixed_file_capacity = table;
            this->Logging(LogLevel::Info, "[UringWorldServer] fixed file table: " + std::to_string(table) + " slots");
        }
    }
    this->Logging(LogLevel::Info, std::string("[UringWorldServer] options: sqpoll=") + (this->use_sqpoll ? "on" : "off") +
        ", fixed_files=" + (this->use_fixed_files ? "on" : "off"));
    return true;
}

int UringWorldServer::Acquire_File_Slot(int fd){
    if(!this->use_fixed_files){
        return -1;
    }
    int slot = -1;
    if(!this->free_file_slots.empty()){
        slot = this->free_file_slots.back();
        this->free_file_slots.pop_back();
    }else if(this->next_file_slot < this->fixed_file_capacity){
        slot = static_cast<int>(this->next_file_slot++);
    }else{
        return -1;   // 표가 가득 찼다: 이 세션은 일반 fd로 쓴다
    }
    if(io_uring_register_files_update(&this->ring, static_cast<unsigned>(slot), &fd, 1) != 1){
        this->free_file_slots.push_back(slot);
        return -1;
    }
    return slot;
}

void UringWorldServer::Release_File_Slot(int slot){
    if(slot < 0){
        return;
    }
    int empty = -1;
    io_uring_register_files_update(&this->ring, static_cast<unsigned>(slot), &empty, 1);
    this->free_file_slots.push_back(slot);
}

void UringWorldServer::Set_Target(io_uring_sqe* sqe, const ClientSession& session){
    // prep 함수가 넣은 fd 자리를 고정 파일 칸 번호로 바꾼다.
    if(session.file_slot >= 0){
        sqe->fd = session.file_slot;
        sqe->flags |= IOSQE_FIXED_FILE;
    }
}

bool UringWorldServer::Setup_Provided_Buffers(){
    // 공용 수신 버퍼 묶음을 커널에 등록한다. multishot recv는 데이터가 올 때 여기서 버퍼 하나를 골라 채우고, CQE에 버퍼 번호를 담아 준다.
    // 다 쓴 버퍼는 우리가 다시 넣어 줘야(Recycle_Buffer) 커널이 또 쓸 수 있다. 연결 수와 상관없이 메모리가 묶음 크기로 정해진다.
    const std::size_t total = static_cast<std::size_t>(this->provided_buffer_count) * this->provided_buffer_size;
    this->provided_buffers.assign(total, 0);
    int error = 0;
    this->buffer_ring = io_uring_setup_buf_ring(&this->ring, this->provided_buffer_count, kBufferGroup, 0, &error);
    if(this->buffer_ring == nullptr){
        this->Logging(LogLevel::Error, "[UringWorldServer] io_uring_setup_buf_ring failed: " + Errno_Text(-error));
        return false;
    }
    const int mask = io_uring_buf_ring_mask(this->provided_buffer_count);
    for(unsigned i = 0; i < this->provided_buffer_count; ++i){
        io_uring_buf_ring_add(this->buffer_ring, this->provided_buffers.data() + static_cast<std::size_t>(i) * this->provided_buffer_size,
            this->provided_buffer_size, static_cast<unsigned short>(i), mask, static_cast<int>(i));
    }
    io_uring_buf_ring_advance(this->buffer_ring, static_cast<int>(this->provided_buffer_count));
    this->Logging(LogLevel::Info, "[UringWorldServer] provided buffers: " + std::to_string(this->provided_buffer_count) +
        " x " + std::to_string(this->provided_buffer_size) + " bytes");
    return true;
}

std::uint64_t UringWorldServer::Pack(Op op, std::uint32_t session_id){
    return (static_cast<std::uint64_t>(op) << 56) | session_id;
}

io_uring_sqe* UringWorldServer::Get_Sqe(){
    // 제출 큐가 가득 차면 지금까지 쌓인 것을 먼저 제출하고 다시 얻는다.
    io_uring_sqe* sqe = io_uring_get_sqe(&this->ring);
    while(sqe == nullptr){
        io_uring_submit(&this->ring);
        ++this->enter_count;
        sqe = io_uring_get_sqe(&this->ring);
    }
    ++this->sqe_count;
    return sqe;
}

void UringWorldServer::Arm_Accept(){
    // multishot accept: SQE 하나로 접속마다 CQE가 계속 온다. CQE에 IORING_CQE_F_MORE가 꺼지면 끝난 것이라 다시 건다.
    io_uring_sqe* sqe = this->Get_Sqe();
    io_uring_prep_multishot_accept(sqe, this->listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
    io_uring_sqe_set_data64(sqe, Pack(Op::Accept, 0));
    this->accept_armed = true;
    ++this->pending_ops;
}

void UringWorldServer::Arm_Wake(){
    // eventfd에 read를 걸어 둔다. 틱 스레드나 Stop()이 write하면 이 read가 끝나 ring 스레드가 깨어난다.
    io_uring_sqe* sqe = this->Get_Sqe();
    io_uring_prep_read(sqe, this->wake_fd, &this->wake_value, sizeof(this->wake_value), 0);
    io_uring_sqe_set_data64(sqe, Pack(Op::Wake, 0));
    this->wake_armed = true;
    ++this->pending_ops;
}

void UringWorldServer::Post_Recv(const std::shared_ptr<ClientSession>& session){
    if(session->closing.load() || session->recv_pending || session->recv_paused){
        return;
    }
    io_uring_sqe* sqe = this->Get_Sqe();
    if(this->recv_mode == RecvMode::Fixed){
        // IOCP의 WSARecv와 같은 모양: 세션 버퍼를 맡기고, 데이터가 오면 한 번 끝난다. 끝나면 다시 건다.
        io_uring_prep_recv(sqe, session->fd, session->recv_buffer.data(), session->recv_buffer.size(), 0);
        this->Set_Target(sqe, *session);
    }else{
        // multishot recv + 버퍼 고르기: 한 번 걸면 데이터가 올 때마다 CQE가 오고, 버퍼는 커널이 공용 묶음에서 고른다.
        io_uring_prep_recv_multishot(sqe, session->fd, nullptr, 0, 0);
        this->Set_Target(sqe, *session);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = kBufferGroup;
    }
    io_uring_sqe_set_data64(sqe, Pack(Op::Recv, session->session_id));
    session->recv_pending = true;
    ++this->pending_ops;
}

void UringWorldServer::Post_Send(const std::shared_ptr<ClientSession>& session){
    // 세션당 송신은 하나만 진행한다. 끝나면 Handle_Send가 남은 것을 이어서 제출한다.
    if(session->send_inflight || session->closing.load()){
        return;
    }
    session->send_iov.clear();
    session->send_in_flight.clear();
    std::size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(session->send_mutex);
        std::size_t offset = session->send_offset;
        // 큐의 버퍼들을 복사하지 않고 iovec으로 가리킨다. 커널이 다 쓸 때까지 send_in_flight가 버퍼를 붙잡는다(v8 in_flight와 같다).
        for(const SendBuffer& data : session->send_queue){
            if(session->send_iov.size() == kMaxIovPerSend || total >= kMaxBytesPerSend){
                break;
            }
            session->send_iov.push_back(iovec{const_cast<char*>(data->data() + offset), data->size() - offset});
            session->send_in_flight.push_back(data);
            total += data->size() - offset;
            offset = 0;
        }
    }
    if(total == 0){
        return;
    }
    session->send_inflight_bytes = total;
    // msghdr도 완료까지 살아 있어야 하므로 세션에 둔다.
    session->send_message = msghdr{};
    session->send_message.msg_iov = session->send_iov.data();
    session->send_message.msg_iovlen = session->send_iov.size();
    io_uring_sqe* sqe = this->Get_Sqe();
    // MSG_NOSIGNAL: 끊긴 연결에 써도 SIGPIPE 대신 -EPIPE 완료로 받는다(v9와 같다).
    io_uring_prep_sendmsg(sqe, session->fd, &session->send_message, MSG_NOSIGNAL);
    this->Set_Target(sqe, *session);
    io_uring_sqe_set_data64(sqe, Pack(Op::Send, session->session_id));
    session->send_inflight = true;
    ++this->pending_ops;
    ++this->send_count;
}

// ---------------------------------------------------------------- ring 루프

void UringWorldServer::Server_Run(){
    const World::Config& config = this->world.Get_Config();
    this->Logging(LogLevel::Info, "[UringWorldServer] server starting, port=" + std::to_string(this->server_port) +
        ", tick_ms=" + std::to_string(this->tick_period.count()) +
        ", world_size=" + std::to_string(static_cast<int>(config.world_size)) +
        ", cell_size=" + std::to_string(static_cast<int>(config.cell_size)) +
        ", recv_mode=" + std::string(this->recv_mode == RecvMode::Fixed ? "fixed" : "provided") +
        ", ring_entries=" + std::to_string(this->ring_entries) +
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
    pthread_setname_np(pthread_self(), "v10-ring");
    if(!this->Create_Listen_Socket() || !this->Create_Ring()){
        this->server_state.store(ServerState::Stopping);
        this->Clean_Up();
        return;
    }
    this->game_thread = std::thread(&UringWorldServer::Game_Tick_Loop, this);
    this->Arm_Accept();
    this->Arm_Wake();
    this->Logging(LogLevel::Info, "[UringWorldServer] listening, ring thread submits and reaps completions");

    this->Ring_Loop();
    this->Log_Status();
    this->Clean_Up();
}

void UringWorldServer::Stop(){
    ServerState expected = ServerState::Running;
    if(!this->server_state.compare_exchange_strong(expected, ServerState::Stopping)){
        expected = ServerState::Creating;
        this->server_state.compare_exchange_strong(expected, ServerState::Stopped);
        return;
    }
    this->Logging(LogLevel::Info, "[UringWorldServer] stop requested");
    this->Wake_Ring();
}

void UringWorldServer::Drain_Completions(){
    // 쌓인 CQE를 모두 처리한 뒤 한 번에 소비 표시한다. 처리 중에 새 SQE를 만들어도 된다(다음 제출 때 함께 나간다).
    unsigned head = 0;
    unsigned count = 0;
    io_uring_cqe* cqe = nullptr;
    io_uring_for_each_cqe(&this->ring, head, cqe){
        this->Handle_Cqe(cqe);
        ++count;
    }
    io_uring_cq_advance(&this->ring, count);
    this->cqe_count += count;
}

void UringWorldServer::Ring_Loop(){
    auto next_status = std::chrono::steady_clock::now() + this->status_interval;
    while(this->server_state.load() == ServerState::Running){
        // 쌓인 SQE를 제출하고, 완료가 하나라도 오거나 다음 상태 출력 시각이 될 때까지 기다린다. 시스템 호출 한 번이다.
        const auto now = std::chrono::steady_clock::now();
        const auto wait_us = (std::max)(1000LL, static_cast<long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(next_status - now).count()));
        __kernel_timespec timeout{};
        timeout.tv_sec = wait_us / 1000000;
        timeout.tv_nsec = (wait_us % 1000000) * 1000;
        io_uring_cqe* first = nullptr;
        const int result = io_uring_submit_and_wait_timeout(&this->ring, &first, 1, &timeout, nullptr);
        ++this->enter_count;
        if(result < 0 && result != -ETIME && result != -EINTR && result != -EBUSY){
            this->Logging(LogLevel::Error, "[UringWorldServer] io_uring_submit_and_wait failed: " + Errno_Text(-result));
            break;
        }
        this->Drain_Completions();

        this->Retry_Pending_Leaves();
        this->Resume_Paused_Sessions();
        this->pending_ops_now.store(this->pending_ops);
        this->cq_overflow.store(*this->ring.cq.koverflow);
        if(std::chrono::steady_clock::now() >= next_status){
            this->Log_Status();
            next_status += this->status_interval;
        }
    }
}

void UringWorldServer::Handle_Cqe(const io_uring_cqe* cqe){
    const std::uint64_t data = io_uring_cqe_get_data64(cqe);
    const auto op = static_cast<Op>(data >> 56);
    const auto session_id = static_cast<std::uint32_t>(data & 0xFFFFFFFFu);
    const bool more = (cqe->flags & IORING_CQE_F_MORE) != 0;
    // 마지막 CQE(F_MORE가 꺼짐)가 와야 그 작업이 끝난 것이다. multishot은 그 전에 여러 번 온다.
    if(!more){
        --this->pending_ops;
    }
    switch(op){
        case Op::Accept:
            this->Handle_Accept(cqe->res, more);
            return;
        case Op::Wake:
            this->wake_armed = false;
            ++this->wake_count;
            this->Process_Requests();
            if(this->server_state.load() == ServerState::Running){
                this->Arm_Wake();
            }
            return;
        case Op::Cancel:
            return;   // 취소 요청 자체의 완료. 취소된 작업은 자기 CQE(-ECANCELED)로 따로 돌아온다
        case Op::Recv:
        case Op::Send:{
            auto found = this->sessions.find(session_id);
            if(found == this->sessions.end()){
                // 세션은 자기 작업이 모두 끝난 뒤에만 지우므로 여기 오면 안 된다. 그래도 provided 버퍼는 돌려준다.
                if(op == Op::Recv && (cqe->flags & IORING_CQE_F_BUFFER)){
                    this->Recycle_Buffer(static_cast<std::uint16_t>(cqe->flags >> IORING_CQE_BUFFER_SHIFT));
                }
                this->Logging(LogLevel::Error, "[UringWorldServer] completion for unknown session=" + std::to_string(session_id));
                return;
            }
            const std::shared_ptr<ClientSession> session = found->second;
            if(op == Op::Recv){
                this->Handle_Recv(session, cqe->res, cqe->flags);
            }else{
                this->Handle_Send(session, cqe->res);
            }
            return;
        }
    }
}

void UringWorldServer::Handle_Accept(int result, bool more){
    if(!more){
        this->accept_armed = false;
        if(this->server_state.load() == ServerState::Running){
            this->Arm_Accept();
        }
    }
    if(result < 0){
        if(result != -ECANCELED){
            this->Logging(LogLevel::Error, "[UringWorldServer] accept failed: " + Errno_Text(-result));
        }
        return;
    }
    const int client_fd = result;
    if(this->server_state.load() != ServerState::Running){
        close(client_fd);
        return;
    }
    int no_delay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));

    auto session = std::make_shared<ClientSession>(client_fd, this->next_session_id++);
    sockaddr_in remote{};
    socklen_t remote_length = sizeof(remote);
    char ip_text[INET_ADDRSTRLEN]{};
    if(getpeername(client_fd, reinterpret_cast<sockaddr*>(&remote), &remote_length) == 0 &&
        inet_ntop(AF_INET, &remote.sin_addr, ip_text, sizeof(ip_text)) != nullptr){
        session->remote_endpoint = std::string(ip_text) + ":" + std::to_string(ntohs(remote.sin_port));
    }
    if(this->recv_mode == RecvMode::Fixed){
        session->recv_buffer.resize(kFixedRecvBufferSize);
    }
    session->file_slot = this->Acquire_File_Slot(client_fd);
    this->sessions.emplace(session->session_id, session);
    ++this->session_count;

    // Enter는 첫 recv를 걸기 전에 넣는다(v7~v9와 같은 이유).
    GameJob enter;
    enter.type = GameJob::JobType::Enter;
    enter.session = session;
    if(!this->game_jobs.Push(std::move(enter))){
        ++this->rejected_job_count;
        this->Begin_Close(session, "job queue full on accept");
        return;
    }
    this->Post_Recv(session);
    this->Logging(LogLevel::Debug, "[UringWorldServer] client connected, session=" + std::to_string(session->session_id) +
        ", fd=" + std::to_string(client_fd) + ", remote=" + session->remote_endpoint);
}

bool UringWorldServer::Consume_Received(const std::shared_ptr<ClientSession>& session, const char* data, std::size_t length){
    session->assembler.Append(data, length);
    // 보완(2026-11-25): 이미 수위를 넘었거나 이 세션이 멈춘 상태면, 받은 바이트는 조립기에 그대로 두고 job 큐에 넣지 않는다.
    // 완료 통지에서는 수위를 넘는 순간에도 연결마다 이미 받아 온 데이터가 하나씩 있어, 그대로 넣으면 넘침이 연결 수에 비례했다
    // (수위 200, 2,000명에서 큐 최대 약 1,100). v9에서 읽지 않은 데이터가 커널 소켓 버퍼에 남던 것과 같은 효과를 세션 조립기로 낸다.
    // 재개할 때(Resume_Paused_Sessions) 쌓아 둔 바이트에서 패킷을 꺼내 넣는다.
    if(session->recv_paused || this->game_jobs.Size() >= this->job_high_watermark){
        this->Pause_Recv(session);
        return false;
    }
    return this->Push_Assembled(session);
}

bool UringWorldServer::Push_Assembled(const std::shared_ptr<ClientSession>& session){
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
            this->Begin_Close(session, "game job queue hard limit");
            return false;
        }
    }
    if(session->assembler.Has_Error()){
        this->Begin_Close(session, "malformed packet header");
        return false;
    }
    if(this->game_jobs.Size() >= this->job_high_watermark){
        this->Pause_Recv(session);
        return false;
    }
    return true;
}

void UringWorldServer::Recycle_Buffer(std::uint16_t buffer_id){
    // 커널이 채워 준 공용 버퍼를 다 읽었으니 다시 묶음에 넣는다. 넣지 않으면 버퍼가 바닥나 recv가 -ENOBUFS로 끝난다.
    io_uring_buf_ring_add(this->buffer_ring,
        this->provided_buffers.data() + static_cast<std::size_t>(buffer_id) * this->provided_buffer_size,
        this->provided_buffer_size, buffer_id, io_uring_buf_ring_mask(this->provided_buffer_count), 0);
    io_uring_buf_ring_advance(this->buffer_ring, 1);
}

void UringWorldServer::Handle_Recv(const std::shared_ptr<ClientSession>& session, int result, std::uint32_t flags){
    const bool more = (flags & IORING_CQE_F_MORE) != 0;
    const bool has_buffer = (flags & IORING_CQE_F_BUFFER) != 0;
    const auto buffer_id = static_cast<std::uint16_t>(flags >> IORING_CQE_BUFFER_SHIFT);
    if(!more){
        session->recv_pending = false;
    }

    if(result > 0){
        const char* data = has_buffer
            ? this->provided_buffers.data() + static_cast<std::size_t>(buffer_id) * this->provided_buffer_size
            : session->recv_buffer.data();
        // 닫는 중이어도 이미 받은 데이터의 버퍼는 돌려줘야 한다. 데이터는 버린다.
        const bool keep = session->closing.load() ? false : this->Consume_Received(session, data, static_cast<std::size_t>(result));
        if(has_buffer){
            this->Recycle_Buffer(buffer_id);
        }
        if(session->closing.load()){
            this->Try_Finish_Close(session);
            return;
        }
        // 고정 버퍼 방식은 매번, multishot은 끝났을 때만 다시 건다. 멈춘 세션은 다시 걸지 않는다(수신 멈춤).
        if(keep && !session->recv_pending){
            this->Post_Recv(session);
        }
        return;
    }

    if(has_buffer){
        this->Recycle_Buffer(buffer_id);
    }
    if(session->closing.load()){
        this->Try_Finish_Close(session);
        return;
    }
    if(result == -ENOBUFS){
        // 공용 버퍼가 잠깐 바닥났다. 버퍼는 위에서 바로바로 돌려주므로 곧 다시 걸 수 있다.
        ++this->no_buffer_count;
        this->Post_Recv(session);
        return;
    }
    if(result == -ECANCELED){
        // 수신 멈춤으로 multishot을 취소한 것. 그 사이에 재개됐다면 다시 건다.
        this->Post_Recv(session);
        return;
    }
    this->Begin_Close(session, result == 0 ? "peer closed" : "recv error");
}

void UringWorldServer::Handle_Send(const std::shared_ptr<ClientSession>& session, int result){
    session->send_inflight = false;
    if(result < 0){
        session->send_in_flight.clear();
        if(session->closing.load()){
            this->Try_Finish_Close(session);
        }else{
            this->Begin_Close(session, "send error");
        }
        return;
    }
    bool has_more = false;
    {
        std::lock_guard<std::mutex> lock(session->send_mutex);
        // 보낸 바이트만큼 큐를 앞으로 당긴다(v8 Handle_Send_Completion과 같은 계산).
        std::size_t remaining = static_cast<std::size_t>(result);
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
        has_more = !session->send_queue.empty();
    }
    if(static_cast<std::size_t>(result) < session->send_inflight_bytes){
        ++this->partial_send_count;
    }
    // 커널이 다 썼으므로 붙잡아 두었던 버퍼를 놓는다.
    session->send_in_flight.clear();
    if(session->closing.load()){
        this->Try_Finish_Close(session);
        return;
    }
    if(has_more){
        this->Post_Send(session);
    }
}

void UringWorldServer::Process_Requests(){
    std::vector<std::shared_ptr<ClientSession>> flushes;
    std::vector<std::shared_ptr<ClientSession>> closes;
    {
        std::lock_guard<std::mutex> lock(this->request_mutex);
        flushes.swap(this->flush_requests);
        closes.swap(this->close_requests);
    }
    for(const auto& session : flushes){
        {
            std::lock_guard<std::mutex> lock(session->send_mutex);
            session->flush_requested = false;
        }
        // 이미 송신이 진행 중이면 그 완료가 이어서 보낸다.
        this->Post_Send(session);
    }
    for(const auto& session : closes){
        this->Begin_Close(session, "close requested by tick");
    }
}

void UringWorldServer::Pause_Recv(const std::shared_ptr<ClientSession>& session){
    // 고정 버퍼 방식은 recv를 다시 걸지 않으면 된다(IOCP와 같다). multishot은 걸려 있는 동안 계속 받으므로 취소한다.
    // 취소가 닿기 전에 온 데이터는 그대로 처리한다(버리지 않는다).
    if(session->recv_paused){
        return;
    }
    session->recv_paused = true;
    this->paused_sessions.push_back(session);
    this->paused_count.store(this->paused_sessions.size());
    ++this->recv_pause_count;
    if(this->recv_mode == RecvMode::Provided && session->recv_pending){
        io_uring_sqe* sqe = this->Get_Sqe();
        io_uring_prep_cancel64(sqe, Pack(Op::Recv, session->session_id), 0);
        io_uring_sqe_set_data64(sqe, Pack(Op::Cancel, session->session_id));
        ++this->pending_ops;
        ++this->cancel_count;
    }
}

void UringWorldServer::Resume_Paused_Sessions(){
    if(this->paused_sessions.empty() || this->game_jobs.Size() > this->job_low_watermark){
        return;
    }
    std::vector<std::shared_ptr<ClientSession>> to_resume;
    to_resume.swap(this->paused_sessions);
    this->paused_count.store(0);
    for(std::size_t i = 0; i < to_resume.size(); ++i){
        const std::shared_ptr<ClientSession>& session = to_resume[i];
        if(session->closing.load()){
            continue;
        }
        // 재개하는 중에도 세션마다 수위를 먼저 본다. 넘었으면 남은 세션은 멈춘 채로 다음 기회에 재개한다.
        // 이 확인이 없을 때는 멈춘 세션 수천 개가 쌓아 둔 패킷을 한꺼번에 넣어 큐가 연결 수만큼 다시 찼다.
        if(this->game_jobs.Size() >= this->job_high_watermark){
            this->paused_sessions.insert(this->paused_sessions.end(), to_resume.begin() + static_cast<std::ptrdiff_t>(i), to_resume.end());
            this->paused_count.store(this->paused_sessions.size());
            return;
        }
        session->recv_paused = false;
        // 멈춘 동안 조립기에 쌓아 둔 바이트에서 패킷을 꺼내 넣는다. 그 사이 다시 수위를 넘었으면 Push_Assembled가 다시 멈춘다.
        if(!this->Push_Assembled(session)){
            continue;
        }
        // 취소한 multishot이 아직 끝나지 않았으면(recv_pending) 그 완료(-ECANCELED)에서 다시 건다.
        this->Post_Recv(session);
    }
}

void UringWorldServer::Begin_Close(const std::shared_ptr<ClientSession>& session, const char* reason){
    if(!session || session->closing.exchange(true)){
        return;
    }
    --this->session_count;
    {
        std::lock_guard<std::mutex> lock(session->send_mutex);
        session->send_queue.clear();
        session->send_queued_bytes = 0;
        session->send_offset = 0;
    }
    if(session->recv_pending || session->send_inflight){
        // 진행 중인 recv·send를 취소한다. shutdown도 같이 해, 취소가 닿기 전에 막혀 있던 recv가 바로 끝나게 한다.
        // 취소된 작업은 -ECANCELED(또는 0, -EPIPE)로 돌아오고, 모두 돌아온 뒤에야 Try_Finish_Close가 fd를 닫는다.
        // 그 전에 fd를 닫거나 세션을 놓으면 커널이 아직 쓰는 버퍼(고정 수신 버퍼, 송신 iovec)를 해제하게 된다.
        shutdown(session->fd, SHUT_RDWR);
        io_uring_sqe* sqe = this->Get_Sqe();
        if(session->file_slot >= 0){
            io_uring_prep_cancel_fd(sqe, session->file_slot, IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_FD_FIXED);
        }else{
            io_uring_prep_cancel_fd(sqe, session->fd, IORING_ASYNC_CANCEL_ALL);
        }
        io_uring_sqe_set_data64(sqe, Pack(Op::Cancel, session->session_id));
        ++this->pending_ops;
        ++this->cancel_count;
    }
    this->Logging(LogLevel::Debug, "[UringWorldServer] closing session=" + std::to_string(session->session_id) +
        ", reason=" + reason);
    this->Submit_Leave(session);
    this->Try_Finish_Close(session);
}

void UringWorldServer::Try_Finish_Close(const std::shared_ptr<ClientSession>& session){
    if(!session->closing.load() || session->recv_pending || session->send_inflight){
        return;
    }
    // 이 세션의 작업이 모두 끝났다. 이제야 fd를 닫고 목록에서 뺀다(세션 메모리는 shared_ptr이 마지막 참조와 함께 놓는다).
    if(this->sessions.erase(session->session_id) == 0){
        return;
    }
    // 고정 파일 표에 등록된 fd는 io_uring이 따로 참조를 들고 있고, 표에서 빼는 일(Release_File_Slot)은 비동기로 늦게 끝난다.
    // close만 하면 그동안 소켓이 살아 있어 상대가 종료를 받지 못하므로, shutdown으로 연결부터 끝낸다.
    shutdown(session->fd, SHUT_RDWR);
    this->Release_File_Slot(session->file_slot);
    close(session->fd);
}

void UringWorldServer::Submit_Leave(const std::shared_ptr<ClientSession>& session){
    if(this->server_state.load() != ServerState::Running){
        return;
    }
    GameJob leave;
    leave.type = GameJob::JobType::Leave;
    leave.session = session;
    if(!this->game_jobs.Push(std::move(leave))){
        this->pending_leaves.push_back(session);
    }
}

void UringWorldServer::Retry_Pending_Leaves(){
    if(this->pending_leaves.empty()){
        return;
    }
    std::vector<std::shared_ptr<ClientSession>> retry;
    retry.swap(this->pending_leaves);
    for(const auto& session : retry){
        this->Submit_Leave(session);
    }
}

void UringWorldServer::Clean_Up(){
    this->Logging(LogLevel::Info, "[UringWorldServer] cleanup started");
    // 1) 틱 루프를 먼저 멈춘다.
    this->game_jobs.Close();
    if(this->game_thread.joinable()){
        this->game_thread.join();
    }
    std::size_t closed = 0;
    if(this->ring_ready){
        const auto reap_for = [this](std::chrono::milliseconds limit, const auto& done){
            const auto deadline = std::chrono::steady_clock::now() + limit;
            while(!done() && std::chrono::steady_clock::now() < deadline){
                __kernel_timespec timeout{};
                timeout.tv_nsec = 100 * 1000 * 1000;
                io_uring_cqe* first = nullptr;
                io_uring_submit_and_wait_timeout(&this->ring, &first, 1, &timeout, nullptr);
                ++this->enter_count;
                this->Drain_Completions();
            }
        };
        // 2) 남은 송신을 한 번 더 보낸다(best-effort, 최대 1초).
        for(auto& [id, session] : this->sessions){
            this->Post_Send(session);
        }
        reap_for(std::chrono::milliseconds(1000), [this]{
            for(const auto& [id, session] : this->sessions){
                if(session->send_inflight) return false;
            }
            return true;
        });
        // 3) 모든 세션을 닫고, accept와 eventfd read도 취소한다.
        std::vector<std::shared_ptr<ClientSession>> all;
        for(const auto& [id, session] : this->sessions) all.push_back(session);
        closed = all.size();
        for(const auto& session : all){
            this->Begin_Close(session, "server stopping");
        }
        if(this->accept_armed){
            io_uring_sqe* sqe = this->Get_Sqe();
            io_uring_prep_cancel_fd(sqe, this->listen_fd, IORING_ASYNC_CANCEL_ALL);
            io_uring_sqe_set_data64(sqe, Pack(Op::Cancel, 0));
            ++this->pending_ops;
        }
        if(this->wake_armed){
            io_uring_sqe* sqe = this->Get_Sqe();
            io_uring_prep_cancel64(sqe, Pack(Op::Wake, 0), 0);
            io_uring_sqe_set_data64(sqe, Pack(Op::Cancel, 0));
            ++this->pending_ops;
        }
        // 4) 제출한 작업의 완료를 모두 회수한다(v8 Clean_Up의 pending I/O 대기와 같다). 그 전에 ring을 닫으면 안 된다.
        reap_for(std::chrono::milliseconds(10000), [this]{ return this->pending_ops == 0; });
        if(this->pending_ops != 0){
            this->Logging(LogLevel::Error, "[UringWorldServer] still " + std::to_string(this->pending_ops) +
                " operation(s) pending after 10s");
        }
        for(auto& [id, session] : this->sessions){
            close(session->fd);
        }
        this->sessions.clear();
        if(this->buffer_ring != nullptr){
            io_uring_free_buf_ring(&this->ring, this->buffer_ring, this->provided_buffer_count, kBufferGroup);
            this->buffer_ring = nullptr;
        }
        io_uring_queue_exit(&this->ring);
        this->ring_ready = false;
    }
    this->paused_sessions.clear();
    this->pending_leaves.clear();
    this->session_count.store(0);
    {
        std::lock_guard<std::mutex> lock(this->request_mutex);
        this->flush_requests.clear();
        this->close_requests.clear();
    }
    if(this->listen_fd >= 0){
        close(this->listen_fd);
        this->listen_fd = -1;
    }
    this->server_state.store(ServerState::Stopped);
    this->Logging(LogLevel::Info, "[UringWorldServer] server stopped, closed_sessions=" + std::to_string(closed) +
        ", pending_ops=" + std::to_string(this->pending_ops) +
        ", jobs_processed=" + std::to_string(this->processed_job_count.load()) +
        ", jobs_discarded=" + std::to_string(this->discarded_job_count.load()) +
        ", ticks=" + std::to_string(this->tick_count.load()) +
        ", late_ticks=" + std::to_string(this->late_tick_count.load()) +
        ", max_tick_us=" + std::to_string(this->total_tick_us_max.load()));
}

// ---------------------------------------------------------------- 틱 스레드 → ring

void UringWorldServer::Wake_Ring(){
    // eventfd는 8바이트 카운터다. 여러 번 써도 ring는 한 번 깨어나 모아 처리한다.
    const std::uint64_t one = 1;
    if(this->wake_fd >= 0){
        [[maybe_unused]] const ssize_t written = write(this->wake_fd, &one, sizeof(one));
    }
}

void UringWorldServer::Enqueue_Send(const std::shared_ptr<ClientSession>& session, SendBuffer data){
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
            // ring에 이미 알린 세션이면 다시 알리지 않는다. 한 틱 끝에 목록을 한 번에 넘긴다.
            if(!session->flush_requested){
                session->flush_requested = true;
                this->tick_flushes.push_back(session);
            }
        }
    }
    if(overflow){
        // 응답을 읽지 않는 느린 클라이언트. 틱 스레드는 fd를 닫을 수 없으므로 ring에 닫기를 부탁한다.
        ++this->send_overflow_count;
        this->Logging(LogLevel::Error, "[UringWorldServer] send queue overflow, closing slow client session=" +
            std::to_string(session->session_id));
        this->Request_Close(session);
    }
}

void UringWorldServer::Request_Close(const std::shared_ptr<ClientSession>& session){
    // 한 번만 요청한다. ring가 실제로 닫기 전까지 틱이 여러 번 돌아도 요청과 로그가 쌓이지 않는다.
    if(session->close_requested.exchange(true)){
        return;
    }
    {
        std::lock_guard<std::mutex> lock(this->request_mutex);
        this->close_requests.push_back(session);
    }
    this->Wake_Ring();
}

// ---------------------------------------------------------------- 틱 스레드 (v8과 같은 흐름)

void UringWorldServer::Game_Tick_Loop(){
    this->Logging(LogLevel::Info, "[UringWorldServer] tick loop started, period_ms=" + std::to_string(this->tick_period.count()));
    pthread_setname_np(pthread_self(), "v10-tick");
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
        // 큐를 비웠으니 수신을 멈춘 세션이 있으면 ring를 깨워 다시 켜게 한다(v8은 틱 스레드가 직접 Recv를 게시했다).
        if(this->paused_count.load() != 0 && this->game_jobs.Size() <= this->job_low_watermark){
            this->Wake_Ring();
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
    this->Logging(LogLevel::Info, "[UringWorldServer] tick loop stopped");
}

bool UringWorldServer::Process_Or_Defer(GameJob& job){
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

void UringWorldServer::Process_Job(GameJob& job){
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

void UringWorldServer::Record_Tick_Time(std::uint64_t elapsed_us){
    ++this->window_tick_count;
    this->window_tick_us_sum += elapsed_us;
    const auto update_max = [elapsed_us](std::atomic<std::uint64_t>& target){
        std::uint64_t current = target.load();
        while(elapsed_us > current && !target.compare_exchange_weak(current, elapsed_us)){}
    };
    update_max(this->window_tick_us_max);
    update_max(this->total_tick_us_max);
}

void UringWorldServer::Handle_Enter(const std::shared_ptr<ClientSession>& session){
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

void UringWorldServer::Handle_Leave(const std::shared_ptr<ClientSession>& session){
    if(this->players.erase(session->session_id) == 0){
        return;
    }
    this->world.Leave(session->session_id);
    this->outbox.erase(session->session_id);
    this->Logging(LogLevel::Debug, "[Game] player " + std::to_string(session->session_id) + " left, players=" +
        std::to_string(this->world.Player_Count()));
}

void UringWorldServer::Handle_Packet(const std::shared_ptr<ClientSession>& session, const protocol::Packet& packet){
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

void UringWorldServer::Expire_Idle_Inputs(){
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

void UringWorldServer::Flush_Tick_Output(){
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
    // 이번 틱에 보낼 것이 생긴 세션을 한 번에 넘기고 ring를 한 번만 깨운다.
    if(!this->tick_flushes.empty()){
        {
            std::lock_guard<std::mutex> lock(this->request_mutex);
            this->flush_requests.insert(this->flush_requests.end(),
                std::make_move_iterator(this->tick_flushes.begin()), std::make_move_iterator(this->tick_flushes.end()));
        }
        this->tick_flushes.clear();
        this->Wake_Ring();
    }
    const auto send_end = std::chrono::steady_clock::now();
    this->window_phase_us[2] += Elapsed_Us(build_start, collect_end);
    this->window_phase_us[3] += Elapsed_Us(collect_end, send_start);
    this->window_phase_us[4] += Elapsed_Us(send_start, send_end);
    this->window_sent_bytes += tick_bytes;
    this->player_count.store(this->world.Player_Count());
    this->max_cell_population.store(this->world.Max_Cell_Population());
}

void UringWorldServer::Log_Status(){
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
        ", jobs_processed=" + std::to_string(this->processed_job_count.load()) +
        ", jobs_rejected=" + std::to_string(this->rejected_job_count.load()) +
        ", recv_paused_now=" + std::to_string(this->paused_count.load()) +
        ", recv_pauses=" + std::to_string(this->recv_pause_count.load()) +
        ", uring_enters=" + std::to_string(this->enter_count.load()) +
        ", sqes=" + std::to_string(this->sqe_count.load()) +
        ", cqes=" + std::to_string(this->cqe_count.load()) +
        ", pending_ops=" + std::to_string(this->pending_ops_now.load()) +
        ", cq_overflow=" + std::to_string(this->cq_overflow.load()) +
        ", wakes=" + std::to_string(this->wake_count.load()) +
        ", sends=" + std::to_string(this->send_count.load()) +
        ", partial_sends=" + std::to_string(this->partial_send_count.load()) +
        ", send_overflows=" + std::to_string(this->send_overflow_count.load()) +
        ", cancels=" + std::to_string(this->cancel_count.load()) +
        ", no_buffers=" + std::to_string(this->no_buffer_count.load()) +
        ", moves=" + std::to_string(this->move_input_count.load()) +
        ", inputs_expired=" + std::to_string(this->expired_input_count.load()) +
        ", chats=" + std::to_string(this->chat_count.load()) +
        ", chats_dropped=" + std::to_string(this->dropped_chat_count.load()) +
        ", log_dropped=" + std::to_string(this->logger.Dropped_Count()));
}
