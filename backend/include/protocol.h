#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

namespace tcp_protocol {

// 协议常量
constexpr uint16_t MAGIC = 0xABCD;
constexpr uint16_t FUNC_READ_REQUEST = 0x0000;
constexpr uint16_t FUNC_WRITE_REQUEST = 0x0001;
constexpr uint16_t FUNC_READ_RESPONSE = 0x0002;
constexpr uint16_t FUNC_WRITE_RESPONSE = 0x0003;

// 消息头大小（不包括data部分）
constexpr size_t HEADER_SIZE = 10; // func(2) + magic(2) + length(2) + func(2) + dataSize(2)

// 协议消息结构
struct Message {
    uint16_t func;
    uint16_t magic;
    uint16_t length;
    uint16_t func2;  // 重复的功能码
    uint16_t dataSize;
    std::vector<uint8_t> data;
};

// 编码消息为字节流
std::vector<uint8_t> EncodeMessage(const Message& msg);

// 解码字节流为消息
bool DecodeMessage(const uint8_t* buffer, size_t size, Message& msg);

// 创建读数据请求
Message CreateReadRequest();

// 创建写数据请求
Message CreateWriteRequest(const std::vector<uint8_t>& data);

} // namespace tcp_protocol

#endif // PROTOCOL_H
