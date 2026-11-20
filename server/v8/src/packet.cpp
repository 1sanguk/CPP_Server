#include <packet.h>

#include <algorithm>
#include <bit>
#include <cmath>

namespace protocol{

namespace{
void Write_U16(std::vector<char>& out, std::uint16_t value){
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void Write_U32(std::vector<char>& out, std::uint32_t value){
    for(int shift = 0; shift < 32; shift += 8){
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

// 미리 크기를 잡아 둔 위치에 바로 쓴다. 목록 패킷은 항목 수천 개를 바이트마다 push_back하면 느리다.
char* Put_U16(char* cursor, std::uint16_t value){
    cursor[0] = static_cast<char>(value & 0xFF);
    cursor[1] = static_cast<char>((value >> 8) & 0xFF);
    return cursor + 2;
}

char* Put_U32(char* cursor, std::uint32_t value){
    for(int i = 0; i < 4; ++i){
        cursor[i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    }
    return cursor + 4;
}

// 좌표 × (65535 / world_size)를 반올림한다. 나눗셈과 std::round는 항목마다 하기에 비싸므로, 배율은 목록마다
// 한 번 계산하고 반올림은 +0.5 후 정수 변환으로 한다. Quantize_Coordinate도 같은 식을 써서 결과가 항상 같다.
std::uint16_t Quantize_With_Scale(float value, float scale){
    const float scaled = value * scale;
    if(!(scaled > 0.0f)){
        return 0;   // 음수와 NaN
    }
    if(scaled >= 65535.0f){
        return 65535;
    }
    return static_cast<std::uint16_t>(scaled + 0.5f);
}

float Quantize_Scale(float world_size){
    return world_size > 0.0f ? 65535.0f / world_size : 0.0f;
}

void Write_F32(std::vector<char>& out, float value){
    // float의 비트 표현을 그대로 보낸다. 서버와 클라이언트 모두 IEEE 754 little-endian 환경을 가정한다.
    Write_U32(out, std::bit_cast<std::uint32_t>(value));
}

// 헤더 자리를 먼저 비워 두고 body를 직접 쓴 뒤 size를 채운다. 임시 body vector를 만들지 않기 위해서다.
std::size_t Begin_Packet(std::vector<char>& out, PacketId id){
    const std::size_t start = out.size();
    Write_U16(out, 0);
    Write_U16(out, static_cast<std::uint16_t>(id));
    return start;
}

void End_Packet(std::vector<char>& out, std::size_t start){
    const auto size = static_cast<std::uint16_t>(out.size() - start);
    out[start] = static_cast<char>(size & 0xFF);
    out[start + 1] = static_cast<char>((size >> 8) & 0xFF);
}
}

std::uint16_t Read_U16(const char* data){
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

std::uint32_t Read_U32(const char* data){
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

float Read_F32(const char* data){
    return std::bit_cast<float>(Read_U32(data));
}

bool Append_Packet(std::vector<char>& out, PacketId id, const char* body, std::size_t body_length){
    if(body_length > kMaxBodySize){
        return false;
    }
    const std::size_t start = Begin_Packet(out, id);
    out.insert(out.end(), body, body + body_length);
    End_Packet(out, start);
    return true;
}

std::vector<char> Make_Packet(PacketId id, const char* body, std::size_t body_length){
    std::vector<char> out;
    out.reserve(kHeaderSize + body_length);
    Append_Packet(out, id, body, body_length);
    return out;
}

void Append_Chat(std::vector<char>& out, std::uint32_t sender_id, std::string_view text){
    const std::size_t start = Begin_Packet(out, PacketId::S_Chat);
    Write_U32(out, sender_id);
    // 본문 상한을 넘는 텍스트는 잘라서 항상 유효한 패킷을 만든다.
    const std::size_t text_length = (std::min)(text.size(), kMaxBodySize - 4);
    out.insert(out.end(), text.data(), text.data() + text_length);
    End_Packet(out, start);
}

std::vector<char> Make_Chat(std::uint32_t sender_id, std::string_view text){
    std::vector<char> out;
    Append_Chat(out, sender_id, text);
    return out;
}

std::vector<char> Make_Notice(std::string_view text){
    const std::size_t text_length = (std::min)(text.size(), kMaxBodySize);
    return Make_Packet(PacketId::S_Notice, text.data(), text_length);
}

void Append_Enter_World(std::vector<char>& out, const EnterWorldInfo& info){
    const std::size_t start = Begin_Packet(out, PacketId::S_EnterWorld);
    Write_U32(out, info.id);
    Write_F32(out, info.x);
    Write_F32(out, info.y);
    Write_F32(out, info.world_size);
    Write_F32(out, info.cell_size);
    End_Packet(out, start);
}

std::uint16_t Quantize_Coordinate(float value, float world_size){
    return Quantize_With_Scale(value, Quantize_Scale(world_size));
}

float Dequantize_Coordinate(std::uint16_t value, float world_size){
    return static_cast<float>(value) * world_size / 65535.0f;
}

void Append_Self_State(std::vector<char>& out, const SelfState& state){
    const std::size_t start = Begin_Packet(out, PacketId::S_SelfState);
    Write_U32(out, state.last_input_seq);
    Write_F32(out, state.x);
    Write_F32(out, state.y);
    End_Packet(out, start);
}

void Append_Entity_States(std::vector<char>& out, PacketId id, std::span<const EntityState> states, float world_size){
    const float scale = Quantize_Scale(world_size);
    std::size_t index = 0;
    while(index < states.size()){
        const std::size_t count = (std::min)(states.size() - index, kMaxEntityStatesPerPacket);
        const std::size_t size = kHeaderSize + kListCountSize + count * kEntityStateSize;
        const std::size_t start = out.size();
        out.resize(start + size);
        char* cursor = out.data() + start;
        cursor = Put_U16(cursor, static_cast<std::uint16_t>(size));
        cursor = Put_U16(cursor, static_cast<std::uint16_t>(id));
        cursor = Put_U16(cursor, static_cast<std::uint16_t>(count));
        for(std::size_t i = 0; i < count; ++i){
            const EntityState& state = states[index + i];
            cursor = Put_U32(cursor, state.id);
            cursor = Put_U16(cursor, Quantize_With_Scale(state.x, scale));
            cursor = Put_U16(cursor, Quantize_With_Scale(state.y, scale));
        }
        index += count;
    }
}

void Append_Despawns(std::vector<char>& out, std::span<const std::uint32_t> ids){
    std::size_t index = 0;
    while(index < ids.size()){
        const std::size_t count = (std::min)(ids.size() - index, kMaxIdsPerPacket);
        const std::size_t size = kHeaderSize + kListCountSize + count * 4;
        const std::size_t start = out.size();
        out.resize(start + size);
        char* cursor = out.data() + start;
        cursor = Put_U16(cursor, static_cast<std::uint16_t>(size));
        cursor = Put_U16(cursor, static_cast<std::uint16_t>(PacketId::S_Despawn));
        cursor = Put_U16(cursor, static_cast<std::uint16_t>(count));
        for(std::size_t i = 0; i < count; ++i){
            cursor = Put_U32(cursor, ids[index + i]);
        }
        index += count;
    }
}

std::vector<char> Make_Client_Chat(std::string_view text){
    const std::size_t text_length = (std::min)(text.size(), kMaxBodySize);
    return Make_Packet(PacketId::C_Chat, text.data(), text_length);
}

std::vector<char> Make_Move(std::uint32_t input_seq, float velocity_x, float velocity_y){
    std::vector<char> out;
    const std::size_t start = Begin_Packet(out, PacketId::C_Move);
    Write_U32(out, input_seq);
    Write_F32(out, velocity_x);
    Write_F32(out, velocity_y);
    End_Packet(out, start);
    return out;
}

std::optional<EnterWorldInfo> Parse_Enter_World(const std::vector<char>& body){
    if(body.size() != 20){
        return std::nullopt;
    }
    EnterWorldInfo info;
    info.id = Read_U32(body.data());
    info.x = Read_F32(body.data() + 4);
    info.y = Read_F32(body.data() + 8);
    info.world_size = Read_F32(body.data() + 12);
    info.cell_size = Read_F32(body.data() + 16);
    return info;
}

std::optional<SelfState> Parse_Self_State(const std::vector<char>& body){
    if(body.size() != 12){
        return std::nullopt;
    }
    return SelfState{Read_U32(body.data()), Read_F32(body.data() + 4), Read_F32(body.data() + 8)};
}

std::optional<std::vector<EntityState>> Parse_Entity_States(const std::vector<char>& body, float world_size){
    if(body.size() < kListCountSize){
        return std::nullopt;
    }
    const std::size_t count = Read_U16(body.data());
    if(body.size() != kListCountSize + count * kEntityStateSize){
        return std::nullopt;
    }
    std::vector<EntityState> states(count);
    const char* cursor = body.data() + kListCountSize;
    for(std::size_t i = 0; i < count; ++i, cursor += kEntityStateSize){
        states[i].id = Read_U32(cursor);
        states[i].x = Dequantize_Coordinate(Read_U16(cursor + 4), world_size);
        states[i].y = Dequantize_Coordinate(Read_U16(cursor + 6), world_size);
    }
    return states;
}

std::optional<std::vector<std::uint32_t>> Parse_Despawns(const std::vector<char>& body){
    if(body.size() < kListCountSize){
        return std::nullopt;
    }
    const std::size_t count = Read_U16(body.data());
    if(body.size() != kListCountSize + count * 4){
        return std::nullopt;
    }
    std::vector<std::uint32_t> ids(count);
    for(std::size_t i = 0; i < count; ++i){
        ids[i] = Read_U32(body.data() + kListCountSize + i * 4);
    }
    return ids;
}

std::optional<MoveInput> Parse_Move(const std::vector<char>& body){
    if(body.size() != 12){
        return std::nullopt;
    }
    return MoveInput{Read_U32(body.data()), Read_F32(body.data() + 4), Read_F32(body.data() + 8)};
}

}
