#pragma once

#include <packet.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

struct ClientSession;

struct GameJob{
    enum class JobType{
        Enter,   // 세션이 accept되어 방에 들어옴
        Packet,  // 조립이 끝난 클라이언트 패킷
        Leave,   // 세션이 닫힘
    };

    JobType type = JobType::Packet;
    std::shared_ptr<ClientSession> session;
    protocol::Packet packet;
};

// completion worker(생산자)와 game logic 스레드(소비자)를 잇는 bounded 큐.
// v3의 queue + mutex + condition_variable 패턴을 그대로 사용한다.
class GameJobQueue{
    private:
        std::deque<GameJob> jobs;
        std::mutex queue_mutex;
        std::condition_variable queue_condition;
        std::size_t capacity;
        std::size_t peak_size = 0;
        std::uint64_t pushed_count = 0;
        bool closed = false;

    public:
        explicit GameJobQueue(std::size_t capacity);

        // 큐가 닫혔거나 가득 찼으면 false를 반환한다. 생산자는 completion worker이므로
        // 큐가 빌 때까지 기다리지 않는다. IOCP worker가 game 로직 때문에 멈추면 안 되기 때문이다.
        bool Push(GameJob job);
        // 큐가 빌 때는 잠들어 기다린다. 닫혔고 남은 job도 없을 때만 false를 반환한다.
        bool Pop(GameJob& out);
        // 이후 Push를 막고 대기 중인 소비자를 깨운다. 이미 들어간 job은 Pop으로 계속 꺼낼 수 있다.
        void Close();

        std::size_t Size();
        std::size_t Peak_Size();
        std::uint64_t Pushed_Count();
};
