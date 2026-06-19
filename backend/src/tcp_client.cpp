#include "tcp_client.h"
#include <iostream>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <algorithm>

namespace tcp_client {

namespace {
constexpr uint64_t kDefaultTimeoutMs = 5000;
constexpr size_t kMaxReceiveBuffer = 64 * 1024;
}

TcpClient::TcpClient()
    : loop_(nullptr)
    , tcp_(nullptr)
    , async_(nullptr)
    , getaddrinfo_req_(nullptr)
    , reconnect_timer_(nullptr)
    , state_(ConnectionState::Disconnected)
    , should_stop_(false)
    , port_(0)
    , request_timeout_ms_(kDefaultTimeoutMs)
    , request_seq_(0)
    , auto_reconnect_(false)
    , reconnect_max_(0)
    , reconnect_attempts_(0)
    , reconnect_backoff_ms_(500)
    , async_closed_(false)
    , reconnect_timer_closed_(false)
{
    loop_ = uv_loop_new();
    if (!loop_) {
        throw std::runtime_error("Failed to create UV loop");
    }

    async_ = new uv_async_t;
    if (uv_async_init(loop_, async_, AsyncCallback) != 0) {
        delete async_;
        async_ = nullptr;
        uv_loop_delete(loop_);
        loop_ = nullptr;
        throw std::runtime_error("Failed to initialize async handle");
    }
    async_->data = this;

    reconnect_timer_ = new uv_timer_t;
    if (uv_timer_init(loop_, reconnect_timer_) != 0) {
        delete reconnect_timer_;
        reconnect_timer_ = nullptr;
    } else {
        reconnect_timer_->data = this;
    }
}

TcpClient::~TcpClient() {
    // 通知用户线程：loop 即将停止
    should_stop_.store(true);
    if (loop_ && async_ && !async_closed_) {
        uv_async_send(async_);
    }
    if (loop_) {
        uv_stop(loop_);
    }

    if (loop_) {
        // 1. 关闭所有 pending request 的 timer
        for (auto* req : pending_requests_) {
            if (req && req->timer) {
                uv_timer_stop(req->timer);
                uv_close(reinterpret_cast<uv_handle_t*>(req->timer),
                         [](uv_handle_t* h) { delete reinterpret_cast<uv_timer_t*>(h); });
                req->timer = nullptr;
            }
        }

        // 2. 关闭主 handle
        if (reconnect_timer_ && !reconnect_timer_closed_) {
            reconnect_timer_closed_ = true;
            uv_timer_stop(reconnect_timer_);
            uv_close(reinterpret_cast<uv_handle_t*>(reconnect_timer_), OnTimerClose);
        }
        if (async_ && !async_closed_) {
            async_closed_ = true;
            uv_close(reinterpret_cast<uv_handle_t*>(async_), OnAsyncClose);
        }
        if (tcp_) {
            uv_tcp_t* h = tcp_;
            tcp_ = nullptr;
            uv_close(reinterpret_cast<uv_handle_t*>(h), OnTcpClose);
        }

        // 3. walk 关闭其它残留 handle（连接 req 等内部资源由 libuv 管理；
        //    定时器 / write_req 等已托管）
        uv_walk(loop_, [](uv_handle_t* h, void*) {
            if (!uv_is_closing(h)) {
                uv_close(h, nullptr);
            }
        }, nullptr);

        // 4. drain：用 NOWAIT 反复 spin，直到所有 handle close cb 触发完毕。
        //    uv_stop 不影响 close cb 的执行；但会让 uv_run 早退，
        //    所以这里循环到 alive == 0。
        for (int i = 0; i < 1000; ++i) {
            uv_run(loop_, UV_RUN_NOWAIT);
            if (uv_loop_alive(loop_) == 0) break;
        }

        uv_loop_delete(loop_);
        loop_ = nullptr;
    }

    for (auto* req : pending_requests_) {
        delete req;
    }
    pending_requests_.clear();
}

void TcpClient::SetState(ConnectionState s) {
    state_.store(s);
}

ConnectionState TcpClient::GetState() const {
    return state_.load();
}

bool TcpClient::IsConnected() const {
    return state_.load() == ConnectionState::Connected;
}

void TcpClient::SetErrorCallback(ErrorCallback callback) {
    error_callback_ = std::move(callback);
}

void TcpClient::SetDisconnectCallback(DisconnectCallback callback) {
    disconnect_callback_ = std::move(callback);
}

void TcpClient::SetRequestTimeout(uint64_t timeout_ms) {
    request_timeout_ms_ = timeout_ms;
}

void TcpClient::EnableAutoReconnect(bool enable, uint32_t max_attempts, uint64_t backoff_ms) {
    auto_reconnect_ = enable;
    reconnect_max_ = max_attempts;
    reconnect_backoff_ms_ = backoff_ms;
    reconnect_attempts_ = 0;
}

void TcpClient::Run() {
    should_stop_.store(false);
    uv_run(loop_, UV_RUN_DEFAULT);
}

void TcpClient::Stop() {
    should_stop_.store(true);
    if (loop_) {
        uv_stop(loop_);
        if (async_ && !async_closed_) {
            uv_async_send(async_);
        }
    }
}

void TcpClient::ReportError(const std::string& msg) {
    if (error_callback_) {
        error_callback_(msg);
    }
}

void TcpClient::EnqueueTask(PendingTask task) {
    {
        std::lock_guard<std::mutex> lk(task_mutex_);
        task_queue_.push_back(std::move(task));
    }
    NotifyAsync();
}

void TcpClient::NotifyAsync() {
    if (async_ && !async_closed_) {
        uv_async_send(async_);
    }
}

bool TcpClient::Connect(const std::string& host, uint16_t port, ConnectCallback callback) {
    PendingTask t;
    t.type = TaskType::Connect;
    t.host = host;
    t.port = port;
    t.connect_cb = std::move(callback);
    EnqueueTask(std::move(t));
    return true;
}

void TcpClient::Disconnect() {
    PendingTask t;
    t.type = TaskType::Disconnect;
    EnqueueTask(std::move(t));
}

void TcpClient::ReadData(ReadCallback callback) {
    PendingTask t;
    t.type = TaskType::Read;
    t.read_cb = std::move(callback);
    EnqueueTask(std::move(t));
}

void TcpClient::WriteData(const std::vector<uint8_t>& data, WriteCallback callback) {
    PendingTask t;
    t.type = TaskType::Write;
    t.payload = data;
    t.write_cb = std::move(callback);
    EnqueueTask(std::move(t));
}

void TcpClient::AsyncCallback(uv_async_t* handle) {
    TcpClient* client = static_cast<TcpClient*>(handle->data);
    if (!client) return;
    client->ProcessPendingTasks();

    if (client->should_stop_.load()) {
        uv_stop(client->loop_);
    }
}

void TcpClient::ProcessPendingTasks() {
    std::deque<PendingTask> tasks;
    {
        std::lock_guard<std::mutex> lk(task_mutex_);
        tasks.swap(task_queue_);
    }

    while (!tasks.empty()) {
        PendingTask task = std::move(tasks.front());
        tasks.pop_front();

        switch (task.type) {
            case TaskType::Connect:
                DoConnect(task.host, task.port, std::move(task.connect_cb));
                break;
            case TaskType::Disconnect:
                auto_reconnect_ = false;
                DoDisconnect(true, ErrorCode::Cancelled, "User disconnect");
                break;
            case TaskType::Read: {
                if (state_.load() != ConnectionState::Connected) {
                    if (task.read_cb) task.read_cb(false, {});
                    ReportError("ReadData: not connected");
                    break;
                }
                PendingRequest* pr = MakePendingRequest(tcp_protocol::FUNC_READ_RESPONSE);
                pr->read_cb = std::move(task.read_cb);
                pending_requests_.push_back(pr);
                tcp_protocol::Message msg = tcp_protocol::CreateReadRequest();
                DoSendMessage(msg);
                break;
            }
            case TaskType::Write: {
                if (state_.load() != ConnectionState::Connected) {
                    if (task.write_cb) task.write_cb(false, 0);
                    ReportError("WriteData: not connected");
                    break;
                }
                PendingRequest* pr = MakePendingRequest(tcp_protocol::FUNC_WRITE_RESPONSE);
                pr->write_cb = std::move(task.write_cb);
                pending_requests_.push_back(pr);
                tcp_protocol::Message msg = tcp_protocol::CreateWriteRequest(task.payload);
                DoSendMessage(msg);
                break;
            }
        }
    }
}

TcpClient::PendingRequest* TcpClient::MakePendingRequest(uint16_t expected_func) {
    PendingRequest* pr = new PendingRequest;
    pr->expected_func = expected_func;
    pr->owner = this;
    pr->seq = ++request_seq_;
    pr->done = false;
    pr->timer = nullptr;

    if (request_timeout_ms_ > 0) {
        pr->timer = new uv_timer_t;
        if (uv_timer_init(loop_, pr->timer) == 0) {
            pr->timer->data = pr;
            uv_timer_start(pr->timer, OnRequestTimeout, request_timeout_ms_, 0);
        } else {
            delete pr->timer;
            pr->timer = nullptr;
        }
    }
    return pr;
}

void TcpClient::CompletePendingRequest(PendingRequest* req, bool success,
                                       const std::vector<uint8_t>& data, uint16_t dataSize) {
    if (!req || req->done) return;
    req->done = true;

    if (req->timer) {
        uv_timer_stop(req->timer);
        uv_close(reinterpret_cast<uv_handle_t*>(req->timer), [](uv_handle_t* h) {
            delete reinterpret_cast<uv_timer_t*>(h);
        });
        req->timer = nullptr;
    }

    if (req->expected_func == tcp_protocol::FUNC_READ_RESPONSE) {
        if (req->read_cb) req->read_cb(success, data);
    } else if (req->expected_func == tcp_protocol::FUNC_WRITE_RESPONSE) {
        if (req->write_cb) req->write_cb(success, dataSize);
    }
}

void TcpClient::OnRequestTimeout(uv_timer_t* timer) {
    PendingRequest* req = static_cast<PendingRequest*>(timer->data);
    if (!req || req->done) return;
    TcpClient* client = req->owner;

    auto it = std::find(client->pending_requests_.begin(), client->pending_requests_.end(), req);
    if (it != client->pending_requests_.end()) {
        client->pending_requests_.erase(it);
    }

    client->ReportError("Request timeout (seq=" + std::to_string(req->seq) + ")");
    client->CompletePendingRequest(req, false, {}, 0);
    delete req;
}

void TcpClient::DoConnect(const std::string& host, uint16_t port, ConnectCallback cb) {
    ConnectionState st = state_.load();
    if (st == ConnectionState::Connected || st == ConnectionState::Connecting ||
        st == ConnectionState::Resolving) {
        ReportError("Already connected or connecting");
        if (cb) cb(false);
        return;
    }

    host_ = host;
    port_ = port;
    if (cb) connect_callback_ = std::move(cb);

    receive_buffer_.clear();

    tcp_ = new uv_tcp_t;
    if (uv_tcp_init(loop_, tcp_) != 0) {
        delete tcp_;
        tcp_ = nullptr;
        ReportError("Failed to init tcp handle");
        if (connect_callback_) { connect_callback_(false); connect_callback_ = nullptr; }
        SetState(ConnectionState::Disconnected);
        if (auto_reconnect_) ScheduleReconnect();
        return;
    }
    tcp_->data = this;

    SetState(ConnectionState::Resolving);

    getaddrinfo_req_ = new uv_getaddrinfo_t;
    getaddrinfo_req_->data = this;

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
        ReportError(std::string("getaddrinfo failed: ") + uv_strerror(r));
        if (connect_callback_) { connect_callback_(false); connect_callback_ = nullptr; }
        CleanupTcpHandle();
        SetState(ConnectionState::Disconnected);
        if (auto_reconnect_) ScheduleReconnect();
    }
}

void TcpClient::OnGetAddrInfo(uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
    TcpClient* client = static_cast<TcpClient*>(req->data);
    delete req;
    client->getaddrinfo_req_ = nullptr;

    if (status < 0 || !res) {
        client->ReportError(std::string("DNS resolution failed: ") + uv_strerror(status));
        if (client->connect_callback_) {
            client->connect_callback_(false);
            client->connect_callback_ = nullptr;
        }
        if (res) uv_freeaddrinfo(res);
        client->CleanupTcpHandle();
        client->SetState(ConnectionState::Disconnected);
        if (client->auto_reconnect_) client->ScheduleReconnect();
        return;
    }

    client->SetState(ConnectionState::Connecting);

    uv_connect_t* connect_req = new uv_connect_t;
    connect_req->data = client;

    int r = uv_tcp_connect(connect_req, client->tcp_, res->ai_addr, OnConnect);
    uv_freeaddrinfo(res);
    if (r != 0) {
        delete connect_req;
        client->ReportError(std::string("uv_tcp_connect failed: ") + uv_strerror(r));
        if (client->connect_callback_) {
            client->connect_callback_(false);
            client->connect_callback_ = nullptr;
        }
        client->CleanupTcpHandle();
        client->SetState(ConnectionState::Disconnected);
        if (client->auto_reconnect_) client->ScheduleReconnect();
    }
}

void TcpClient::OnConnect(uv_connect_t* req, int status) {
    TcpClient* client = static_cast<TcpClient*>(req->data);
    delete req;

    if (status < 0) {
        client->ReportError(std::string("Connection failed: ") + uv_strerror(status));
        if (client->connect_callback_) {
            client->connect_callback_(false);
            client->connect_callback_ = nullptr;
        }
        client->CleanupTcpHandle();
        client->SetState(ConnectionState::Disconnected);
        if (client->auto_reconnect_) client->ScheduleReconnect();
        return;
    }

    client->SetState(ConnectionState::Connected);
    client->reconnect_attempts_ = 0;
    client->StartRead();

    if (client->connect_callback_) {
        client->connect_callback_(true);
        client->connect_callback_ = nullptr;
    }
}

void TcpClient::StartRead() {
    if (!tcp_) return;
    int r = uv_read_start(reinterpret_cast<uv_stream_t*>(tcp_), OnAlloc, OnRead);
    if (r != 0) {
        ReportError(std::string("uv_read_start failed: ") + uv_strerror(r));
        DoDisconnect(true, ErrorCode::ConnectFailed, "read_start failed");
    }
}

void TcpClient::StopRead() {
    if (tcp_) {
        uv_read_stop(reinterpret_cast<uv_stream_t*>(tcp_));
    }
}

void TcpClient::OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    (void)handle;
    buf->base = new char[suggested_size];
    buf->len = suggested_size;
}

void TcpClient::OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    TcpClient* client = static_cast<TcpClient*>(stream->data);

    if (nread < 0) {
        if (buf->base) delete[] buf->base;
        if (nread != UV_EOF) {
            client->ReportError(std::string("Read error: ") + uv_strerror(nread));
        }
        client->DoDisconnect(true, ErrorCode::PeerClosed,
                              nread == UV_EOF ? "Peer closed" : uv_strerror(nread));
        return;
    }

    if (nread == 0) {
        if (buf->base) delete[] buf->base;
        return;
    }

    if (client->receive_buffer_.size() + static_cast<size_t>(nread) > kMaxReceiveBuffer) {
        if (buf->base) delete[] buf->base;
        client->ReportError("Receive buffer overflow");
        client->DoDisconnect(true, ErrorCode::ProtocolMismatch, "rx overflow");
        return;
    }

    size_t old_size = client->receive_buffer_.size();
    client->receive_buffer_.resize(old_size + static_cast<size_t>(nread));
    std::memcpy(client->receive_buffer_.data() + old_size, buf->base, nread);
    if (buf->base) delete[] buf->base;

    while (client->receive_buffer_.size() >= tcp_protocol::HEADER_SIZE) {
        uint16_t length = static_cast<uint16_t>(client->receive_buffer_[4]) |
                          (static_cast<uint16_t>(client->receive_buffer_[5]) << 8);

        if (length < tcp_protocol::HEADER_SIZE) {
            client->ReportError("Invalid frame length: " + std::to_string(length));
            client->receive_buffer_.clear();
            client->DoDisconnect(true, ErrorCode::DecodeError, "invalid length");
            return;
        }

        if (client->receive_buffer_.size() < length) {
            break;
        }

        tcp_protocol::Message msg;
        if (tcp_protocol::DecodeMessage(client->receive_buffer_.data(), length, msg)) {
            std::vector<uint8_t> rest(client->receive_buffer_.begin() + length,
                                      client->receive_buffer_.end());
            client->receive_buffer_.swap(rest);
            client->HandleMessage(msg);
        } else {
            client->ReportError("Failed to decode message");
            client->receive_buffer_.clear();
            client->DoDisconnect(true, ErrorCode::DecodeError, "decode failed");
            return;
        }
    }
}

void TcpClient::HandleMessage(const tcp_protocol::Message& msg) {
    if (msg.func != tcp_protocol::FUNC_READ_RESPONSE &&
        msg.func != tcp_protocol::FUNC_WRITE_RESPONSE) {
        ReportError("Unexpected response func: " + std::to_string(msg.func));
        return;
    }

    PendingRequest* matched = nullptr;
    for (auto it = pending_requests_.begin(); it != pending_requests_.end(); ++it) {
        if ((*it)->expected_func == msg.func && !(*it)->done) {
            matched = *it;
            pending_requests_.erase(it);
            break;
        }
    }

    if (!matched) {
        ReportError("Received response without matching request (func=" +
                    std::to_string(msg.func) + ")");
        return;
    }

    CompletePendingRequest(matched, true, msg.data, msg.dataSize);
    delete matched;
}

void TcpClient::OnWrite(uv_write_t* req, int status) {
    WriteRequestData* data = static_cast<WriteRequestData*>(req->data);
    TcpClient* client = data->client;
    if (data->buffer) delete[] data->buffer;
    delete data;
    delete req;

    if (status < 0) {
        client->ReportError(std::string("Write error: ") + uv_strerror(status));
        client->DoDisconnect(true, ErrorCode::WriteFailed, uv_strerror(status));
    }
}

void TcpClient::DoSendMessage(const tcp_protocol::Message& msg) {
    if (state_.load() != ConnectionState::Connected || !tcp_) {
        ReportError("DoSendMessage: not connected");
        return;
    }

    std::vector<uint8_t> buffer = tcp_protocol::EncodeMessage(msg);

    char* write_buf = new char[buffer.size()];
    std::memcpy(write_buf, buffer.data(), buffer.size());

    uv_write_t* write_req = new uv_write_t;
    WriteRequestData* data = new WriteRequestData;
    data->client = this;
    data->buffer = write_buf;
    write_req->data = data;

    uv_buf_t buf = uv_buf_init(write_buf, static_cast<unsigned int>(buffer.size()));
    int r = uv_write(write_req, reinterpret_cast<uv_stream_t*>(tcp_), &buf, 1, OnWrite);
    if (r != 0) {
        delete[] write_buf;
        delete data;
        delete write_req;
        ReportError(std::string("uv_write failed: ") + uv_strerror(r));
        DoDisconnect(true, ErrorCode::WriteFailed, uv_strerror(r));
    }
}

void TcpClient::FailAllPending(ErrorCode code, const std::string& reason) {
    (void)code;
    auto pending = std::move(pending_requests_);
    pending_requests_.clear();
    for (auto* req : pending) {
        if (!req->done) {
            ReportError("Pending request failed: " + reason);
            CompletePendingRequest(req, false, {}, 0);
        }
        delete req;
    }
}

void TcpClient::CleanupTcpHandle() {
    if (tcp_) {
        uv_tcp_t* h = tcp_;
        tcp_ = nullptr;
        uv_close(reinterpret_cast<uv_handle_t*>(h), OnTcpClose);
    }
}

void TcpClient::DoDisconnect(bool notify_user, ErrorCode code, const std::string& reason) {
    ConnectionState st = state_.load();
    if (st == ConnectionState::Disconnected || st == ConnectionState::Closing) {
        // 仍然清理可能残留的 pending requests
        FailAllPending(code, reason);
        return;
    }

    SetState(ConnectionState::Closing);
    StopRead();
    FailAllPending(code, reason);
    receive_buffer_.clear();
    CleanupTcpHandle();
    SetState(ConnectionState::Disconnected);

    if (notify_user && disconnect_callback_) {
        disconnect_callback_();
    }

    if (auto_reconnect_ && code != ErrorCode::Cancelled) {
        ScheduleReconnect();
    }
}

void TcpClient::ScheduleReconnect() {
    if (!reconnect_timer_ || reconnect_timer_closed_) return;
    if (reconnect_max_ > 0 && reconnect_attempts_ >= reconnect_max_) {
        ReportError("Reconnect attempts exhausted");
        auto_reconnect_ = false;
        return;
    }
    reconnect_attempts_++;
    uint64_t delay = reconnect_backoff_ms_ * reconnect_attempts_;
    reconnect_timer_->data = this;
    uv_timer_start(reconnect_timer_, OnReconnectTimer, delay, 0);
}

void TcpClient::OnReconnectTimer(uv_timer_t* timer) {
    TcpClient* client = static_cast<TcpClient*>(timer->data);
    uv_timer_stop(timer);
    if (!client->auto_reconnect_) return;
    client->ReportError("Reconnect attempt #" + std::to_string(client->reconnect_attempts_));
    client->DoConnect(client->host_, client->port_, nullptr);
}

void TcpClient::OnTcpClose(uv_handle_t* handle) {
    delete reinterpret_cast<uv_tcp_t*>(handle);
}

void TcpClient::OnAsyncClose(uv_handle_t* handle) {
    delete reinterpret_cast<uv_async_t*>(handle);
}

void TcpClient::OnTimerClose(uv_handle_t* handle) {
    delete reinterpret_cast<uv_timer_t*>(handle);
}

} // namespace tcp_client
