#include "tcp_client.h"
#include <iostream>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace tcp_client {

TcpClient::TcpClient()
    : loop_(nullptr)
    , tcp_(nullptr)
    , connect_req_(nullptr)
    , getaddrinfo_req_(nullptr)
    , async_(nullptr)
    , connected_(false)
    , should_stop_(false)
    , client_data_(nullptr)
    , expected_length_(0)
    , reading_header_(true)
{
    loop_ = uv_loop_new();
    if (!loop_) {
        throw std::runtime_error("Failed to create UV loop");
    }
    
    async_ = new uv_async_t;
    if (uv_async_init(loop_, async_, AsyncCallback) != 0) {
        delete async_;
        uv_loop_delete(loop_);
        throw std::runtime_error("Failed to initialize async handle");
    }
    async_->data = this;
}

TcpClient::~TcpClient() {
    Disconnect();
    if (async_) {
        uv_close(reinterpret_cast<uv_handle_t*>(async_), nullptr);
    }
    if (loop_) {
        uv_run(loop_, UV_RUN_DEFAULT);
        uv_loop_delete(loop_);
    }
    delete async_;
}

bool TcpClient::Connect(const std::string& host, uint16_t port, ConnectCallback callback) {
    if (connected_) {
        if (error_callback_) {
            error_callback_("Already connected");
        }
        return false;
    }
    
    connect_callback_ = callback;
    connect_host_ = host;
    connect_port_ = port;
    
    tcp_ = new uv_tcp_t;
    if (uv_tcp_init(loop_, tcp_) != 0) {
        delete tcp_;
        tcp_ = nullptr;
        if (error_callback_) {
            error_callback_("Failed to initialize TCP handle");
        }
        return false;
    }
    
    client_data_ = new ClientData;
    client_data_->client = this;
    client_data_->tcp = tcp_;
    client_data_->reading_header = true;
    client_data_->expected_size = tcp_protocol::HEADER_SIZE;
    tcp_->data = client_data_;
    
    // 使用getaddrinfo解析主机名
    getaddrinfo_req_ = new uv_getaddrinfo_t;
    getaddrinfo_req_->data = client_data_;
    
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%u", port);
    
    int r = uv_getaddrinfo(loop_, getaddrinfo_req_, OnGetAddrInfo, host.c_str(), port_str, &hints);
    if (r != 0) {
        delete getaddrinfo_req_;
        getaddrinfo_req_ = nullptr;
        delete client_data_;
        uv_close(reinterpret_cast<uv_handle_t*>(tcp_), OnClose);
        tcp_ = nullptr;
        if (error_callback_) {
            std::string error = "Failed to resolve hostname: ";
            error += uv_strerror(r);
            error_callback_(error);
        }
        return false;
    }
    
    return true;
}

void TcpClient::Disconnect() {
    if (tcp_ && connected_) {
        connected_ = false;
        uv_read_stop(reinterpret_cast<uv_stream_t*>(tcp_));
        uv_close(reinterpret_cast<uv_handle_t*>(tcp_), OnClose);
        tcp_ = nullptr;
    }
}

bool TcpClient::IsConnected() const {
    return connected_;
}

void TcpClient::ReadData(ReadCallback callback) {
    if (!connected_) {
        if (callback) {
            callback(false, {});
        }
        if (error_callback_) {
            error_callback_("Not connected");
        }
        return;
    }
    
    read_callback_ = callback;
    
    tcp_protocol::Message msg = tcp_protocol::CreateReadRequest();
    SendMessage(msg);
}

void TcpClient::WriteData(const std::vector<uint8_t>& data, WriteCallback callback) {
    if (!connected_) {
        if (callback) {
            callback(false, 0);
        }
        if (error_callback_) {
            error_callback_("Not connected");
        }
        return;
    }
    
    write_callback_ = callback;
    
    tcp_protocol::Message msg = tcp_protocol::CreateWriteRequest(data);
    SendMessage(msg);
}

void TcpClient::SetErrorCallback(ErrorCallback callback) {
    error_callback_ = callback;
}

void TcpClient::Run() {
    should_stop_ = false;
    uv_run(loop_, UV_RUN_DEFAULT);
}

void TcpClient::Stop() {
    should_stop_ = true;
    uv_stop(loop_);
}

void TcpClient::OnGetAddrInfo(uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
    ClientData* data = static_cast<ClientData*>(req->data);
    TcpClient* client = data->client;
    
    if (status < 0) {
        std::string error = "DNS resolution failed: ";
        error += uv_strerror(status);
        if (client->error_callback_) {
            client->error_callback_(error);
        }
        if (client->connect_callback_) {
            client->connect_callback_(false);
        }
        uv_freeaddrinfo(res);
        delete req;
        client->getaddrinfo_req_ = nullptr;
        delete client->client_data_;
        uv_close(reinterpret_cast<uv_handle_t*>(client->tcp_), OnClose);
        client->tcp_ = nullptr;
        return;
    }
    
    // 尝试连接第一个可用的地址
    struct addrinfo* addr = res;
    bool connected = false;
    
    while (addr != nullptr) {
        uv_connect_t* connect_req = new uv_connect_t;
        connect_req->data = data;
        
        int r = uv_tcp_connect(connect_req, client->tcp_, addr->ai_addr, OnConnect);
        if (r == 0) {
            client->connect_req_ = connect_req;
            connected = true;
            break;
        }
        
        delete connect_req;
        addr = addr->ai_next;
    }
    
    uv_freeaddrinfo(res);
    delete req;
    client->getaddrinfo_req_ = nullptr;
    
    if (!connected) {
        if (client->error_callback_) {
            client->error_callback_("Failed to connect to any address");
        }
        if (client->connect_callback_) {
            client->connect_callback_(false);
        }
        delete client->client_data_;
        uv_close(reinterpret_cast<uv_handle_t*>(client->tcp_), OnClose);
        client->tcp_ = nullptr;
    }
}

void TcpClient::OnConnect(uv_connect_t* req, int status) {
    ClientData* data = static_cast<ClientData*>(req->data);
    TcpClient* client = data->client;
    
    if (status < 0) {
        std::string error = "Connection failed: ";
        error += uv_strerror(status);
        if (client->error_callback_) {
            client->error_callback_(error);
        }
        if (client->connect_callback_) {
            client->connect_callback_(false);
        }
        delete req;
        client->connect_req_ = nullptr;
        return;
    }
    
    client->connected_ = true;
    
    if (uv_read_start(reinterpret_cast<uv_stream_t*>(data->tcp), OnAlloc, OnRead) != 0) {
        if (client->error_callback_) {
            client->error_callback_("Failed to start reading");
        }
        if (client->connect_callback_) {
            client->connect_callback_(false);
        }
        delete req;
        client->connect_req_ = nullptr;
        return;
    }
    
    if (client->connect_callback_) {
        client->connect_callback_(true);
    }
    
    delete req;
    client->connect_req_ = nullptr;
}

void TcpClient::OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    // 分配一个固定大小的缓冲区用于读取
    // libuv会在OnRead后自动释放这个缓冲区
    buf->base = new char[suggested_size];
    buf->len = suggested_size;
}

void TcpClient::OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    ClientData* data = static_cast<ClientData*>(stream->data);
    TcpClient* client = data->client;
    
    if (nread < 0) {
        // 释放缓冲区
        if (buf->base) {
            delete[] buf->base;
        }
        if (nread != UV_EOF) {
            std::string error = "Read error: ";
            error += uv_strerror(nread);
            if (client->error_callback_) {
                client->error_callback_(error);
            }
        }
        client->Disconnect();
        return;
    }
    
    if (nread == 0) {
        // 释放缓冲区
        if (buf->base) {
            delete[] buf->base;
        }
        return;
    }
    
    // 将读取的数据添加到接收缓冲区
    size_t old_size = client->receive_buffer_.size();
    client->receive_buffer_.resize(old_size + nread);
    std::memcpy(client->receive_buffer_.data() + old_size, buf->base, nread);
    
    // 释放libuv分配的缓冲区
    if (buf->base) {
        delete[] buf->base;
    }
    
    // 尝试解析消息
    while (client->receive_buffer_.size() >= tcp_protocol::HEADER_SIZE) {
        // 先读取header获取length
        uint16_t length = client->receive_buffer_[4] | (client->receive_buffer_[5] << 8);
        
        if (client->receive_buffer_.size() < length) {
            // 数据不完整，等待更多数据
            break;
        }
        
        // 解析完整消息
        tcp_protocol::Message msg;
        if (tcp_protocol::DecodeMessage(client->receive_buffer_.data(), length, msg)) {
            client->HandleMessage(msg);
            
            // 移除已处理的消息
            client->receive_buffer_.erase(
                client->receive_buffer_.begin(),
                client->receive_buffer_.begin() + length
            );
        } else {
            // 解析失败，清空缓冲区
            if (client->error_callback_) {
                client->error_callback_("Failed to decode message");
            }
            client->receive_buffer_.clear();
            break;
        }
    }
}

void TcpClient::OnWrite(uv_write_t* req, int status) {
    WriteRequestData* data = static_cast<WriteRequestData*>(req->data);
    TcpClient* client = data->client;
    
    // 释放分配的缓冲区
    if (data->buffer) {
        delete[] data->buffer;
    }
    delete data;
    delete req;
    
    if (status < 0) {
        std::string error = "Write error: ";
        error += uv_strerror(status);
        if (client->error_callback_) {
            client->error_callback_(error);
        }
    }
}

void TcpClient::OnClose(uv_handle_t* handle) {
    ClientData* data = static_cast<ClientData*>(handle->data);
    if (data) {
        delete data;
    }
}

void TcpClient::AsyncCallback(uv_async_t* handle) {
    // 用于异步操作的回调
}

void TcpClient::HandleMessage(const tcp_protocol::Message& msg) {
    switch (msg.func) {
        case tcp_protocol::FUNC_READ_RESPONSE:
            if (read_callback_) {
                read_callback_(true, msg.data);
            }
            break;
            
        case tcp_protocol::FUNC_WRITE_RESPONSE:
            if (write_callback_) {
                write_callback_(true, msg.dataSize);
            }
            break;
            
        default:
            if (error_callback_) {
                error_callback_("Unknown message type");
            }
            break;
    }
}

void TcpClient::SendMessage(const tcp_protocol::Message& msg) {
    if (!connected_ || !tcp_) {
        if (error_callback_) {
            error_callback_("Not connected");
        }
        return;
    }
    
    std::vector<uint8_t> buffer = tcp_protocol::EncodeMessage(msg);
    
    uv_write_t* write_req = new uv_write_t;
    WriteRequestData* data = new WriteRequestData;
    data->client = this;
    
    char* write_buf = new char[buffer.size()];
    std::memcpy(write_buf, buffer.data(), buffer.size());
    data->buffer = write_buf;  // 保存指针以便在OnWrite中释放
    
    write_req->data = data;
    
    uv_buf_t buf = uv_buf_init(write_buf, buffer.size());
    
    if (uv_write(write_req, reinterpret_cast<uv_stream_t*>(tcp_), &buf, 1, OnWrite) != 0) {
        delete[] write_buf;
        delete data;
        delete write_req;
        if (error_callback_) {
            error_callback_("Failed to write data");
        }
    }
}

} // namespace tcp_client
