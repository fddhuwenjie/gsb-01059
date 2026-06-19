#include <uv.h>
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include "protocol.h"

struct ClientData {
    uv_tcp_t* tcp;
    std::vector<uint8_t> receive_buffer;
    std::vector<uint8_t> storage;
    uint64_t read_delay_ms = 0;       // 下一次 read 请求的延迟
    bool drop_after_next_write = false;
    bool split_next_response = false;
    bool closing = false;
};

static std::map<uv_tcp_t*, ClientData*> g_clients;

struct WriteCtx {
    char* buffer;
};

static void OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    (void)handle;
    buf->base = new char[suggested_size];
    buf->len = suggested_size;
}

static void DoWrite(uv_tcp_t* tcp, const std::vector<uint8_t>& bytes,
                    void (*after)(uv_write_t*, int) = nullptr) {
    uv_write_t* req = new uv_write_t;
    char* buf = new char[bytes.size()];
    std::memcpy(buf, bytes.data(), bytes.size());
    WriteCtx* ctx = new WriteCtx{buf};
    req->data = ctx;

    uv_buf_t ub = uv_buf_init(buf, static_cast<unsigned int>(bytes.size()));

    auto default_after = [](uv_write_t* r, int status) {
        WriteCtx* c = static_cast<WriteCtx*>(r->data);
        if (status < 0) {
            std::cerr << "Write error: " << uv_strerror(status) << std::endl;
        }
        delete[] c->buffer;
        delete c;
        delete r;
    };

    uv_write(req, reinterpret_cast<uv_stream_t*>(tcp), &ub, 1,
             after ? after : default_after);
}

static void CloseClient(uv_tcp_t* tcp) {
    if (!tcp) return;
    ClientData* c = static_cast<ClientData*>(tcp->data);
    if (c && c->closing) return;
    if (c) c->closing = true;
    uv_close(reinterpret_cast<uv_handle_t*>(tcp), [](uv_handle_t* h) {
        uv_tcp_t* t = reinterpret_cast<uv_tcp_t*>(h);
        ClientData* d = static_cast<ClientData*>(h->data);
        g_clients.erase(t);
        delete d;
        delete t;
    });
}

struct DelayedReadCtx {
    uv_tcp_t* tcp;
    std::vector<uint8_t> response_bytes;
    bool split;
    uv_timer_t* timer;
};

static void SendBytesPossiblySplit(uv_tcp_t* tcp,
                                   const std::vector<uint8_t>& bytes,
                                   bool split) {
    if (!split || bytes.size() < 4) {
        DoWrite(tcp, bytes);
        return;
    }
    size_t mid = bytes.size() / 2;
    std::vector<uint8_t> first(bytes.begin(), bytes.begin() + mid);
    std::vector<uint8_t> second(bytes.begin() + mid, bytes.end());
    DoWrite(tcp, first);

    // 第二片用一个 50ms 定时器再发出去，制造真实"半包"
    uv_timer_t* timer = new uv_timer_t;
    uv_timer_init(uv_default_loop(), timer);
    auto* ctx = new std::pair<uv_tcp_t*, std::vector<uint8_t>>(tcp, std::move(second));
    timer->data = ctx;
    uv_timer_start(timer, [](uv_timer_t* t) {
        auto* c = static_cast<std::pair<uv_tcp_t*, std::vector<uint8_t>>*>(t->data);
        if (g_clients.count(c->first)) {
            DoWrite(c->first, c->second);
        }
        uv_timer_stop(t);
        uv_close(reinterpret_cast<uv_handle_t*>(t), [](uv_handle_t* h) {
            delete reinterpret_cast<uv_timer_t*>(h);
        });
        delete c;
    }, 50, 0);
}

static void HandleMessage(uv_tcp_t* tcp, const tcp_protocol::Message& msg) {
    ClientData* client = static_cast<ClientData*>(tcp->data);

    switch (msg.func) {
        case tcp_protocol::FUNC_READ_REQUEST: {
            std::cout << "Received read request" << std::endl;
            tcp_protocol::Message resp = tcp_protocol::CreateReadResponse(client->storage);
            std::vector<uint8_t> bytes = tcp_protocol::EncodeMessage(resp);

            bool split = client->split_next_response;
            client->split_next_response = false;

            if (client->read_delay_ms > 0) {
                uint64_t delay = client->read_delay_ms;
                client->read_delay_ms = 0;

                uv_timer_t* timer = new uv_timer_t;
                uv_timer_init(uv_default_loop(), timer);
                auto* ctx = new DelayedReadCtx{tcp, std::move(bytes), split, timer};
                timer->data = ctx;
                uv_timer_start(timer, [](uv_timer_t* t) {
                    auto* c = static_cast<DelayedReadCtx*>(t->data);
                    if (g_clients.count(c->tcp)) {
                        SendBytesPossiblySplit(c->tcp, c->response_bytes, c->split);
                    }
                    uv_timer_stop(t);
                    uv_close(reinterpret_cast<uv_handle_t*>(t), [](uv_handle_t* h) {
                        delete reinterpret_cast<uv_timer_t*>(h);
                    });
                    delete c;
                }, delay, 0);
            } else {
                SendBytesPossiblySplit(tcp, bytes, split);
            }
            break;
        }

        case tcp_protocol::FUNC_WRITE_REQUEST: {
            std::cout << "Received write request, data size: " << msg.data.size() << std::endl;

            // 解析控制前缀
            std::string s(msg.data.begin(), msg.data.end());
            bool drop_after = false;

            auto starts_with = [](const std::string& a, const std::string& p) {
                return a.size() >= p.size() && std::equal(p.begin(), p.end(), a.begin());
            };

            if (starts_with(s, "__DELAY:")) {
                size_t end = s.find("__", 8);
                if (end != std::string::npos) {
                    uint64_t ms = std::strtoull(s.substr(8, end - 8).c_str(), nullptr, 10);
                    client->read_delay_ms = ms;
                    std::cout << "  -> next read will delay " << ms << "ms" << std::endl;
                }
            } else if (starts_with(s, "__SPLIT__")) {
                client->split_next_response = true;
                std::cout << "  -> next response will be split" << std::endl;
            } else if (starts_with(s, "__DROP_AFTER_RESP__")) {
                drop_after = true;
                std::cout << "  -> server will drop after this write response" << std::endl;
            } else if (starts_with(s, "__DROP__")) {
                // 直接立刻断开，不发响应，让客户端写/读都因为连接断开而失败
                std::cout << "  -> server drops connection immediately" << std::endl;
                CloseClient(tcp);
                return;
            } else if (starts_with(s, "__NORESP__")) {
                // 不发送响应，用于触发客户端超时
                std::cout << "  -> server will NOT respond (trigger client timeout)" << std::endl;
                client->storage = msg.data;
                return;
            } else {
                // 普通写入：保存数据
                client->storage = msg.data;
            }

            tcp_protocol::Message resp =
                tcp_protocol::CreateWriteResponse(static_cast<uint16_t>(msg.data.size()));
            std::vector<uint8_t> bytes = tcp_protocol::EncodeMessage(resp);
            DoWrite(tcp, bytes);

            if (drop_after) {
                // 等 50ms 让响应送出去后再断开
                uv_timer_t* timer = new uv_timer_t;
                uv_timer_init(uv_default_loop(), timer);
                timer->data = tcp;
                uv_timer_start(timer, [](uv_timer_t* t) {
                    uv_tcp_t* target = static_cast<uv_tcp_t*>(t->data);
                    if (g_clients.count(target)) {
                        std::cout << "  -> closing client (DROP)" << std::endl;
                        CloseClient(target);
                    }
                    uv_timer_stop(t);
                    uv_close(reinterpret_cast<uv_handle_t*>(t), [](uv_handle_t* h) {
                        delete reinterpret_cast<uv_timer_t*>(h);
                    });
                }, 50, 0);
            }
            break;
        }

        default:
            std::cerr << "Unknown message type: " << msg.func << std::endl;
            break;
    }
}

static void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    ClientData* client = static_cast<ClientData*>(stream->data);

    if (nread < 0) {
        if (buf->base) delete[] buf->base;
        if (nread != UV_EOF) {
            std::cerr << "Read error: " << uv_strerror(nread) << std::endl;
        }
        CloseClient(reinterpret_cast<uv_tcp_t*>(stream));
        return;
    }

    if (nread == 0) {
        if (buf->base) delete[] buf->base;
        return;
    }

    size_t old_size = client->receive_buffer.size();
    client->receive_buffer.resize(old_size + nread);
    std::memcpy(client->receive_buffer.data() + old_size, buf->base, nread);
    if (buf->base) delete[] buf->base;

    while (client->receive_buffer.size() >= tcp_protocol::HEADER_SIZE) {
        uint16_t length = client->receive_buffer[4] |
                          (client->receive_buffer[5] << 8);

        if (length < tcp_protocol::HEADER_SIZE) {
            std::cerr << "Invalid length, dropping client" << std::endl;
            CloseClient(reinterpret_cast<uv_tcp_t*>(stream));
            return;
        }

        if (client->receive_buffer.size() < length) {
            break;
        }

        tcp_protocol::Message msg;
        if (tcp_protocol::DecodeMessage(client->receive_buffer.data(), length, msg)) {
            std::vector<uint8_t> rest(client->receive_buffer.begin() + length,
                                      client->receive_buffer.end());
            client->receive_buffer.swap(rest);
            HandleMessage(reinterpret_cast<uv_tcp_t*>(stream), msg);
        } else {
            std::cerr << "Failed to decode message" << std::endl;
            client->receive_buffer.clear();
            CloseClient(reinterpret_cast<uv_tcp_t*>(stream));
            return;
        }
    }
}

static void OnNewConnection(uv_stream_t* server, int status) {
    if (status < 0) {
        std::cerr << "New connection error: " << uv_strerror(status) << std::endl;
        return;
    }

    uv_tcp_t* client_tcp = new uv_tcp_t;
    uv_tcp_init(server->loop, client_tcp);

    ClientData* client = new ClientData;
    client->tcp = client_tcp;
    client_tcp->data = client;
    g_clients[client_tcp] = client;

    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(client_tcp)) == 0) {
        std::cout << "New client connected" << std::endl;
        uv_read_start(reinterpret_cast<uv_stream_t*>(client_tcp), OnAlloc, OnRead);
    } else {
        CloseClient(client_tcp);
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
