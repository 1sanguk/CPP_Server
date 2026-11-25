#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// v8 패킷 형식 (모든 정수와 float는 little-endian)
//   [uint16 size][uint16 id][body ...]
// 헤더 규칙은 v7과 같다. v8은 월드 좌표를 주고받는 패킷을 추가한다.
// 주변 엔티티 좌표(S_Spawn/S_MoveBatch)는 월드 크기 기준 16비트 고정소수점으로 줄여 보낸다(Quantize_Coordinate).
namespace protocol{

constexpr std::size_t kHeaderSize = 4;
// 비정상 길이로 메모리를 과하게 쓰게 만드는 입력을 막기 위한 상한이다.
constexpr std::size_t kMaxPacketSize = 8 * 1024;
constexpr std::size_t kMaxBodySize = kMaxPacketSize - kHeaderSize;

enum class PacketId : std::uint16_t{
    // client -> server
    C_Chat = 1,         // body: UTF-8 text. 시야(주변 셀) 안의 플레이어와 자신에게만 전달된다
    C_Move = 2,         // body: uint32 input_seq + float vx + float vy (초당 이동량). 서버가 속도 상한으로 보정한다

    // server -> client
    S_Chat = 102,       // body: uint32 sender_id + UTF-8 text
    S_Notice = 103,     // body: UTF-8 text
    S_EnterWorld = 104, // body: uint32 my_id + float x + float y + float world_size + float cell_size
    S_Spawn = 105,      // body: uint16 count + [uint32 id + uint16 qx + uint16 qy] * count
    S_Despawn = 106,    // body: uint16 count + [uint32 id] * count
    S_MoveBatch = 107,  // body: uint16 count + [uint32 id + uint16 qx + uint16 qy] * count (자기 자신 제외)
    // 내 위치와 "몇 번 입력까지 반영했는지". 클라이언트가 예측 이동 후 서버 결과로 보정할 때 쓴다.
    S_SelfState = 108,  // body: uint32 last_input_seq + float x + float y
};

struct Packet{
    std::uint16_t id = 0;
    std::vector<char> body;
};

// 시야 진입(S_Spawn)과 이동(S_MoveBatch)에 공통으로 쓰는 엔티티 한 개의 상태.
struct EntityState{
    std::uint32_t id = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct SelfState{
    std::uint32_t last_input_seq = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct MoveInput{
    std::uint32_t input_seq = 0;
    float velocity_x = 0.0f;
    float velocity_y = 0.0f;
};

struct EnterWorldInfo{
    std::uint32_t id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float world_size = 0.0f;
    float cell_size = 0.0f;
};

// v8 보완 전에는 float 두 개로 12바이트였다. 16비트 좌표로 8바이트가 되어 이동 패킷이 1/3 줄었다.
constexpr std::size_t kEntityStateSize = 8;
constexpr std::size_t kListCountSize = 2;
// 목록 패킷 하나에 들어가는 최대 항목 수. 넘치면 같은 id의 패킷 여러 개로 나눈다.
constexpr std::size_t kMaxEntityStatesPerPacket = (kMaxBodySize - kListCountSize) / kEntityStateSize;
constexpr std::size_t kMaxIdsPerPacket = (kMaxBodySize - kListCountSize) / 4;

// out 뒤에 헤더를 붙인 패킷을 이어 붙인다. v8은 한 세션에 보낼 한 틱의 패킷들을 버퍼 하나에 모으므로
// 패킷마다 vector를 따로 만들지 않는다. body가 kMaxBodySize를 넘으면 아무것도 쓰지 않고 false를 반환한다.
bool Append_Packet(std::vector<char>& out, PacketId id, const char* body, std::size_t body_length);
std::vector<char> Make_Packet(PacketId id, const char* body, std::size_t body_length);

void Append_Chat(std::vector<char>& out, std::uint32_t sender_id, std::string_view text);
std::vector<char> Make_Chat(std::uint32_t sender_id, std::string_view text);
std::vector<char> Make_Notice(std::string_view text);
void Append_Enter_World(std::vector<char>& out, const EnterWorldInfo& info);
void Append_Self_State(std::vector<char>& out, const SelfState& state);
// S_Spawn/S_MoveBatch 목록을 kMaxEntityStatesPerPacket 단위로 나눠 이어 붙인다. 빈 목록이면 아무것도 쓰지 않는다.
// 좌표는 world_size 기준 16비트로 줄여 쓴다.
void Append_Entity_States(std::vector<char>& out, PacketId id, std::span<const EntityState> states, float world_size);

// 좌표 [0, world_size]를 0~65535로 나타낸다. 월드 1000이면 한 단계가 약 0.015다.
std::uint16_t Quantize_Coordinate(float value, float world_size);
float Dequantize_Coordinate(std::uint16_t value, float world_size);
void Append_Despawns(std::vector<char>& out, std::span<const std::uint32_t> ids);

// 클라이언트(부하 봇, 테스트)가 보내는 패킷
std::vector<char> Make_Client_Chat(std::string_view text);
std::vector<char> Make_Move(std::uint32_t input_seq, float velocity_x, float velocity_y);

// 받은 패킷 body 해석. 길이가 맞지 않으면 nullopt를 반환한다.
std::optional<EnterWorldInfo> Parse_Enter_World(const std::vector<char>& body);
// 좌표는 world_size로 복원한 값이다(서버 좌표를 Quantize → Dequantize한 값과 같다).
std::optional<std::vector<EntityState>> Parse_Entity_States(const std::vector<char>& body, float world_size);
std::optional<SelfState> Parse_Self_State(const std::vector<char>& body);
std::optional<std::vector<std::uint32_t>> Parse_Despawns(const std::vector<char>& body);
// C_Move body. 12바이트가 아니면 nullopt를 반환한다.
std::optional<MoveInput> Parse_Move(const std::vector<char>& body);

std::uint16_t Read_U16(const char* data);
std::uint32_t Read_U32(const char* data);
float Read_F32(const char* data);

}
