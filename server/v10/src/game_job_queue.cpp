#include <game_job_queue.h>

#include <algorithm>

GameJobQueue::GameJobQueue(std::size_t capacity) : capacity(capacity){}

bool GameJobQueue::Push(GameJob job){
    std::lock_guard<std::mutex> lock(this->queue_mutex);
    if(this->closed || this->jobs.size() >= this->capacity){
        return false;
    }
    this->jobs.emplace_back(std::move(job));
    ++this->pushed_count;
    ++this->push_call_count;
    this->peak_size = (std::max)(this->peak_size, this->jobs.size());
    this->window_peak_size = (std::max)(this->window_peak_size, this->jobs.size());
    // 소비자를 깨우지 않는다. 틱 루프는 정해진 시각에 스스로 깨어나 한꺼번에 가져간다.
    return true;
}

bool GameJobQueue::Push_Batch(std::vector<GameJob>& batch){
    if(batch.empty()){
        return true;
    }
    std::lock_guard<std::mutex> lock(this->queue_mutex);
    if(this->closed || this->jobs.size() + batch.size() > this->capacity){
        return false;
    }
    for(GameJob& job : batch){
        this->jobs.emplace_back(std::move(job));
    }
    this->pushed_count += batch.size();
    ++this->push_call_count;
    this->peak_size = (std::max)(this->peak_size, this->jobs.size());
    this->window_peak_size = (std::max)(this->window_peak_size, this->jobs.size());
    batch.clear();
    return true;
}

bool GameJobQueue::Drain_Until(std::deque<GameJob>& out, std::chrono::steady_clock::time_point deadline){
    out.clear();
    std::unique_lock<std::mutex> lock(this->queue_mutex);
    // job이 와도 깨어나지 않는다. 종료(Close)만 기다림을 끊는다.
    this->queue_condition.wait_until(lock, deadline, [this]{ return this->closed; });
    if(this->closed){
        return false;
    }
    // swap은 deque의 내부 블록만 맞바꾸므로 job 수와 관계없이 잠금 구간이 짧다.
    // 소비자가 비운 out을 넘겨주므로 생산자는 그 메모리를 다음 틱에 다시 쓴다.
    out.swap(this->jobs);
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

std::uint64_t GameJobQueue::Push_Call_Count(){
    std::lock_guard<std::mutex> lock(this->queue_mutex);
    return this->push_call_count;
}

std::size_t GameJobQueue::Take_Window_Peak(){
    std::lock_guard<std::mutex> lock(this->queue_mutex);
    const std::size_t peak = this->window_peak_size;
    this->window_peak_size = this->jobs.size();
    return peak;
}
