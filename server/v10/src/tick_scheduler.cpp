#include <tick_scheduler.h>

TickScheduler::TickScheduler(Clock::duration period_value, Clock::time_point start)
    : period(period_value.count() > 0 ? period_value : Clock::duration{1}), deadline(start + period){}

TickScheduler::Clock::time_point TickScheduler::Next_Deadline() const{
    return this->deadline;
}

std::uint64_t TickScheduler::Begin_Tick(Clock::time_point now){
    std::uint64_t periods = 1;
    if(now > this->deadline){
        // 시작 시각을 주기 단위로 몇 번 넘겼는지 센다. 넘긴 만큼은 따로 돌리지 않고 건너뛴다.
        const auto missed = static_cast<std::uint64_t>((now - this->deadline) / this->period);
        periods += missed;
        this->skipped_count += missed;
    }
    this->deadline += this->period * static_cast<Clock::duration::rep>(periods);
    ++this->tick_count;
    return periods;
}

bool TickScheduler::End_Tick(Clock::time_point now){
    if(now > this->deadline){
        ++this->late_count;
        return true;
    }
    return false;
}

TickScheduler::Clock::duration TickScheduler::Period() const{
    return this->period;
}

std::uint64_t TickScheduler::Tick_Count() const{
    return this->tick_count;
}

std::uint64_t TickScheduler::Late_Count() const{
    return this->late_count;
}

std::uint64_t TickScheduler::Skipped_Count() const{
    return this->skipped_count;
}
