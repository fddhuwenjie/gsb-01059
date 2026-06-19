#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <uv.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <deque>
#include <mutex>
#include <atomic>
#include <cstdint>
#include "protocol.h"

namespace tcp_client {

enum class ConnectionState {
    Disconnected = 0,
    Resolving,
    Connecting,
    Connected,
    Closing,
};

enum class ErrorCode {
    Ok = 0,
    NotConnected,
    Timeout,
    PeerClosed,
    DecodeError,
    ProtocolMismatch,
    WriteFailed,
    ConnectFailed,
    Cancelled,
};

class TcpClient {
public:
    using ReadCallback = std::function<void(bool success, const std::vector<uint8_t>& data)>;
    using WriteCallback = std::function<void(bool success, uint16_t dataSize)>;
    using ConnectCallback = std::function<void(bool success)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    using DisconnectCallback = std::function<void()>;

    TcpClient();
    ~TcpClient();

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    bool Connect(const std::string& host, uint16_t port, ConnectCallback callback = nullptr);

    void Disconnect();

    bool IsConnected() const;
    ConnectionState GetState() const;

    void ReadData(ReadCallback callback);
    void WriteData(const std::vector<uint8_t>& data, WriteCallback callback);

    void SetErrorCallback(ErrorCallback callback);
    void SetDisconnectCallback(DisconnectCallback callback);

    void SetRequestTimeout(uint64_t timeout_ms);

    void EnableAutoReconnect(bool enable, uint32_t max_attempts = 5, uint64_t backoff_ms = 500);

    void Run();
    void Stop();

private:
    enum class TaskType { Read, Write, Disconnect, Connect };

    struct PendingRequest {
        uint16_t expected_func;
        ReadCallback read_cb;
        WriteCallback write_cb;
        uv_timer_t* timer;
        TcpClient* owner;
        uint64_t seq;
        bool done;
    };

    struct PendingTask {
        TaskType type;
        std::vector<uint8_t> payload;
        ReadCallback read_cb;
        WriteCallback write_cb;
        ConnectCallback connect_cb;
        std::string host;
        uint16_t port;
    };

    struct WriteRequestData {
        TcpClient* client;
        char* buffer;
    };

    static void OnConnect(uv_connect_t* req, int status);
    static void OnGetAddrInfo(uv_getaddrinfo_t* req, int status, struct addrinfo* res);
    static void OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
    static void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
    static void OnWrite(uv_write_t* req, int status);
    static void OnTcpClose(uv_handle_t* handle);
    static void OnAsyncClose(uv_handle_t* handle);
    static void OnTimerClose(uv_handle_t* handle);
    static void AsyncCallback(uv_async_t* handle);
    static void OnRequestTimeout(uv_timer_t* timer);
    static void OnReconnectTimer(uv_timer_t* timer);

    void ProcessPendingTasks();
    void HandleMessage(const tcp_protocol::Message& msg);
    void DoSendMessage(const tcp_protocol::Message& msg);
    void DoConnect(const std::string& host, uint16_t port, ConnectCallback cb);
    void DoDisconnect(bool notify_user, ErrorCode code, const std::string& reason);
    void StartRead();
    void StopRead();
    void FailAllPending(ErrorCode code, const std::string& reason);
    void ScheduleReconnect();
    void CleanupTcpHandle();
    void ReportError(const std::string& msg);
    void SetState(ConnectionState s);
    void EnqueueTask(PendingTask task);
    void NotifyAsync();
    PendingRequest* MakePendingRequest(uint16_t expected_func);
    void CompletePendingRequest(PendingRequest* req, bool success, const std::vector<uint8_t>& data, uint16_t dataSize);

    uv_loop_t* loop_;
    uv_tcp_t* tcp_;
    uv_async_t* async_;
    uv_getaddrinfo_t* getaddrinfo_req_;
    uv_timer_t* reconnect_timer_;

    std::atomic<ConnectionState> state_;
    std::atomic<bool> should_stop_;

    std::string host_;
    uint16_t port_;

    std::deque<PendingRequest*> pending_requests_;

    std::vector<uint8_t> receive_buffer_;

    std::mutex task_mutex_;
    std::deque<PendingTask> task_queue_;

    ConnectCallback connect_callback_;
    ErrorCallback error_callback_;
    DisconnectCallback disconnect_callback_;

    uint64_t request_timeout_ms_;
    uint64_t request_seq_;

    bool auto_reconnect_;
    uint32_t reconnect_max_;
    uint32_t reconnect_attempts_;
    uint64_t reconnect_backoff_ms_;

    bool async_closed_;
    bool reconnect_timer_closed_;
};

} // namespace tcp_client

#endif // TCP_CLIENT_H
