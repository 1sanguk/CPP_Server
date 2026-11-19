#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// v7 패킷 형식 (모든 정수는 little-endian)
//   [uint16 size][uint16 id][body ...]
// size는 헤더 4바이트를 포함한 패킷 전체 길이다. TCP는 경계가 없는 바이트 스트림이므로
// 수신 측은 size를 보고 패킷 하나가 다 모였는지 판단한다.
namespace protocol{

constexpr std::size_t kHeaderSize = 4;
// 비정상 길이로 메모리를 과하게 쓰게 만드는 입력을 막기 위한 상한이다.
constexpr std::size_t kMaxPacketSize = 8 * 1024;
constexpr std::size_t kMaxBodySize = kMaxPacketSize - kHeaderSize;

enum class PacketId : std::uint16_t{
    // client -> server
    C_Chat = 1,         // body: UTF-8 text

    // server -> client
    S_Welcome = 101,    // body: uint32 session_id
    S_Chat = 102,       // body: uint32 sender_session_id + UTF-8 text
    S_Notice = 103,     // body: UTF-8 text
};

struct Packet{
    std::uint16_t id = 0;
    std::vector<char> body;
};

// 헤더를 붙여 송신 가능한 바이트 열을 만든다. body가 kMaxBodySize를 넘으면 빈 벡터를 반환한다.
std::vector<char> Make_Packet(PacketId id, const char* body, std::size_t body_length);
std::vector<char> Make_Welcome(std::uint32_t session_id);
std::vector<char> Make_Chat(std::uint32_t sender_session_id, std::string_view text);
std::vector<char> Make_Notice(std::string_view text);

std::uint16_t Read_U16(const char* data);
std::uint32_t Read_U32(const char* data);

}
