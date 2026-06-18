#include "protocol.h"
#include <cstring>

namespace tcp_protocol {

std::vector<uint8_t> EncodeMessage(const Message& msg) {
    std::vector<uint8_t> buffer;
    buffer.reserve(HEADER_SIZE + msg.data.size());
    
    // func (2 bytes, little-endian)
    buffer.push_back(static_cast<uint8_t>(msg.func & 0xFF));
    buffer.push_back(static_cast<uint8_t>((msg.func >> 8) & 0xFF));
    
    // magic (2 bytes, little-endian)
    buffer.push_back(static_cast<uint8_t>(msg.magic & 0xFF));
    buffer.push_back(static_cast<uint8_t>((msg.magic >> 8) & 0xFF));
    
    // length (2 bytes, little-endian)
    buffer.push_back(static_cast<uint8_t>(msg.length & 0xFF));
    buffer.push_back(static_cast<uint8_t>((msg.length >> 8) & 0xFF));
    
    // func2 (2 bytes, little-endian)
    buffer.push_back(static_cast<uint8_t>(msg.func2 & 0xFF));
    buffer.push_back(static_cast<uint8_t>((msg.func2 >> 8) & 0xFF));
    
    // dataSize (2 bytes, little-endian)
    buffer.push_back(static_cast<uint8_t>(msg.dataSize & 0xFF));
    buffer.push_back(static_cast<uint8_t>((msg.dataSize >> 8) & 0xFF));
    
    // data
    buffer.insert(buffer.end(), msg.data.begin(), msg.data.end());
    
    return buffer;
}

bool DecodeMessage(const uint8_t* buffer, size_t size, Message& msg) {
    if (size < HEADER_SIZE) {
        return false;
    }
    
    size_t offset = 0;
    
    // func (2 bytes, little-endian)
    msg.func = buffer[offset] | (buffer[offset + 1] << 8);
    offset += 2;
    
    // magic (2 bytes, little-endian)
    msg.magic = buffer[offset] | (buffer[offset + 1] << 8);
    offset += 2;
    
    // length (2 bytes, little-endian)
    msg.length = buffer[offset] | (buffer[offset + 1] << 8);
    offset += 2;
    
    // func2 (2 bytes, little-endian)
    msg.func2 = buffer[offset] | (buffer[offset + 1] << 8);
    offset += 2;
    
    // dataSize (2 bytes, little-endian)
    msg.dataSize = buffer[offset] | (buffer[offset + 1] << 8);
    offset += 2;
    
    // 验证魔数
    if (msg.magic != MAGIC) {
        return false;
    }
    
    // 验证长度
    if (msg.length != size) {
        return false;
    }
    
    // 读取data（如果有，且消息长度大于HEADER_SIZE）
    size_t dataSize = msg.dataSize;
    if (dataSize > 0 && size > HEADER_SIZE) {
        if (size < HEADER_SIZE + dataSize) {
            return false;
        }
        msg.data.assign(buffer + offset, buffer + offset + dataSize);
    } else {
        msg.data.clear();
    }
    
    return true;
}

Message CreateReadResponse(const std::vector<uint8_t>& data) {
    Message msg;
    msg.func = FUNC_READ_RESPONSE;
    msg.magic = MAGIC;
    msg.func2 = FUNC_READ_RESPONSE;
    msg.dataSize = static_cast<uint16_t>(data.size());
    msg.data = data;
    msg.length = HEADER_SIZE + static_cast<uint16_t>(data.size());
    return msg;
}

Message CreateWriteResponse(uint16_t dataSize) {
    Message msg;
    msg.func = FUNC_WRITE_RESPONSE;
    msg.magic = MAGIC;
    msg.func2 = FUNC_WRITE_RESPONSE;
    msg.dataSize = dataSize;
    msg.length = HEADER_SIZE;
    return msg;
}

} // namespace tcp_protocol
