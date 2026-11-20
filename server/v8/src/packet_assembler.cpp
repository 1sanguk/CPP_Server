#include <packet_assembler.h>

void PacketAssembler::Compact(){
    // 읽은 앞부분이 남은 데이터보다 많아졌을 때만 한 번에 당겨서 복사 비용을 줄인다.
    if(this->read_offset == 0){
        return;
    }
    if(this->read_offset == this->buffer.size()){
        this->buffer.clear();
        this->read_offset = 0;
        return;
    }
    if(this->read_offset >= this->buffer.size() - this->read_offset){
        this->buffer.erase(this->buffer.begin(),
            this->buffer.begin() + static_cast<std::ptrdiff_t>(this->read_offset));
        this->read_offset = 0;
    }
}

void PacketAssembler::Append(const char* data, std::size_t length){
    if(this->error || length == 0){
        return;
    }
    this->Compact();
    this->buffer.insert(this->buffer.end(), data, data + length);
}

std::optional<protocol::Packet> PacketAssembler::Next(){
    if(this->error){
        return std::nullopt;
    }

    const std::size_t available = this->buffer.size() - this->read_offset;
    if(available < protocol::kHeaderSize){
        return std::nullopt;
    }

    const char* head = this->buffer.data() + this->read_offset;
    const std::size_t packet_size = protocol::Read_U16(head);
    if(packet_size < protocol::kHeaderSize || packet_size > protocol::kMaxPacketSize){
        this->error = true;
        return std::nullopt;
    }
    if(available < packet_size){
        return std::nullopt;
    }

    protocol::Packet packet;
    packet.id = protocol::Read_U16(head + 2);
    packet.body.assign(head + protocol::kHeaderSize, head + packet_size);
    this->read_offset += packet_size;
    return packet;
}

bool PacketAssembler::Has_Error() const{
    return this->error;
}

std::size_t PacketAssembler::Buffered_Bytes() const{
    return this->buffer.size() - this->read_offset;
}
