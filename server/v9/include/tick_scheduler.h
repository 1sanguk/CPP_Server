#pragma once

#include <chrono>
#include <cstdint>

// 고정 주기 틱의 시작 시각을 계산한다. 시계를 직접 읽지 않고 호출자가 현재 시각을 넘기므로
// 단위 테스트에서 가짜 시각으로 늦은 틱과 건너뛴 틱을 재현할 수 있다.
//
// 늦은 틱 정책: 건너뛰기
// - 처리가 주기보다 오래 걸려 시작 시각을 여러 번 놓쳤다면, 놓친 틱을 연달아 돌려 따라잡지 않고 건너뛴다.
//   따라잡기는 이미 느린 서버에 일을 더 몰아줘 지연이 계속 커질 수 있기 때문이다.
// - 대신 Begin_Tick이 이번 틱이 대신하는 주기 수를 돌려주므로, 호출자는 dt = 주기 × 그 수로 월드를 움직여
//   이동 속도가 실제 시간 기준으로 유지되게 한다.
class TickScheduler{
    public:
        using Clock = std::chrono::steady_clock;

        TickScheduler(Clock::duration period, Clock::time_point start);

        // 다음 틱을 시작해야 하는 시각
        Clock::time_point Next_Deadline() const;
        // 틱을 시작할 때 호출한다. 이번 틱이 대신하는 주기 수(1 이상)를 반환하고 다음 시작 시각을 정한다.
        std::uint64_t Begin_Tick(Clock::time_point now);
        // 틱 처리를 마쳤을 때 호출한다. 다음 시작 시각을 넘겼으면 늦은 틱으로 세고 true를 반환한다.
        bool End_Tick(Clock::time_point now);

        Clock::duration Period() const;
        std::uint64_t Tick_Count() const;
        std::uint64_t Late_Count() const;
        std::uint64_t Skipped_Count() const;

    private:
        Clock::duration period;
        Clock::time_point deadline;
        std::uint64_t tick_count = 0;
        std::uint64_t late_count = 0;
        std::uint64_t skipped_count = 0;
};
