#include <iostream>
#include <string>
#include <queue>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread_pool_echo_server.h>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>

using std::cout;
using std::endl;
using std::thread;

constexpr int kBacklog = 128;
constexpr std::size_t kMaxQueueSize = 128;
constexpr int kAcceptPollTimeoutMs = 100;
// 접속자 수와 무관하게 유지할 고정 worker 수.
constexpr int numThreads = 4;

ThreadPoolEchoServer::ThreadPoolEchoServer(int port){
    this->server_port = port;
}

ThreadPoolEchoServer::~ThreadPoolEchoServer(){
    // 명시적 Stop() 호출 여부와 관계없이 동일한 종료 절차를 한 번 거친다.
    this->Stop();

    // std::thread가 joinable인 채 파괴되면 std::terminate()가 호출되므로 모두 회수한다.
    for(std::thread& worker : this->workers){
        if(worker.joinable()){
            worker.join();
        }
    }

    if(monitor_thread.joinable()){
        monitor_thread.join();
    }

    // accept 루프와 worker가 모두 끝난 뒤 listen 소켓의 소유권을 최종 정리한다.
    if(this->fd >= 0){
        close(this->fd);
        this->fd = -1;
    }
}

void ThreadPoolEchoServer::Server_Run(){
    // 1) TCP/IPv4 listen 소켓 생성.
    cout << "ThreadPoolEchoServer Start Running... " << endl;    
    
    int socket_result = socket(AF_INET, SOCK_STREAM, 0);
    if(socket_result < 0){
        perror("socket_result < 0");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(this->queue_mutex);
        if(this->stopping){
            close(socket_result);
            return;
        }
        this->fd = socket_result;
    }

    // 서버 종료 후 같은 주소/포트로 즉시 재실행할 수 있도록 bind() 전에 주소 재사용을 허용한다.
    int reuse_addr = 1;
    int option_result = setsockopt(
        this->fd,
        SOL_SOCKET,         // 소켓 API 공통 옵션 레벨
        SO_REUSEADDR,       // 종료 후 남은 주소를 재사용하도록 허용
        &reuse_addr,        // 1: 옵션 활성화
        sizeof(reuse_addr)  // 커널에 전달하는 옵션 값의 크기
    );

    // setsockopt()도 예외가 아니라 음수 반환값으로 실패를 알린다.
    if(option_result < 0){
        perror("option_result < 0");
        return;
    }

    // 2) 모든 로컬 인터페이스의 지정 포트에 바인딩할 주소 구성.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(this->server_port);

    // 3) 주소/포트 바인딩.
    cout << "ThreadPoolEchoServer Start Binding: " << this->fd << endl;
    int bind_result = bind(this->fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if(bind_result < 0){
        perror("bind_result < 0");
        return;
    }

    // 4) 연결 요청을 backlog 큐에서 기다릴 수 있도록 listen 상태로 전환.
    cout << "ThreadPoolEchoServer Start Listening: " << this->fd << endl;
    int listen_result = listen(this->fd, kBacklog);
    if(listen_result < 0){
        perror("listen_result < 0");
        return;
    }

    // poll() 뒤 accept() 사이의 종료 race에서도 accept()가 blocking되지 않도록 설정한다.
    int current_flags = fcntl(this->fd, F_GETFL, 0);
    if(current_flags < 0 || fcntl(this->fd, F_SETFL, current_flags | O_NONBLOCK) < 0){
        perror("fcntl O_NONBLOCK");
        return;
    }

    // 5) accept 루프 전에 worker를 한 번만 생성한다.
    // v2처럼 접속마다 스레드를 생성하지 않는 것이 v3의 핵심 차이다.
    cout << "ThreadPoolEchoServer Thread Initializing... " << endl;
    for(int i=0; i<numThreads; i++){
        this->workers.emplace_back(&ThreadPoolEchoServer::Worker_Loop, this);
        cout << "Thread " << i << "th Initializing... " << endl;
    }

    // worker의 sleeping/awake/queued 상태를 30초마다 출력하는 관찰용 스레드.
    this->monitor_thread = std::thread(&ThreadPoolEchoServer::Monitor_Workers, this);

    // 6) accept 스레드는 클라이언트를 직접 처리하지 않고 fd를 작업 큐에 넣는 생산자 역할만 한다.
    while(!this->Is_Stopping()){
        pollfd listen_event{};
        listen_event.fd = this->fd;
        listen_event.events = POLLIN;

        int poll_result = poll(&listen_event, 1, kAcceptPollTimeoutMs);
        if(poll_result < 0){
            if(errno == EINTR){
                continue;
            }
            perror("poll_result < 0");
            break;
        }

        // timeout마다 stopping을 다시 확인해 플랫폼별 listen shutdown 동작에 의존하지 않는다.
        if(poll_result == 0){
            continue;
        }

        if((listen_event.revents & POLLIN) == 0){
            if(this->Is_Stopping()){
                break;
            }
            continue;
        }

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        
        cout << "ThreadPoolEchoServer Thread Accepting... " << endl;
        int accept_result = accept(this->fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

        if(accept_result < 0){
            if(this->Is_Stopping()){
                break;
            }

            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR){
                continue;
            }
            
            perror("accept_result < 0");
            continue;
        }

        // 일부 플랫폼에서는 listen fd의 O_NONBLOCK이 accept된 fd에도 이어질 수 있다.
        // v3 worker는 blocking recv 구조를 학습하므로 client fd만 다시 blocking으로 되돌린다.
        int client_flags = fcntl(accept_result, F_GETFL, 0);
        if(client_flags < 0 || fcntl(accept_result, F_SETFL, client_flags & ~O_NONBLOCK) < 0){
            perror("fcntl client blocking");
            close(accept_result);
            continue;
        }

        // Stop()과 accept() 성공이 겹치면 새 fd를 큐에 넣지 않고 즉시 정리한다.
        if(this->Is_Stopping()){
            close(accept_result);
            break;
        }

        // 큐에서 대기 중인 연결도 종료 시 추적할 수 있도록 accept 직후 활성 목록에 등록한다.
        {
            std::lock_guard<std::mutex> lock(this->active_clients_mutex);
            this->active_clients.insert(accept_result);
            cout << "[Active Clients] Client Joined fd=" << accept_result
                 << ", count=" << this->active_clients.size() << endl;
        }

        if(!this->Enqueue_Client(accept_result)){
            cout << "ThreadPoolEchoServer Queue Full. Rejecting Client: " << accept_result << endl;
            this->Close_Client(accept_result);
        }
    }

    cout << "ThreadPoolEchoServer Stopping... " << endl;
}

void ThreadPoolEchoServer::Worker_Loop(){
    // worker 하나가 여러 client 작업에 재사용되도록 서버 수명 동안 반복한다.
    while(true){

        cout << "ThreadPoolEchoServer Thread Lock worker... " << endl;
        std::unique_lock<std::mutex> worker_lock(this->queue_mutex);

        // 아래 wait에서 잠들기 직전부터 반환 직후까지를 sleeping 상태로 집계한다.
        this->sleeping_worker_count++;
        
        cout << "ThreadPoolEchoServer Thread Wait... " << endl;
        // wait는 큐가 비어 있는 동안 mutex를 풀고 worker를 재운다.
        // 작업 또는 종료 요청으로 깨어날 때 mutex를 다시 획득한 뒤 predicate를 재확인한다.
        this->queue_condition.wait(
            worker_lock,
            [this]{return this->stopping || !this->client_queue.empty();}
        );

        this->sleeping_worker_count--;
        if(this->stopping && this->client_queue.empty())break;        

        // queue_mutex를 보유한 상태에서 한 worker만 front/pop을 수행한다.
        cout << "ThreadPoolEchoServer Thread Start Pop... " << endl;
        int client_fd = this->client_queue.front();
        this->client_queue.pop();

        // Process_Client()는 연결 종료까지 오래 blocking될 수 있으므로 큐 잠금을 먼저 해제한다.
        worker_lock.unlock();
        
        this->Process_Client(client_fd);
    }
        
}

bool ThreadPoolEchoServer::Enqueue_Client(int client_fd){
    cout << "ThreadPoolEchoServer Enqueue_Client: " << client_fd << endl;
    {
        std::lock_guard<std::mutex> lock(this->queue_mutex);
        if(this->stopping || this->client_queue.size() >= kMaxQueueSize){
            return false;
        }
        this->client_queue.push(client_fd);
    }

    // 새 작업은 worker 하나만 필요하므로 대기 중인 스레드 하나를 깨운다.
    this->queue_condition.notify_one();
    return true;
}

void ThreadPoolEchoServer::Process_Client(int client_fd){
    // worker별 스택에 생성되므로 다른 worker의 buffer와 공유되지 않는다.
    char buffer[4096];

    // 이 blocking 루프가 연결 수명 동안 worker 하나를 점유하는 것이 v3의 의도적인 한계다.
    while(true){
        cout << "ThreadPoolEchoServer Thread Process_Client Receiving: " << client_fd << ": " << sizeof(buffer) << endl;
        ssize_t recv_result = recv(client_fd, buffer, sizeof(buffer), 0);
        
        if(recv_result < 0){
            if(errno == EINTR){
                continue;
            }
            perror("recv_result < 0");
            this->Close_Client(client_fd);
            break;
        }
        else if(recv_result == 0){
            cout << "Client " << client_fd << " is Leaving... " << endl;
            this->Close_Client(client_fd);
            break;
        }

        std::string s(buffer, recv_result);

        cout << "ThreadPoolEchoServer Thread Process_Client Sending " << client_fd << " -> " << s << endl;
        // 이전 단일 전송 구현(비교용): send(client_fd, buffer, recv_result, 0);
        // 현재는 partial send에 대비해 Send_All()이 남은 범위를 반복 전송한다.
        bool send_flag = this->Send_All(client_fd, buffer, recv_result);
        if(!send_flag){
            cout << "ThreadPoolEchoServer Sending Client Buffer Failed: " << client_fd << endl;
            this->Close_Client(client_fd);
            break;
        }
    }
}

bool ThreadPoolEchoServer::Send_All(int client_fd, const char* data, std::size_t length){
    // 지금까지 실제로 전송한 바이트 수. length에 도달하면 전체 전송이 완료된 것이다.
    std::size_t total_sent = 0;

    while(total_sent < length){
        // partial send 이후에는 이미 보낸 범위를 건너뛰고 남은 주소와 길이만 다시 전달한다.
        const char* start_data = data + total_sent;
        std::size_t send_len = length - total_sent;

        cout << "ThreadPoolEchoServer Sending: " << client_fd << " -> ";
        cout.write(start_data, send_len);
        cout << endl;

        ssize_t send_result = send(client_fd, start_data, send_len, 0);
        if(send_result > 0){
            total_sent += send_result;
        }
        else if(send_result < 0 && errno == EINTR){
            // 시그널로 중단된 경우에는 전송량을 변경하지 않고 같은 남은 범위를 다시 시도한다.
            continue;
        }
        else{
            perror("send_result <= 0");
            return false;
        }
    }

    return true;
}

void ThreadPoolEchoServer::Close_Client(int client_fd){
    // erase와 close를 같은 임계 영역에서 처리해 fd 번호가 재사용되는 사이
    // 이전 worker가 새 연결의 active_clients 기록을 지우는 race를 막는다.
    std::lock_guard<std::mutex> lock(this->active_clients_mutex);
    this->active_clients.erase(client_fd);
    cout << "[Active Clients] Client Leaving fd=" << client_fd
         << ", count=" << this->active_clients.size() << endl;
    close(client_fd);
}

void ThreadPoolEchoServer::Monitor_Workers(){
    while(true){
        std::size_t queue_size = 0;

        // wait_for는 timeout 동안 mutex를 풀며 Stop()의 notify_all()에 즉시 반응한다.
        {
            std::unique_lock<std::mutex> lock(this->queue_mutex);
            bool monitor_stop = this->queue_condition.wait_for(
                lock,
                std::chrono::seconds(30),
                [this]{return this->stopping;}
            );

            if(monitor_stop){
                break;
            }

            queue_size = this->client_queue.size();
        }

        int sleeping = this->sleeping_worker_count.load();
        int awake = numThreads - sleeping;

        cout << "[Worker Status] Total: " << numThreads << ", Sleeping: " << sleeping << ", Awake: " << awake << ", Queued: " << queue_size << endl;
    }
}

bool ThreadPoolEchoServer::Is_Stopping(){
    std::lock_guard<std::mutex> lock(this->queue_mutex);
    return this->stopping;
}

void ThreadPoolEchoServer::Stop(){
    int listen_fd = -1;
    {
        std::lock_guard<std::mutex> lock(this->queue_mutex);
        if(this->stopping){
            return;
        }

        this->stopping = true;
        listen_fd = this->fd;
    }

    if(listen_fd >= 0){
        shutdown(listen_fd, SHUT_RDWR);
    }

    {
        std::lock_guard<std::mutex> client_lock(this->active_clients_mutex);
        for(int client_fd : this->active_clients){
            shutdown(client_fd, SHUT_RDWR);
        }
    }

    this->queue_condition.notify_all();
}
