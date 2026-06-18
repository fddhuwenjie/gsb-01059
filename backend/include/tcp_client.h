#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <uv.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "protocol.h"

namespace tcp_client {

class TcpClient {
public:
    using ReadCallback = std::function<void(bool success, const std::vector<uint8_t>& data)>;
    using WriteCallback = std::function<void(bool success, uint16_t dataSize)>;
    using ConnectCallback = std::function<void(bool success)>;
    using ErrorCallback = std::function<void(const std::string& error)>;

    TcpClient();
    ~TcpClient();

    // 禁止拷贝
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    // 连接到服务器
    bool Connect(const std::string& host, uint16_t port, ConnectCallback callback = nullptr);
    
    // 断开连接
    void Disconnect();
    
    // 检查连接状态
    bool IsConnected() const;
    
    // 读取数据
    void ReadData(ReadCallback callback);
    
    // 写入数据
    void WriteData(const std::vector<uint8_t>& data, WriteCallback callback);
    
    // 设置错误回调
    void SetErrorCallback(ErrorCallback callback);
    
    // 运行事件循环（阻塞）
    void Run();
    
    // 停止事件循环
    void Stop();

private:
    struct ClientData {
        TcpClient* client;
        uv_tcp_t* tcp;
        uv_connect_t* connect_req;
        std::vector<uint8_t> read_buffer;
        size_t expected_size;
        bool reading_header;
    };
    
    struct WriteRequestData {
        TcpClient* client;
        char* buffer;  // 保存分配的缓冲区指针以便释放
    };

    static void OnConnect(uv_connect_t* req, int status);
    static void OnGetAddrInfo(uv_getaddrinfo_t* req, int status, struct addrinfo* res);
    static void OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
    static void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
    static void OnWrite(uv_write_t* req, int status);
    static void OnClose(uv_handle_t* handle);
    static void AsyncCallback(uv_async_t* handle);

    void HandleMessage(const tcp_protocol::Message& msg);
    void SendMessage(const tcp_protocol::Message& msg);

    uv_loop_t* loop_;
    uv_tcp_t* tcp_;
    uv_connect_t* connect_req_;
    uv_getaddrinfo_t* getaddrinfo_req_;
    uv_async_t* async_;
    bool connected_;
    bool should_stop_;
    
    ClientData* client_data_;
    std::string connect_host_;
    uint16_t connect_port_;
    
    ReadCallback read_callback_;
    WriteCallback write_callback_;
    ConnectCallback connect_callback_;
    ErrorCallback error_callback_;
    
    std::vector<uint8_t> receive_buffer_;
    size_t expected_length_;
    bool reading_header_;
};

} // namespace tcp_client

#endif // TCP_CLIENT_H
