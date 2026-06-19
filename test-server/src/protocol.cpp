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
    if (buffer == nullptr) {
        return false;
    }
    if (size < HEADER_SIZE) {
        return false;
    }
    
    size_t offset = 0;
    
    msg.func = buffer[offset] | (buffer[offset + 1] << 8);
    offset += 2;
    
    msg.magic = buffer[offset] | (buffer[offset + 1] << 8);
    offset += 2;
    
    msg.length = buffer[offset] | (buffer[offset + 1] << 8);
    offset += 2;
    
    msg.func2 = buffer[offset] | (buffer[offset + 1] << 8);
    offset += 2;
    
    msg.dataSize = buffer[offset] | (buffer[offset + 1] << 8);
    offset += 2;
    
    if (msg.magic != MAGIC) {
        return false;
    }
    if (msg.length < HEADER_SIZE) {
        return false;
    }
    if (msg.length != size) {
        return false;
    }
    if (msg.func != msg.func2) {
        return false;
    }
    size_t expected_payload = static_cast<size_t>(msg.length) - HEADER_SIZE;
    bool has_data = (msg.func == FUNC_READ_RESPONSE) || (msg.func == FUNC_WRITE_REQUEST);
    if (has_data) {
        if (static_cast<size_t>(msg.dataSize) != expected_payload) {
            return false;
        }
    } else {
        if (expected_payload != 0) {
            return false;
        }
    }

    if (expected_payload > 0) {
        if (size < HEADER_SIZE + expected_payload) {
            return false;
        }
        msg.data.assign(buffer + offset, buffer + offset + expected_payload);
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
