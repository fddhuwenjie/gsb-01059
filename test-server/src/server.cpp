#include <uv.h>
#include <iostream>
#include <vector>
#include <map>
#include <cstddef>
#include <cstring>
#include "protocol.h"

struct ClientData {
    uv_tcp_t* tcp;
    std::vector<uint8_t> receive_buffer;
    std::vector<uint8_t> storage;  // 简单的内存存储
};

std::map<uv_tcp_t*, ClientData*> clients;

void OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    ClientData* client = static_cast<ClientData*>(handle->data);
    buf->base = new char[suggested_size];
    buf->len = suggested_size;
}

void SendResponse(uv_tcp_t* tcp, const tcp_protocol::Message& msg) {
    std::vector<uint8_t> buffer = tcp_protocol::EncodeMessage(msg);
    
    uv_write_t* write_req = new uv_write_t;
    char* write_buf = new char[buffer.size()];
    std::memcpy(write_buf, buffer.data(), buffer.size());
    
    uv_buf_t buf = uv_buf_init(write_buf, buffer.size());
    
    write_req->data = write_buf;  // 保存指针以便释放
    
    uv_write(write_req, reinterpret_cast<uv_stream_t*>(tcp), &buf, 1, [](uv_write_t* req, int status) {
        if (status < 0) {
            std::cerr << "Write error: " << uv_strerror(status) << std::endl;
        }
        char* buf = static_cast<char*>(req->data);
        delete[] buf;
        delete req;
    });
}

void HandleMessage(uv_tcp_t* tcp, const tcp_protocol::Message& msg) {
    ClientData* client = static_cast<ClientData*>(tcp->data);
    
    switch (msg.func) {
        case tcp_protocol::FUNC_READ_REQUEST:
            std::cout << "Received read request" << std::endl;
            {
                tcp_protocol::Message response = tcp_protocol::CreateReadResponse(client->storage);
                SendResponse(tcp, response);
            }
            break;
            
        case tcp_protocol::FUNC_WRITE_REQUEST:
            std::cout << "Received write request, data size: " << msg.data.size() << std::endl;
            client->storage = msg.data;
            {
                tcp_protocol::Message response = tcp_protocol::CreateWriteResponse(static_cast<uint16_t>(msg.data.size()));
                SendResponse(tcp, response);
            }
            break;
            
        default:
            std::cerr << "Unknown message type: " << msg.func << std::endl;
            break;
    }
}

void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    ClientData* client = static_cast<ClientData*>(stream->data);
    
    if (nread < 0) {
        if (nread != UV_EOF) {
            std::cerr << "Read error: " << uv_strerror(nread) << std::endl;
        }
        uv_close(reinterpret_cast<uv_handle_t*>(stream), [](uv_handle_t* handle) {
            ClientData* client = static_cast<ClientData*>(handle->data);
            clients.erase(reinterpret_cast<uv_tcp_t*>(handle));
            delete client;
        });
        delete[] buf->base;
        return;
    }
    
    if (nread == 0) {
        delete[] buf->base;
        return;
    }
    
    // 将数据添加到接收缓冲区
    size_t old_size = client->receive_buffer.size();
    client->receive_buffer.resize(old_size + nread);
    std::memcpy(client->receive_buffer.data() + old_size, buf->base, nread);
    delete[] buf->base;
    
    // 尝试解析消息
    while (client->receive_buffer.size() >= tcp_protocol::HEADER_SIZE) {
        // 先读取header获取length
        uint16_t length = client->receive_buffer[4] | (client->receive_buffer[5] << 8);
        
        if (client->receive_buffer.size() < length) {
            // 数据不完整，等待更多数据
            break;
        }
        
        // 解析完整消息
        tcp_protocol::Message msg;
        if (tcp_protocol::DecodeMessage(client->receive_buffer.data(), length, msg)) {
            HandleMessage(reinterpret_cast<uv_tcp_t*>(stream), msg);
            
            // 移除已处理的消息
            client->receive_buffer.erase(
                client->receive_buffer.begin(),
                client->receive_buffer.begin() + length
            );
        } else {
            // 解析失败，清空缓冲区
            std::cerr << "Failed to decode message" << std::endl;
            client->receive_buffer.clear();
            break;
        }
    }
}

void OnNewConnection(uv_stream_t* server, int status) {
    if (status < 0) {
        std::cerr << "New connection error: " << uv_strerror(status) << std::endl;
        return;
    }
    
    uv_tcp_t* client_tcp = new uv_tcp_t;
    uv_tcp_init(server->loop, client_tcp);
    
    ClientData* client = new ClientData;
    client->tcp = client_tcp;
    client_tcp->data = client;
    clients[client_tcp] = client;
    
    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(client_tcp)) == 0) {
        std::cout << "New client connected" << std::endl;
        uv_read_start(reinterpret_cast<uv_stream_t*>(client_tcp), OnAlloc, OnRead);
    } else {
        uv_close(reinterpret_cast<uv_handle_t*>(client_tcp), [](uv_handle_t* handle) {
            ClientData* client = static_cast<ClientData*>(handle->data);
            clients.erase(reinterpret_cast<uv_tcp_t*>(handle));
            delete client;
        });
    }
}

int main() {
    uv_loop_t* loop = uv_default_loop();
    
    uv_tcp_t server;
    uv_tcp_init(loop, &server);
    
    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", 8888, &addr);
    
    uv_tcp_bind(&server, reinterpret_cast<const struct sockaddr*>(&addr), 0);
    
    int r = uv_listen(reinterpret_cast<uv_stream_t*>(&server), 128, OnNewConnection);
    if (r) {
        std::cerr << "Listen error: " << uv_strerror(r) << std::endl;
        return 1;
    }
    
    std::cout << "=== TCP Test Server Started ===" << std::endl;
    std::cout << "Startup Success" << std::endl;
    std::cout << "Server listening on: 0.0.0.0:8888" << std::endl;
    std::cout << "Waiting for connections..." << std::endl;
    
    uv_run(loop, UV_RUN_DEFAULT);
    
    return 0;
}
