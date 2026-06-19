#include "protocol.h"
#include <cstring>
#include <algorithm>

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
    
    // 验证长度下限
    if (msg.length < HEADER_SIZE) {
        return false;
    }

    // 验证长度与传入 size 一致
    if (msg.length != size) {
        return false;
    }

    // 验证 func / func2 一致性（防止半包错位带来的伪消息）
    if (msg.func != msg.func2) {
        return false;
    }

    // 对于"携带 data 的消息"（读响应/写请求），dataSize 必须与 length-HEADER 相等
    // 对于"不携带 data 的消息"（读请求/写响应），length 必须等于 HEADER_SIZE，
    // 但 dataSize 字段用于业务回传（如已写字节数），不参与帧长校验
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

    // 读取 data
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

Message CreateReadRequest() {
    Message msg;
    msg.func = FUNC_READ_REQUEST;
    msg.magic = MAGIC;
    msg.func2 = FUNC_READ_REQUEST;
    msg.dataSize = 0;
    msg.length = HEADER_SIZE; // 没有data部分
    return msg;
}

Message CreateWriteRequest(const std::vector<uint8_t>& data) {
    Message msg;
    msg.func = FUNC_WRITE_REQUEST;
    msg.magic = MAGIC;
    msg.func2 = FUNC_WRITE_REQUEST;
    msg.dataSize = static_cast<uint16_t>(data.size());
    msg.data = data;
    msg.length = HEADER_SIZE + static_cast<uint16_t>(data.size());
    return msg;
}

} // namespace tcp_protocol
