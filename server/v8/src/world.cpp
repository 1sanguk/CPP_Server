#include <world.h>

#include <algorithm>
#include <cmath>

CellCoord Cell_Of(float x, float y, float cell_size, int columns){
    const auto to_index = [cell_size, columns](float value){
        const int index = static_cast<int>(std::floor(value / cell_size));
        return std::clamp(index, 0, columns - 1);
    };
    return CellCoord{to_index(x), to_index(y)};
}

bool Cells_Adjacent(CellCoord a, CellCoord b){
    return std::abs(a.x - b.x) <= 1 && std::abs(a.y - b.y) <= 1;
}

World::World(const Config& config_value) : config(config_value), random(config_value.seed){
    if(!(this->config.world_size > 0.0f)){
        this->config.world_size = 1000.0f;
    }
    if(!(this->config.cell_size > 0.0f)){
        this->config.cell_size = this->config.world_size;
    }
    this->columns = (std::max)(1, static_cast<int>(std::ceil(this->config.world_size / this->config.cell_size)));
    this->cells.resize(static_cast<std::size_t>(this->columns) * static_cast<std::size_t>(this->columns));
    this->cell_moves.resize(this->cells.size());
}

std::size_t World::Cell_Index(CellCoord cell) const{
    return static_cast<std::size_t>(cell.y) * static_cast<std::size_t>(this->columns) + static_cast<std::size_t>(cell.x);
}

float World::Clamp_Coordinate(float value) const{
    return std::clamp(value, 0.0f, this->config.world_size);
}

template<typename Visitor>
void World::For_Each_Nearby_Cell(CellCoord cell, Visitor&& visitor) const{
    for(int y = cell.y - 1; y <= cell.y + 1; ++y){
        if(y < 0 || y >= this->columns) continue;
        for(int x = cell.x - 1; x <= cell.x + 1; ++x){
            if(x < 0 || x >= this->columns) continue;
            visitor(this->Cell_Index(CellCoord{x, y}));
        }
    }
}

template<typename Visitor>
void World::For_Each_Nearby(CellCoord cell, Visitor&& visitor) const{
    this->For_Each_Nearby_Cell(cell, [&](std::size_t cell_index){
        for(std::size_t slot : this->cells[cell_index]){
            visitor(slot);
        }
    });
}

void World::Record_Visibility(std::uint32_t observer, std::uint32_t target, int delta){
    int& net = this->visibility_changes[observer][target];
    net += delta;
}

void World::Remove_From_Cell(std::size_t slot){
    auto& members = this->cells[this->Cell_Index(this->players[slot].cell)];
    auto found = std::find(members.begin(), members.end(), slot);
    if(found != members.end()){
        // 셀 안의 순서는 의미가 없으므로 마지막 원소와 바꿔 지워 앞쪽 원소를 옮기지 않는다.
        *found = members.back();
        members.pop_back();
    }
}

std::optional<protocol::EntityState> World::Enter(std::uint32_t id){
    std::uniform_real_distribution<float> position(0.0f, this->config.world_size);
    const float x = position(this->random);
    const float y = position(this->random);
    return this->Enter_At(id, x, y);
}

std::optional<protocol::EntityState> World::Enter_At(std::uint32_t id, float x, float y){
    if(this->slot_of.contains(id)){
        return std::nullopt;
    }
    Player player;
    player.id = id;
    player.x = this->Clamp_Coordinate(std::isfinite(x) ? x : 0.0f);
    player.y = this->Clamp_Coordinate(std::isfinite(y) ? y : 0.0f);
    player.cell = Cell_Of(player.x, player.y, this->config.cell_size, this->columns);

    const std::size_t slot = this->players.size();
    // 새 플레이어와 주변 플레이어가 서로를 보게 된다.
    this->For_Each_Nearby(player.cell, [this, id](std::size_t other_slot){
        const std::uint32_t other_id = this->players[other_slot].id;
        this->Record_Visibility(other_id, id, +1);
        this->Record_Visibility(id, other_id, +1);
    });
    this->players.push_back(player);
    this->slot_of.emplace(id, slot);
    this->cells[this->Cell_Index(player.cell)].push_back(slot);
    return protocol::EntityState{id, player.x, player.y};
}

bool World::Leave(std::uint32_t id){
    auto found = this->slot_of.find(id);
    if(found == this->slot_of.end()){
        return false;
    }
    const std::size_t slot = found->second;
    this->Remove_From_Cell(slot);
    // 남은 주변 플레이어에게서 사라진다. 같은 틱에 들어왔던 대상이면 +1과 상쇄되어 아무것도 보내지 않는다.
    this->For_Each_Nearby(this->players[slot].cell, [this, id](std::size_t other_slot){
        this->Record_Visibility(this->players[other_slot].id, id, -1);
    });
    this->visibility_changes.erase(id);
    this->slot_of.erase(found);

    // 배열 중간을 비우지 않도록 마지막 플레이어를 빈 자리로 옮기고, 그 플레이어가 있던 셀의 slot 번호도 고친다.
    const std::size_t last = this->players.size() - 1;
    if(slot != last){
        this->players[slot] = this->players[last];
        auto& members = this->cells[this->Cell_Index(this->players[slot].cell)];
        std::replace(members.begin(), members.end(), last, slot);
        this->slot_of[this->players[slot].id] = slot;
    }
    this->players.pop_back();
    return true;
}

bool World::Set_Velocity(std::uint32_t id, float velocity_x, float velocity_y, std::uint32_t input_seq){
    auto found = this->slot_of.find(id);
    if(found == this->slot_of.end()){
        return false;
    }
    if(!std::isfinite(velocity_x) || !std::isfinite(velocity_y)){
        velocity_x = 0.0f;
        velocity_y = 0.0f;
    }
    const float speed = std::hypot(velocity_x, velocity_y);
    if(speed > this->config.max_speed){
        const float scale = this->config.max_speed / speed;
        velocity_x *= scale;
        velocity_y *= scale;
    }
    Player& player = this->players[found->second];
    player.velocity_x = velocity_x;
    player.velocity_y = velocity_y;
    player.last_input_seq = input_seq;
    return true;
}

void World::Step(float dt_seconds){
    if(!(dt_seconds > 0.0f)){
        return;
    }
    for(std::size_t slot = 0; slot < this->players.size(); ++slot){
        Player& player = this->players[slot];
        if(player.velocity_x == 0.0f && player.velocity_y == 0.0f){
            continue;
        }
        const float next_x = this->Clamp_Coordinate(player.x + player.velocity_x * dt_seconds);
        const float next_y = this->Clamp_Coordinate(player.y + player.velocity_y * dt_seconds);
        if(next_x == player.x && next_y == player.y){
            continue;   // 벽에 막혀 제자리
        }
        player.x = next_x;
        player.y = next_y;
        player.moved = true;

        const CellCoord old_cell = player.cell;
        const CellCoord new_cell = Cell_Of(next_x, next_y, this->config.cell_size, this->columns);
        if(new_cell == old_cell){
            continue;
        }

        // 셀이 바뀐 경우에만 시야 집합을 다시 계산한다.
        // 예전 3 × 3에만 있던 셀의 플레이어와는 서로 안 보이게 되고, 새 3 × 3에만 있는 셀의 플레이어와는 서로 보이게 된다.
        this->Remove_From_Cell(slot);
        const std::uint32_t id = player.id;
        this->For_Each_Nearby(old_cell, [&](std::size_t other_slot){
            if(Cells_Adjacent(this->players[other_slot].cell, new_cell)) return;
            const std::uint32_t other_id = this->players[other_slot].id;
            this->Record_Visibility(other_id, id, -1);
            this->Record_Visibility(id, other_id, -1);
        });
        this->For_Each_Nearby(new_cell, [&](std::size_t other_slot){
            if(Cells_Adjacent(this->players[other_slot].cell, old_cell)) return;
            const std::uint32_t other_id = this->players[other_slot].id;
            this->Record_Visibility(other_id, id, +1);
            this->Record_Visibility(id, other_id, +1);
        });
        // For_Each_Nearby 안에서 players를 읽기만 했으므로 player 참조는 여전히 유효하다.
        player.cell = new_cell;
        this->cells[this->Cell_Index(new_cell)].push_back(slot);
    }
}

std::span<const ObserverUpdate> World::Collect_Updates(){
    // 1) 셀마다 이번 틱에 움직인 플레이어 상태를 한 번만 모은다. 보완 전에는 관찰자마다 주변 플레이어를 하나씩
    //    훑으며 moved를 확인했다. 이제 관찰자는 주변 9개 셀의 목록을 이어 붙이기만 한다.
    for(auto& moves : this->cell_moves){
        moves.clear();
    }
    for(const Player& player : this->players){
        if(player.moved){
            this->cell_moves[this->Cell_Index(player.cell)].push_back(protocol::EntityState{player.id, player.x, player.y});
        }
    }

    std::size_t count = 0;
    for(const Player& observer : this->players){
        // 2) 관찰자별 결과는 재사용 버퍼에 쓴다. clear()는 vector 용량을 남겨 두므로 틱마다 새로 할당하지 않는다.
        if(count == this->update_pool.size()){
            this->update_pool.emplace_back();
        }
        ObserverUpdate& update = this->update_pool[count];
        update.observer_id = observer.id;
        update.despawns.clear();
        update.spawns.clear();
        update.moves.clear();
        update.has_self = observer.moved || observer.last_input_seq != observer.acked_input_seq;
        update.self = protocol::SelfState{observer.last_input_seq, observer.x, observer.y};

        const std::unordered_map<std::uint32_t, int>* changes = nullptr;
        if(auto found = this->visibility_changes.find(observer.id); found != this->visibility_changes.end()){
            changes = &found->second;
            for(const auto& [target_id, net] : found->second){
                if(net < 0){
                    update.despawns.push_back(target_id);
                }else if(net > 0){
                    // 진입 후 같은 틱에 나갔다면 Leave의 -1과 상쇄되어 여기 오지 않는다. 그래도 방어적으로 확인한다.
                    auto target_slot = this->slot_of.find(target_id);
                    if(target_slot == this->slot_of.end()) continue;
                    const Player& target = this->players[target_slot->second];
                    update.spawns.push_back(protocol::EntityState{target.id, target.x, target.y});
                }
            }
        }

        // 3) 이번 틱에 움직인 주변 플레이어의 최종 위치. 자기 자신은 S_SelfState로 따로 보내고,
        //    이번 틱에 새로 보인 대상은 spawn에 이미 위치가 있으므로 뺀다.
        this->For_Each_Nearby_Cell(observer.cell, [&](std::size_t cell_index){
            for(const protocol::EntityState& state : this->cell_moves[cell_index]){
                if(state.id == observer.id) continue;
                if(changes != nullptr){
                    auto change = changes->find(state.id);
                    if(change != changes->end() && change->second > 0) continue;
                }
                update.moves.push_back(state);
            }
        });

        if(update.has_self || !update.despawns.empty() || !update.spawns.empty() || !update.moves.empty()){
            ++count;
        }
    }
    for(Player& player : this->players){
        player.moved = false;
        player.acked_input_seq = player.last_input_seq;
    }
    this->visibility_changes.clear();
    return std::span<const ObserverUpdate>(this->update_pool.data(), count);
}

std::optional<protocol::EntityState> World::Find(std::uint32_t id) const{
    auto found = this->slot_of.find(id);
    if(found == this->slot_of.end()){
        return std::nullopt;
    }
    const Player& player = this->players[found->second];
    return protocol::EntityState{player.id, player.x, player.y};
}

std::vector<std::uint32_t> World::Visible_Ids(std::uint32_t id) const{
    std::vector<std::uint32_t> result;
    auto found = this->slot_of.find(id);
    if(found == this->slot_of.end()){
        return result;
    }
    const std::size_t self = found->second;
    this->For_Each_Nearby(this->players[self].cell, [&](std::size_t slot){
        if(slot != self) result.push_back(this->players[slot].id);
    });
    std::sort(result.begin(), result.end());
    return result;
}

std::size_t World::Player_Count() const{
    return this->players.size();
}

std::size_t World::Max_Cell_Population() const{
    std::size_t result = 0;
    for(const auto& members : this->cells){
        result = (std::max)(result, members.size());
    }
    return result;
}

int World::Columns() const{
    return this->columns;
}

const World::Config& World::Get_Config() const{
    return this->config;
}
