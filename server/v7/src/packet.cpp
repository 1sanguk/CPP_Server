#include <packet.h>

#include <algorithm>

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

std::vector<char> Make_Packet(PacketId id, const char* body, std::size_t body_length){
    if(body_length > kMaxBodySize){
        return {};
    }
    std::vector<char> out;
    out.reserve(kHeaderSize + body_length);
    Write_U16(out, static_cast<std::uint16_t>(kHeaderSize + body_length));
    Write_U16(out, static_cast<std::uint16_t>(id));
    out.insert(out.end(), body, body + body_length);
    return out;
}

std::vector<char> Make_Welcome(std::uint32_t session_id){
    std::vector<char> body;
    Write_U32(body, session_id);
    return Make_Packet(PacketId::S_Welcome, body.data(), body.size());
}

std::vector<char> Make_Chat(std::uint32_t sender_session_id, std::string_view text){
    std::vector<char> body;
    Write_U32(body, sender_session_id);
    // 본문 상한을 넘는 텍스트는 잘라서 항상 유효한 패킷을 만든다.
    const std::size_t text_length = (std::min)(text.size(), kMaxBodySize - body.size());
    body.insert(body.end(), text.data(), text.data() + text_length);
    return Make_Packet(PacketId::S_Chat, body.data(), body.size());
}

std::vector<char> Make_Notice(std::string_view text){
    const std::size_t text_length = (std::min)(text.size(), kMaxBodySize);
    return Make_Packet(PacketId::S_Notice, text.data(), text_length);
}

}
