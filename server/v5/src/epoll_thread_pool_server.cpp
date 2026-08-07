#include <epoll_thread_pool_server.h>
#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#ifdef __linux__
    #include <sys/epoll.h>
    #include <sys/eventfd.h>
#else
    #warning "epoll is Linux-specific; use a Linux target or update include paths."
#endif

constexpr int kBacklog = 128;
constexpr int kMaxEvents = 64;
constexpr int TIME_OUT = 30000;

using std::cout;
using std::endl;

EpollThreadPoolServer::EpollThreadPoolServer(int port){
    this->server_port = port;
}

EpollThreadPoolServer::~EpollThreadPoolServer(){
    // Proceed_Stop()이 Stop()과 Clean_Up()을 순서대로 호출한다. 두 함수 모두
    // 상태 전이를 compare_exchange로 지키므로 이미 종료된 서버에 다시 호출해도 안전하다.
    this->Proceed_Stop();

    return;
}

// socket/bind/listen/epoll을 준비하고, 워커·모니터 스레드를 띄운 뒤 서버가 멈출
// 때까지(server_state != Running) epoll 이벤트 루프를 돈다. 블로킹 함수이므로
// main()에서 별도 스레드로 돌리고 이 스레드는 Server_Run() 반환을 기다린다.
void EpollThreadPoolServer::Server_Run(){
    {
        std::ostringstream oss;
        oss << "[EpollThreadPoolServer] Server starting on port " << this->server_port;
        this->Logging(oss.str());
    }

    int socket_result = socket(AF_INET, SOCK_STREAM, 0);
    if(socket_result < 0){
        perror("socket_result < 0");
        return;
    }

    // Server_Run()은 Created 상태에서 한 번만 시작할 수 있다.
    // compare_exchange로 Created -> Running 전이에 성공한 경우에만 listen fd를 소유한다.
    ServerState expected = ServerState::Created;

    if(!this->server_state.compare_exchange_strong(expected, ServerState::Running)){
        this->Logging("[EpollThreadPoolServer] Server is already running...");
        close(socket_result);
        return;
    }

    this->listen_fd = socket_result;

    // 개발 중 같은 포트를 빠르게 재사용할 수 있도록 bind 전에 설정한다.
    int reuse_addr = 1;
    int option_result = setsockopt(
        this->listen_fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse_addr,
        sizeof(reuse_addr)
    );

    if(option_result < 0){
        perror("option_result < 0");
        this->Proceed_Stop();
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(this->server_port);

    {
        std::ostringstream oss;
        oss << "[EpollThreadPoolServer] Binding port " << this->server_port
            << " with listen_fd " << this->listen_fd;
        this->Logging(oss.str());
    }
    int bind_result = bind(this->listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if(bind_result < 0){
        perror("bind_result < 0");
        this->Proceed_Stop();
        return;
    }

    {
        std::ostringstream oss;
        oss << "[EpollThreadPoolServer] Listening on port " << this->server_port
            << " with listen_fd " << this->listen_fd;
        this->Logging(oss.str());
    }
    int listen_result = listen(this->listen_fd, kBacklog);
    if(listen_result < 0){
        perror("listen_result < 0");
        this->Proceed_Stop();
        return;
    }

    // reactor가 EPOLLOUT으로 송신을 재개할 수 있으려면 listen fd도 미리 non-blocking으로
    // 만들어둬야 accept()가 backlog를 비운 뒤 즉시 EAGAIN으로 돌아온다.
    int current_flags = fcntl(this->listen_fd, F_GETFL, 0);
    if(current_flags < 0 || fcntl(this->listen_fd, F_SETFL, current_flags | O_NONBLOCK) < 0){
        perror("fcntl O_NONBLOCK");
        this->Proceed_Stop();
        return;
    }

    this->Logging("[EpollThreadPoolServer] Epoll instance creating...");
    int epoll_result = epoll_create1(0);
    if(epoll_result < 0){
        perror("epoll_result < 0");
        this->Proceed_Stop();
        return;
    }

    this->epoll_fd = epoll_result;

    // listen fd는 새 연결 요청을 의미하므로, 이벤트가 오면 accept()로 처리한다.
    epoll_event listen_event{};
    listen_event.events = EPOLLIN;
    listen_event.data.fd = this->listen_fd;
    int listen_ctl = epoll_ctl(this->epoll_fd, EPOLL_CTL_ADD, this->listen_fd, &listen_event);
    if(listen_ctl < 0){
        perror("listen_ctl < 0");
        this->Proceed_Stop();
        return;
    }

    this->Logging("[EpollThreadPoolServer] Stop event fd creating...");
    int stop_fd_result = eventfd(0, EFD_NONBLOCK);
    if(stop_fd_result < 0){
        perror("stop_fd_result < 0");
        this->Proceed_Stop();
        return;
    }

    this->stop_event_fd = stop_fd_result;

    // Stop()이 eventfd에 write하면 epoll_wait()가 즉시 깨어나 종료 분기로 들어온다.
    epoll_event stop_event{};
    stop_event.events = EPOLLIN;
    stop_event.data.fd = this->stop_event_fd;
    int stop_ctl = epoll_ctl(this->epoll_fd, EPOLL_CTL_ADD, this->stop_event_fd, &stop_event);
    if(stop_ctl < 0){
        perror("stop_Ctl < 0");
        this->Proceed_Stop();
        return;
    }
    {
        std::ostringstream oss;
        oss << "[EpollThreadPoolServer] Stop event fd registered: " << this->stop_event_fd;
        this->Logging(oss.str());
    }

    epoll_event wait_events[kMaxEvents]{}; // epoll_wait()가 매번 채워주는 "준비된 이벤트" 결과 배열

    // 고정 개수의 워커 스레드를 미리 만들어 재사용한다. 접속마다 스레드를 새로 만들지
    // 않으므로 접속 수와 스레드 수가 분리된다.
    {
        std::ostringstream oss;
        oss << "[EpollThreadPoolServer] Starting " << this->kWorkerCounts << " worker threads...";
        this->Logging(oss.str());
    }
    for(int i=0; i<this->kWorkerCounts; i++){
        this->threads.emplace_back(&EpollThreadPoolServer::Worker_Loop, this, i);
    }

    this->monitor_thread = std::thread(&EpollThreadPoolServer::Monitor_Workers, this);

    // epoll_wait()는 준비된 fd 목록을 돌려준다. fd 종류에 따라 stop/accept/client로 분기한다.
    while(this->server_state.load() == ServerState::Running){

        int wait_result = epoll_wait(
            this->epoll_fd,
            wait_events,
            kMaxEvents,
            TIME_OUT
        );

        if(wait_result < 0){
            perror("wait_result < 0");
            this->Proceed_Stop();
            return;
        }

        for(int i=0; i<wait_result; i++){
            int event_fd = wait_events[i].data.fd;
            {
                std::ostringstream oss;
                oss << "[EpollThreadPoolServer] Event ready index=" << i
                    << " fd=" << event_fd << " events=" << wait_events[i].events;
                this->Logging(oss.str());
            }

            if(event_fd == this->stop_event_fd){
                std::ostringstream oss;
                oss << "[EpollThreadPoolServer] Stop event received on fd " << event_fd;
                this->Logging(oss.str());

                // Stop()이 이미 Running -> Stopping으로 바꿨으므로 여기서는
                // eventfd만 비우고 루프를 빠져나간다.
                uint64_t stop_value = 1;
                ssize_t read_result = read(this->stop_event_fd, &stop_value, sizeof(stop_value));
                if(read_result < 0){
                    perror("read_result < 0");
                }

                break;
            }
            else if(event_fd == this->listen_fd){
                sockaddr_in client_addr{};
                socklen_t client_len = sizeof(client_addr);
                int accept_result = accept(event_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
                if(accept_result < 0){
                    perror("accept_result < 0");
                    continue;
                }
                {
                    std::ostringstream oss;
                    oss << "[EpollThreadPoolServer] Client accepted: " << accept_result;
                    this->Logging(oss.str());
                }

                // client fd도 reactor가 EPOLLOUT으로 송신을 재개할 수 있어야 하므로
                // non-blocking으로 만든다.
                int current_flags = fcntl(accept_result, F_GETFL, 0);
                if(current_flags < 0 || fcntl(accept_result, F_SETFL, current_flags | O_NONBLOCK) < 0){
                    perror("accept_result fcntl O_NONBLOCK");
                    close(accept_result);
                    continue;
                }

                epoll_event event_epoll{};
                event_epoll.events = EPOLLIN;
                event_epoll.data.fd = accept_result;

                // accept된 client fd도 epoll에 등록해야 이후 recv 이벤트를 받을 수 있다.
                int add_ctl = epoll_ctl(this->epoll_fd, EPOLL_CTL_ADD, accept_result, &event_epoll);
                if(add_ctl < 0){
                    perror("event add_ctl < 0");
                    close(accept_result);
                    continue;
                }
                {
                    std::ostringstream oss;
                    oss << "[EpollThreadPoolServer] Client registered to epoll: " << accept_result;
                    this->Logging(oss.str());
                }

                {
                    std::lock_guard<std::mutex> lock (this->active_client_mutex);
                    if(this->client_fds.find(accept_result) != this->client_fds.end()){
                        std::ostringstream oss;
                        oss << "[EpollThreadPoolServer] Client already tracked: " << accept_result;
                        this->Logging(oss.str());
                    }else{
                        std::ostringstream oss;
                        oss << "[EpollThreadPoolServer] Client tracked: " << accept_result;
                        this->Logging(oss.str());
                        this->client_fds.insert(accept_result);
                    }
                }

                // reactor가 EPOLLOUT을 받았을 때 비울 수 있도록, 각 client fd마다
                // 세션별 송신 버퍼 자리를 미리 만들어둔다.
                {
                    std::lock_guard<std::mutex> lock (this->send_mutex);
                    if(this->send_buffers.find(accept_result) != this->send_buffers.end()){
                        std::ostringstream oss;
                        oss << "[EpollThreadPoolServer] Client already has a send buffer: " << accept_result;
                        this->Logging(oss.str());
                    }
                    else{
                        std::ostringstream oss;
                        oss << "[EpollThreadPoolServer] Send buffer initialized for client: " << accept_result;
                        this->Logging(oss.str());
                        this->send_buffers.insert({accept_result, ""});
                    }
                }

            }
            else{
                uint32_t event = wait_events[i].events;
                if(event & EPOLLIN){
                    char buffer[4096];
                    int recv_result = recv(event_fd, buffer, sizeof(buffer), 0);

                    if(recv_result < 0){
                        // EAGAIN/EWOULDBLOCK은 이번엔 읽을 데이터가 없다는 정상적인
                        // 신호이고, EINTR은 시그널로 중단된 것이라 둘 다 에러가 아니다.
                        if(errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)continue;

                        perror("recv_result < 0");
                        this->Delete_Client_Fd(event_fd);
                        continue;
                    }
                    else if(recv_result == 0){
                        // recv()가 0을 반환하면 peer가 정상적으로 연결을 닫은 것이다.
                        std::ostringstream oss;
                        oss << "[EpollThreadPoolServer] Client disconnected: " << event_fd;
                        this->Logging(oss.str());
                        this->Delete_Client_Fd(event_fd);
                        continue;
                    }
                    else{
                        std::ostringstream oss;
                        oss << "[EpollThreadPoolServer] Received " << recv_result
                            << " bytes from client " << event_fd;
                        this->Logging(oss.str());
                        // reactor는 recv만 하고, 실제 처리(세션 버퍼에 쌓기)는
                        // job queue를 통해 워커에게 넘긴다.
                        this->Enqueue_Job(event_fd, buffer, recv_result);
                    }
                }

                if(event & EPOLLOUT){
                    bool send_state = false; // true면 Send_All()이 복구 불가능한 에러를 반환한 것 -> 연결 정리 필요
                    {
                        std::lock_guard<std::mutex> lock(this->send_mutex);
                        if(this->send_buffers.find(event_fd) != this->send_buffers.end()){
                            SendState send_result = Send_All(event_fd, this->send_buffers[event_fd]);
                            if(send_result == SendState::Completed){
                                std::ostringstream oss;
                                oss << "[EpollThreadPoolServer] Send buffer drained for client: " << event_fd;
                                this->Logging(oss.str());

                                // 더 이상 보낼 게 없으니 EPOLLOUT 감시를 끄지 않으면
                                // 소켓이 계속 쓰기 가능한 상태로 남아 매번 이벤트가 온다.
                                epoll_event send_event{};
                                send_event.events = EPOLLIN;
                                send_event.data.fd = event_fd;

                                int out_send_result = epoll_ctl(this->epoll_fd, EPOLL_CTL_MOD, event_fd, &send_event);
                                if(out_send_result < 0){
                                    perror("EPOLLOUT out_send_result < 0");
                                    continue;
                                }
                            }
                            else if(send_result == SendState::Partial){
                                std::ostringstream oss;
                                oss << "[EpollThreadPoolServer] Send buffer partially drained for client: " << event_fd
                                    << ", remaining=" << this->send_buffers[event_fd].size();
                                this->Logging(oss.str());
                                // EPOLLOUT은 이미 등록돼 있으므로 다음 이벤트에서 이어서 보낸다.
                            }
                            else if(send_result == SendState::Failed){
                                send_state = true;
                            }
                        }
                    }

                    if(send_state){
                        this->Delete_Client_Fd(event_fd);
                    }
                }

            }
        }
    }

    this->Logging("[EpollThreadPoolServer] Event loop stopped");
    this->Clean_Up();

}

// 종료를 요청한다. 상태를 Running -> Stopping으로 바꾸고, 잠들어 있는 워커/모니터를
// 모두 깨우고, stop_event_fd에 써서 epoll_wait()도 깨운다. 여러 스레드가 동시에
// 불러도 compare_exchange 덕분에 실제 종료 절차는 한 번만 실행된다.
void EpollThreadPoolServer::Stop(){
    this->Logging("[EpollThreadPoolServer] Server tries to Stop...");

    int notify_fd = -1; // fd_mutex로 보호된 stop_event_fd를 잠깐 복사해두는 지역 변수

    // 이미 종료 요청/정리/종료 완료 상태라면 중복으로 워커와 eventfd를 깨우지 않는다.
    ServerState current_state = this->server_state.load();
    if(current_state == ServerState::Stopping ||
        current_state == ServerState::Cleaning ||
        current_state == ServerState::Stopped){

        return;
    }

    ServerState expected = ServerState::Running;

    if(!this->server_state.compare_exchange_strong(expected, ServerState::Stopping)){
        // 아직 Server_Run()이 시작되지 않은 Created 상태에서 Stop()이 호출되면
        // 깨울 워커도, 닫을 fd도 없으므로 Stopped로만 전환한다.
        expected = ServerState::Created;
        this->server_state.compare_exchange_strong(expected, ServerState::Stopped);

        return;
    }

    // 워커마다 큐가 따로 있으므로, 하나라도 깨우지 않으면 그 워커는 종료 조건을
    // 확인할 기회가 없어 join()이 영원히 끝나지 않는다.
    for(Worker& worker : this->workers){
        worker.job_condition.notify_all();
    }

    // 모니터는 특정 워커의 job 알림과는 무관하게 자기 전용 조건변수에서 기다리므로
    // 따로 깨워줘야 한다.
    this->monitor_condition.notify_all();

    {
        std::lock_guard<std::mutex>lock (this->fd_mutex);
        // fd 값만 복사해두고 실제 write는 lock을 잡은 채로 수행해 stop_event_fd가
        // Clean_Up()에서 동시에 바뀌는 일이 없도록 한다.
        notify_fd = this->stop_event_fd;

        if(notify_fd >= 0){
            uint64_t stop_value = 1;
            ssize_t write_result = write(notify_fd, &stop_value, sizeof(stop_value));

            if(write_result < 0){
                perror("write_result < 0");
            }else{
                this->Logging("[EpollThreadPoolServer] Stop event notified");
            }
        }
    }
}

// Stop() + Clean_Up()을 순서대로 실행하는 편의 함수. 초기화 실패 경로와 소멸자에서
// "종료 요청 후 바로 정리"가 세트로 필요할 때 반복해서 두 줄을 쓰지 않도록 묶었다.
void EpollThreadPoolServer::Proceed_Stop(){
    this->Stop();
    this->Clean_Up();
    return;
}

// 워커/모니터 스레드 join, client fd·송신 버퍼 정리, listen/epoll/stop fd close까지
// 서버가 쓰던 모든 자원을 되돌린다. Stopping 상태에서만 실제로 동작하므로 여러 번
// 호출돼도 안전하다(중복 close 방지).
void EpollThreadPoolServer::Clean_Up(){
    this->Logging("[EpollThreadPoolServer] Server tries to Clean up...");

    // Stopping -> Cleaning 전이에 성공한 호출자만 fd 정리를 수행한다.
    // 다른 경로에서 Clean_Up()이 다시 호출되면 여기서 빠져나가 중복 close를 막는다.
    ServerState expected = ServerState::Stopping;

    if(!this->server_state.compare_exchange_strong(expected, ServerState::Cleaning)){
        return;
    }

    for(auto& t: this->threads){
        if(t.joinable()){
            t.join();
        }
    }

    this->workers.clear();

    if(this->monitor_thread.joinable()){
        this->monitor_thread.join();
    }

    {
        std::lock_guard<std::mutex>lock (this->active_client_mutex);
        for(int client_fd : this->client_fds){
            {
                std::lock_guard<std::mutex>lock (this->send_mutex);
                if(this->send_buffers.find(client_fd) != this->send_buffers.end()){
                    this->Send_All(client_fd, this->send_buffers[client_fd]);
                }
            }
            
            close(client_fd);
        }
        
        this->client_fds.clear();
    }


    {
        std::lock_guard<std::mutex>lock (this->send_mutex);
        this->send_buffers.clear();
    }

    // fd들은 한 번만 닫아야 하므로 닫은 뒤 -1로 되돌린다.
    if(this->listen_fd > 0){
        close(this->listen_fd);
        this->listen_fd = -1;
    }

    if(this->epoll_fd > 0){
        close(this->epoll_fd);
        this->epoll_fd = -1;
    }

    if(this->stop_event_fd > 0){
        close(this->stop_event_fd);
        this->stop_event_fd = -1;
    }

    this->Logging("[EpollThreadPoolServer] Cleanup finished");
    // 모든 fd/워커 정리가 끝난 뒤에야 Stopped로 표시한다.
    this->server_state.store(ServerState::Stopped);
}

// 워커 스레드의 메인 루프. index로 자기 담당 workers[index] 큐만 보고, job이 오거나
// 서버가 종료될 때까지 잠들어 있다가 깨어나면 job을 하나 꺼내 Process_Job()에 넘긴다.
void EpollThreadPoolServer::Worker_Loop(int index){
    while(true){
        std::unique_lock <std::mutex> worker_lock(this->workers[index].worker_mutex);

        this->sleeping_worker_count++;
        {
            std::ostringstream oss;
            oss << "[Worker " << index << "] Waiting for job...";
            this->Logging(oss.str());
        }

        // wait()는 대기하는 동안 뮤텍스를 풀어 다른 스레드가 큐에 접근할 수 있게 하고,
        // 깨어난 뒤 predicate가 참일 때만 뮤텍스를 다시 쥔 채로 반환한다.
        this->workers[index].job_condition.wait(
            worker_lock,
            [this, index]{
                    ServerState currentState = this->server_state.load();
                    return   currentState == ServerState::Stopping ||
                            currentState == ServerState::Cleaning ||
                            currentState == ServerState::Stopped ||
                            !this->workers[index].job_queue.empty();}
        );

        this->sleeping_worker_count--;
        ServerState currentState = this->server_state.load();
        if( (currentState == ServerState::Stopping ||
            currentState == ServerState::Cleaning ||
            currentState == ServerState::Stopped) &&
            this->workers[index].job_queue.empty()){
            std::ostringstream oss;
            oss << "[Worker " << index << "] Stopping, queue empty. Exiting.";
            this->Logging(oss.str());
            break;
        }

        Job client = this->workers[index].job_queue.front();
        this->workers[index].job_queue.pop();
        {
            std::ostringstream oss;
            oss << "[Worker " << index << "] Popped job for client fd=" << client.client_fd;
            this->Logging(oss.str());
        }

        worker_lock.unlock();
        this->Process_Job(client);
    }
}

// 워커가 job 하나를 실제로 처리하는 자리. 지금은 echo라 "처리"가 곧 세션 송신
// 버퍼에 데이터를 append하는 것뿐이고, 실제 send()는 여기서 하지 않는다(reactor 담당).
void EpollThreadPoolServer::Process_Job(const Job& job){
    bool delete_state = false; // true면 이 job의 대상 커넥션이 이미 정리된 상태 -> 처리 대신 정리

    {
        std::lock_guard<std::mutex> lock (this->send_mutex);
        if(this->send_buffers.find(job.client_fd) != this->send_buffers.end()){
            this->send_buffers[job.client_fd].append(job.data);
        }
        else{
            // 이미 연결이 정리된 fd에 대해 뒤늦게 도착한 job이다. 처리할 세션이
            // 없으므로 이 job은 버리고 정리 경로로 보낸다.
            delete_state = true;
        }
    }

    if(!delete_state){
        std::ostringstream oss;
        oss << "[EpollThreadPoolServer] Job appended to send buffer for client " << job.client_fd
            << ", " << job.data.size() << " bytes";
        this->Logging(oss.str());

        // 워커는 소켓에 직접 쓰지 않는다. EPOLLOUT을 등록해두면 실제 send()는
        // reactor가 전담한다.
        epoll_event process_event{};
        process_event.events = EPOLLIN | EPOLLOUT;
        process_event.data.fd = job.client_fd;
        int process_result = epoll_ctl(this->epoll_fd, EPOLL_CTL_MOD, job.client_fd, &process_event);
        if(process_result < 0){
            perror("process_result < 0");
        }
    }
    else {
        this->Delete_Client_Fd(job.client_fd);
    }
}

// reactor가 recv()로 받은 원본 바이트를 워커에게 넘긴다(job queue에 push + 대상
// 워커 깨우기). reactor 스레드에서 호출되고, 이후 처리는 워커 스레드에서 이어진다.
void EpollThreadPoolServer::Enqueue_Job(int client_fd, const char* data, std::size_t length){
    // 같은 커넥션에서 온 job은 항상 같은 워커가 처리하도록 client_fd로 워커를
    // 고정 배정한다(sticky routing). 그래야 한 커넥션의 메시지 처리 순서가
    // 여러 워커에 흩어져 뒤바뀌는 일이 없다.
    int index = client_fd % this->kWorkerCounts; // 이 job이 들어갈 workers[index]

    Job newJob{client_fd, std::string(data, length)};
    {
        std::lock_guard<std::mutex> lock(this->workers[index].worker_mutex);
        this->workers[index].job_queue.push(newJob);
    }
    {
        std::ostringstream oss;
        oss << "[EpollThreadPoolServer] Enqueued job for client " << client_fd
            << " into worker " << index;
        this->Logging(oss.str());
    }

    this->workers[index].job_condition.notify_one();
}

// client_fd에 data를 최대한 보낸다. client_fd는 non-blocking이라 한 번의 send()로
// 다 못 보낼 수 있고, 그 경우 이미 보낸 만큼 data 앞부분을 지우고 Partial을 반환한다
// (호출한 쪽이 나머지를 다음 EPOLLOUT 때 이어 보낼 수 있도록).
EpollThreadPoolServer::SendState EpollThreadPoolServer::Send_All(int client_fd, std::string& data){
    // 지금까지 실제로 전송한 바이트 수. length에 도달하면 전체 전송이 완료된 것이다.
    std::size_t total_sent = 0;

    while(total_sent < data.size()){
        // partial send 이후에는 이미 보낸 범위를 건너뛰고 남은 주소와 길이만 다시 전달한다.
        const char* start_data = data.data() + total_sent;
        std::size_t send_len = data.size() - total_sent;

        {
            std::ostringstream oss;
            oss << "[EpollThreadPoolServer] Sending to client " << client_fd << ": "
                << std::string(start_data, send_len);
            this->Logging(oss.str());
        }

        ssize_t send_result = send(client_fd, start_data, send_len, 0);
        if(send_result > 0){
            total_sent += send_result;
        }
        else if(send_result < 0 && errno == EINTR){
            // 시그널로 중단된 경우에는 전송량을 변경하지 않고 같은 남은 범위를 다시 시도한다.
            continue;
        }
        else if(errno == EAGAIN || errno == EWOULDBLOCK){
            // non-blocking 소켓의 커널 송신 버퍼가 찬 것뿐이라 에러가 아니다.
            // 지금까지 보낸 만큼만 지우고 나머지는 다음 EPOLLOUT 때 이어서 보낸다.
            data.erase(0, total_sent);
            return SendState::Partial;
        }
        else{
            perror("send_result <= 0");
            return SendState::Failed;
        }
    }

    data.clear();

    return SendState::Completed;
}

// 연결 하나를 완전히 정리한다: client_fds/send_buffers에서 제거, epoll 감시 해제,
// close(). recv 에러/정상종료, send 실패, 뒤늦게 도착한 job 등 여러 경로에서
// 같은 fd로 호출될 수 있어 반드시 중복 호출에 안전해야 한다.
void EpollThreadPoolServer::Delete_Client_Fd(int client_fd){
    if(client_fd >= 0){
        // client_fds와 send_buffers 둘 중 하나라도 이 fd를 찾아서 지웠다면
        // 실제로 정리해야 할 연결이었다는 뜻이다. 같은 fd로 두 번 호출돼도
        // 두 번째 호출에서는 state가 false로 남아 epoll_ctl/close가 중복
        // 실행되지 않는다 (fd 재사용 시 엉뚱한 연결을 끊는 걸 방지).
        bool state = false; // true여야 실제 epoll_ctl(DEL)+close()를 수행
        {
            std::lock_guard<std::mutex> lock(this->active_client_mutex);
            if(this->client_fds.find(client_fd) != this->client_fds.end()){
                this->client_fds.erase(client_fd);
                state = true;
            }else{
                std::ostringstream oss;
                oss << "[EpollThreadPoolServer] Client not found while closing client_fds: " << client_fd;
                this->Logging(oss.str());
            }
        }

        {
            std::lock_guard<std::mutex> lock(this->send_mutex);
            if(this->send_buffers.find(client_fd) != this->send_buffers.end()){
                this->send_buffers.erase(client_fd);
                state = true;
            }else{
                std::ostringstream oss;
                oss << "[EpollThreadPoolServer] Client not found while closing send buffer: " << client_fd;
                this->Logging(oss.str());
            }
        }

        if(state){
            std::ostringstream oss;
            oss << "[EpollThreadPoolServer] Client closed: " << client_fd;
            this->Logging(oss.str());

            #ifdef __linux__
                // client fd를 닫기 전에 epoll 감시 목록에서도 제거한다.
                int del_ctl = epoll_ctl(this->epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                if(del_ctl < 0){
                    perror("event del del_ctl < 0");
                }
            #endif

            close(client_fd);
        }
    }
}

// 30초마다(또는 종료 신호를 받으면 즉시) 워커 상태(sleeping/awake/큐 길이)를 로그로
// 남기는 모니터 스레드의 메인 루프. 어떤 워커의 job 알림과도 얽히지 않도록 전용
// monitor_mutex/monitor_condition만 사용한다.
void EpollThreadPoolServer::Monitor_Workers(){
    while(true){
        std::size_t queue_size = 0; // 이번 주기에 워커별 큐 길이를 합산한 값
        {
            std::unique_lock<std::mutex> lock(this->monitor_mutex);

            // 평상시에는 30초 주기로 상태를 출력하지만, Stop()의 notify_all()과
            // 종료 predicate에는 즉시 반응한다. wait_for()의 반환값이 predicate
            // 결과이므로 true면 종료, false면 순수 타임아웃이다.
            bool monitor_stop = this->monitor_condition.wait_for(
                lock,
                std::chrono::seconds(30),
                [this]{
                    ServerState current_state = this->server_state.load();
                    return  current_state == ServerState::Stopping ||
                            current_state == ServerState::Cleaning ||
                            current_state == ServerState::Stopped;}
            );

            if(monitor_stop){
                break;
            }

            // 워커별 큐 길이는 각 워커의 뮤텍스로 보호되는 자원이라, 합산하려면
            // 워커마다 잠깐씩 잠그고 읽어야 한다.
            for(Worker& worker : this->workers){
                int size = 0;
                {
                    std::lock_guard<std::mutex> lock(worker.worker_mutex);
                    size = worker.job_queue.size();
                }

                queue_size += size;
            }
        }

        int sleeping = this->sleeping_worker_count.load();
        int awake = this->kWorkerCounts - sleeping;
        std::ostringstream oss;
        oss << "[Worker Status] Total: " << this->kWorkerCounts << ", Sleeping: " << sleeping
            << ", Awake: " << awake << ", Queued: " << queue_size;
        this->Logging(oss.str());
    }
}

// 모든 로그가 거치는 단일 출력 지점. 호출부가 std::ostringstream으로 한 줄을
// 완성해서 넘기면, 여기서 log_mutex로 보호된 printf 한 번으로 출력한다.
void EpollThreadPoolServer::Logging(const std::string &msg){
    // 여러 스레드(reactor, 워커, 모니터)가 동시에 로그를 찍어도 한 줄 단위로는
    // 섞이지 않도록, 호출부에서 메시지를 완성한 뒤 이 함수 하나로만 출력한다.
    std::lock_guard<std::mutex> lock(this->log_mutex);
    printf("%s\n", msg.c_str());
}
