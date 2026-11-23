#include <async_logger.h>

AsyncLogger::AsyncLogger(std::ostream& out, std::size_t max_pending) : out(out), max_pending(max_pending){}

AsyncLogger::~AsyncLogger(){
    this->Stop();
}

void AsyncLogger::Start(){
    std::lock_guard<std::mutex> lock(this->queue_mutex);
    if(this->writer.joinable() || this->stopping){
        return;
    }
    this->writer = std::thread(&AsyncLogger::Writer_Loop, this);
}

bool AsyncLogger::Write(std::string line){
    {
        std::lock_guard<std::mutex> lock(this->queue_mutex);
        if(this->pending.size() >= this->max_pending){
            ++this->dropped_count;
            return false;
        }
        this->pending.emplace_back(std::move(line));
    }
    this->queue_condition.notify_one();
    return true;
}

void AsyncLogger::Writer_Loop(){
    std::deque<std::string> batch;
    while(true){
        {
            std::unique_lock<std::mutex> lock(this->queue_mutex);
            this->queue_condition.wait(lock, [this]{
                return this->stopping || !this->pending.empty();
            });
            if(this->pending.empty() && this->stopping){
                break;
            }
            // 큐를 통째로 가져와 잠금 밖에서 출력한다. 출력이 느려도 Write()는 기다리지 않는다.
            batch.swap(this->pending);
        }
        for(const std::string& line : batch){
            this->out << line << '\n';
        }
        this->out.flush();
        batch.clear();
    }
}

void AsyncLogger::Stop(){
    bool start_and_stop = false;
    {
        std::lock_guard<std::mutex> lock(this->queue_mutex);
        if(this->stopping && !this->writer.joinable()){
            return;
        }
        this->stopping = true;
        // 한 번도 시작하지 않았더라도 쌓인 로그는 출력하고 끝낸다.
        start_and_stop = !this->writer.joinable() && !this->pending.empty();
        if(start_and_stop){
            this->writer = std::thread(&AsyncLogger::Writer_Loop, this);
        }
    }
    this->queue_condition.notify_all();
    if(this->writer.joinable()){
        this->writer.join();
    }

    std::uint64_t dropped = this->Dropped_Count();
    if(dropped != 0){
        this->out << "[AsyncLogger] dropped " << dropped << " log line(s) because the queue was full\n";
        this->out.flush();
    }
}

std::uint64_t AsyncLogger::Dropped_Count(){
    std::lock_guard<std::mutex> lock(this->queue_mutex);
    return this->dropped_count;
}
