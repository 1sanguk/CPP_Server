#pragma once

#include <packet.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

struct ClientSession;

struct GameJob{
    enum class JobType{
        Enter,   // 세션이 accept되어 월드에 들어옴
        Packet,  // 조립이 끝난 클라이언트 패킷
        Leave,   // 세션이 닫힘
    };

    JobType type = JobType::Packet;
    std::shared_ptr<ClientSession> session;
    protocol::Packet packet;
};

// completion worker(생산자)와 틱 루프(소비자)를 잇는 bounded 큐.
//
// v7과 다른 점
// - 소비자는 job 하나마다 깨어나지 않는다. 틱 시작 시각까지 자고 있다가 쌓인 job을 한 번에 가져간다(Drain_Until).
//   그래서 Push는 소비자를 깨우지 않고, 잠금은 틱마다 한 번만 소비자와 겹친다.
// - 생산자는 한 번의 Recv에서 나온 패킷들을 Push_Batch로 한 번에 넣는다. 패킷마다 잠그던 비용을 Recv당 한 번으로 줄인다.
class GameJobQueue{
    private:
        std::deque<GameJob> jobs;
        std::mutex queue_mutex;
        std::condition_variable queue_condition;
        std::size_t capacity;
        std::size_t peak_size = 0;
        std::uint64_t pushed_count = 0;
        std::uint64_t push_call_count = 0;
        bool closed = false;

    public:
        explicit GameJobQueue(std::size_t capacity);

        // 큐가 닫혔거나 가득 찼으면 false를 반환한다. IOCP worker가 game 로직 때문에 멈추면 안 되므로 기다리지 않는다.
        bool Push(GameJob job);
        // jobs 전체를 한 번의 잠금으로 넣는다. 다 들어갈 자리가 없으면 하나도 넣지 않고 false를 반환한다.
        // 성공하면 jobs는 비워진다.
        bool Push_Batch(std::vector<GameJob>& jobs);
        // deadline까지 기다린 뒤 쌓인 job을 모두 out으로 옮긴다(out의 기존 내용은 지운다).
        // 큐가 닫히면 기다리지 않고 바로 false를 반환한다.
        bool Drain_Until(std::deque<GameJob>& out, std::chrono::steady_clock::time_point deadline);
        // 이후 Push를 막고 기다리는 소비자를 깨운다.
        void Close();

        std::size_t Size();
        std::size_t Peak_Size();
        std::uint64_t Pushed_Count();
        // Push/Push_Batch 성공 호출 수. Pushed_Count와 비교하면 잠금 한 번에 평균 몇 개를 넣었는지 알 수 있다.
        std::uint64_t Push_Call_Count();
};
