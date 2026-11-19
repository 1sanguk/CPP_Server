#include <game_job_queue.h>

#include <algorithm>

GameJobQueue::GameJobQueue(std::size_t capacity) : capacity(capacity){}

bool GameJobQueue::Push(GameJob job){
    {
        std::lock_guard<std::mutex> lock(this->queue_mutex);
        if(this->closed || this->jobs.size() >= this->capacity){
            return false;
        }
        this->jobs.emplace_back(std::move(job));
        ++this->pushed_count;
        this->peak_size = (std::max)(this->peak_size, this->jobs.size());
    }
    // 잠금을 푼 뒤 깨워서, 깨어난 소비자가 곧바로 mutex에서 다시 막히지 않게 한다.
    this->queue_condition.notify_one();
    return true;
}

bool GameJobQueue::Pop(GameJob& out){
    std::unique_lock<std::mutex> lock(this->queue_mutex);
    this->queue_condition.wait(lock, [this]{
        return this->closed || !this->jobs.empty();
    });
    if(this->jobs.empty()){
        return false;
    }
    out = std::move(this->jobs.front());
    this->jobs.pop_front();
    return true;
}

void GameJobQueue::Close(){
    {
        std::lock_guard<std::mutex> lock(this->queue_mutex);
        this->closed = true;
    }
    this->queue_condition.notify_all();
}

std::size_t GameJobQueue::Size(){
    std::lock_guard<std::mutex> lock(this->queue_mutex);
    return this->jobs.size();
}

std::size_t GameJobQueue::Peak_Size(){
    std::lock_guard<std::mutex> lock(this->queue_mutex);
    return this->peak_size;
}

std::uint64_t GameJobQueue::Pushed_Count(){
    std::lock_guard<std::mutex> lock(this->queue_mutex);
    return this->pushed_count;
}
