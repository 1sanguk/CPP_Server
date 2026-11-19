#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>

// 로그 출력을 전용 스레드로 분리한 로거.
// 서버 스레드는 문자열을 큐에 넣기만 하고, 실제 출력(콘솔/파이프)은 writer 스레드가 한다.
// stdout이 막혀도 completion worker와 logic 스레드는 출력 때문에 멈추지 않는다.
// 큐가 상한을 넘으면 새 로그는 버리고 개수만 센다(서버를 멈추는 것보다 로그를 잃는 쪽을 택한다).
class AsyncLogger{
    private:
        std::ostream& out;
        const std::size_t max_pending;
        std::deque<std::string> pending;
        std::mutex queue_mutex;
        std::condition_variable queue_condition;
        std::thread writer;
        std::uint64_t dropped_count = 0;
        bool stopping = false;

        void Writer_Loop();

    public:
        explicit AsyncLogger(std::ostream& out, std::size_t max_pending = 100000);
        ~AsyncLogger();
        AsyncLogger(const AsyncLogger&) = delete;
        AsyncLogger& operator=(const AsyncLogger&) = delete;

        // writer 스레드를 시작한다. 시작 전에 쓴 로그는 큐에 남아 있다가 시작 후 출력된다.
        void Start();
        // 블로킹 없이 큐에 넣는다. 큐가 가득 차면 버리고 false를 반환한다.
        bool Write(std::string line);
        // 남은 로그를 모두 출력한 뒤 writer 스레드를 join한다. 여러 번 호출해도 안전하다.
        void Stop();
        std::uint64_t Dropped_Count();
};
