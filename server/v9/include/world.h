#pragma once

#include <packet.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <unordered_map>
#include <vector>

// 소켓과 무관한 v8 게임 월드. logic 스레드 하나만 사용하므로 내부 잠금이 없다.
//
// 격자 AOI(Area of Interest)
// - 월드를 cell_size 크기의 정사각형 셀로 나누고, 셀마다 안에 있는 플레이어 목록을 둔다.
// - 두 플레이어는 셀 좌표 차이가 가로·세로 모두 1 이하(자기 셀 + 주변 8칸)일 때 서로 보인다.
//   이 관계는 대칭이므로 "A가 B를 본다"와 "B가 A를 본다"가 항상 같이 바뀐다.
// - 셀을 넘어갈 때만 시야 집합이 바뀌므로, 그때만 진입/이탈을 계산한다.
struct CellCoord{
    int x = 0;
    int y = 0;
    bool operator==(const CellCoord&) const = default;
};

// 좌표를 셀 좌표로 바꾼다. 월드 경계(world_size)에 딱 닿은 좌표도 마지막 셀에 넣는다.
CellCoord Cell_Of(float x, float y, float cell_size, int columns);
// 두 셀이 서로의 시야(3 × 3) 안에 있는지
bool Cells_Adjacent(CellCoord a, CellCoord b);

// 한 틱 동안 한 관찰자에게 보낼 변경 사항. 클라이언트는 despawn → spawn → move 순서로 적용한다.
struct ObserverUpdate{
    std::uint32_t observer_id = 0;
    // 내가 움직였거나 아직 응답하지 않은 입력이 있으면 내 위치와 마지막으로 반영한 입력 순번을 보낸다.
    bool has_self = false;
    protocol::SelfState self;
    std::vector<std::uint32_t> despawns;
    std::vector<protocol::EntityState> spawns;
    std::vector<protocol::EntityState> moves;
};

class World{
    public:
        struct Config{
            float world_size = 1000.0f;
            // 시야 반경과 같은 크기. world_size 이상으로 주면 셀이 하나뿐이라 AOI를 끈 것(전체 방송)과 같다.
            float cell_size = 100.0f;
            // 초당 최대 이동 거리. 클라이언트가 더 빠른 속도를 요청하면 방향은 유지하고 크기만 줄인다.
            float max_speed = 60.0f;
            std::uint32_t seed = 8;
        };

        explicit World(const Config& config);

        // 무작위 위치에 플레이어를 넣고 그 위치를 반환한다. 이미 있는 id면 nullopt.
        std::optional<protocol::EntityState> Enter(std::uint32_t id);
        // 테스트용: 정한 위치에 넣는다. 좌표는 월드 안으로 보정한다.
        std::optional<protocol::EntityState> Enter_At(std::uint32_t id, float x, float y);
        bool Leave(std::uint32_t id);
        // 이동 입력. 같은 틱에 여러 번 오면 마지막 값만 남는다(입력 합치기). 속도 상한과 NaN을 여기서 보정한다.
        // input_seq는 클라이언트가 붙인 입력 순번으로, 다음 S_SelfState에서 "여기까지 반영했다"고 돌려준다.
        bool Set_Velocity(std::uint32_t id, float velocity_x, float velocity_y, std::uint32_t input_seq = 0);
        // dt초만큼 모든 플레이어를 움직이고, 셀이 바뀐 플레이어의 시야 진입/이탈을 기록한다.
        void Step(float dt_seconds);
        // 지난 Collect 이후 쌓인 변경을 관찰자별로 꺼낸다. 변경이 없는 관찰자는 넣지 않는다.
        // 반환한 범위는 다음 Collect_Updates 호출 전까지만 유효하다(내부 버퍼를 틱마다 재사용한다).
        std::span<const ObserverUpdate> Collect_Updates();

        std::optional<protocol::EntityState> Find(std::uint32_t id) const;
        // id의 시야 안에 있는 다른 플레이어 id(자기 자신 제외)
        std::vector<std::uint32_t> Visible_Ids(std::uint32_t id) const;
        std::size_t Player_Count() const;
        std::size_t Max_Cell_Population() const;
        int Columns() const;
        const Config& Get_Config() const;

    private:
        struct Player{
            std::uint32_t id = 0;
            float x = 0.0f;
            float y = 0.0f;
            float velocity_x = 0.0f;
            float velocity_y = 0.0f;
            CellCoord cell;
            bool moved = false;
            std::uint32_t last_input_seq = 0;
            std::uint32_t acked_input_seq = 0;
        };

        Config config;
        int columns = 1;
        std::mt19937 random;
        // 플레이어는 연속 배열에 두고, 셀에는 배열 인덱스(slot)를 넣는다. 매 틱 시야를 훑을 때 해시 조회를 피하기 위해서다.
        std::vector<Player> players;
        std::unordered_map<std::uint32_t, std::size_t> slot_of;
        std::vector<std::vector<std::size_t>> cells;
        // 관찰자 id → (대상 id → 진입 +1 / 이탈 -1 누적). 같은 틱에 들어왔다 나가면 0이 되어 아무것도 보내지 않는다.
        std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, int>> visibility_changes;
        // Collect_Updates가 틱마다 재사용하는 버퍼. 셀마다 이번 틱에 움직인 플레이어 상태를 한 번만 모아 두고,
        // 관찰자는 주변 9개 셀의 목록을 이어 붙이기만 한다. vector 용량은 다음 틱에도 그대로 쓴다.
        std::vector<std::vector<protocol::EntityState>> cell_moves;
        std::vector<ObserverUpdate> update_pool;

        std::size_t Cell_Index(CellCoord cell) const;
        void Remove_From_Cell(std::size_t slot);
        void Record_Visibility(std::uint32_t observer, std::uint32_t target, int delta);
        // cell 주변 3 × 3 셀의 slot마다 visitor를 호출한다.
        template<typename Visitor>
        void For_Each_Nearby(CellCoord cell, Visitor&& visitor) const;
        // cell 주변 3 × 3 셀의 셀 인덱스마다 visitor를 호출한다.
        template<typename Visitor>
        void For_Each_Nearby_Cell(CellCoord cell, Visitor&& visitor) const;
        float Clamp_Coordinate(float value) const;
};
