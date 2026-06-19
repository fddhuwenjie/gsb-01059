#include "tcp_client.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <string>
#include <cstring>
#include <functional>

using namespace std::chrono_literals;

namespace {

struct TestResult {
    std::string name;
    bool passed;
    std::string detail;
};

class WaitFlag {
public:
    void Set() {
        std::lock_guard<std::mutex> lk(m_);
        flag_ = true;
        cv_.notify_all();
    }
    bool Wait(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, timeout, [&] { return flag_; });
    }
    void Reset() {
        std::lock_guard<std::mutex> lk(m_);
        flag_ = false;
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    bool flag_ = false;
};

std::vector<uint8_t> StrToBytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::string BytesToStr(const std::vector<uint8_t>& b) {
    return std::string(b.begin(), b.end());
}

bool WaitConnected(tcp_client::TcpClient& c, std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (c.IsConnected()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return c.IsConnected();
}

// ====== Test 1: 连续多次读写，验证请求与响应的边界匹配 ======
TestResult Test_ContinuousReadWrite(const std::string& host, uint16_t port) {
    TestResult r{"ContinuousReadWrite", false, ""};
    tcp_client::TcpClient client;
    client.SetRequestTimeout(3000);
    client.SetErrorCallback([&](const std::string& e) {
        std::cerr << "  [err] " << e << std::endl;
    });

    WaitFlag connected;
    client.Connect(host, port, [&](bool ok) {
        if (ok) connected.Set();
    });

    std::thread loop([&] { client.Run(); });

    if (!connected.Wait(3000ms)) {
        client.Stop();
        if (loop.joinable()) loop.join();
        r.detail = "connect timeout";
        return r;
    }

    constexpr int N = 20;
    std::atomic<int> write_ok{0}, read_ok{0};
    std::atomic<int> done{0};
    std::vector<std::string> sent(N), got(N);
    WaitFlag all_done;

    for (int i = 0; i < N; ++i) {
        std::string s = "msg-" + std::to_string(i) + "-payload-XYZ";
        sent[i] = s;
        client.WriteData(StrToBytes(s), [&, i](bool ok, uint16_t) {
            if (ok) write_ok++;
        });
        client.ReadData([&, i](bool ok, const std::vector<uint8_t>& data) {
            if (ok) {
                read_ok++;
                got[i] = BytesToStr(data);
            }
            if (++done == N) all_done.Set();
        });
    }

    bool finished = all_done.Wait(8000ms);
    client.Stop();
    if (loop.joinable()) loop.join();

    if (!finished) {
        r.detail = "not all callbacks fired in time, done=" + std::to_string(done.load());
        return r;
    }
    if (write_ok != N || read_ok != N) {
        r.detail = "write_ok=" + std::to_string(write_ok) + " read_ok=" + std::to_string(read_ok);
        return r;
    }
    // 由于服务端是 storage = last_write，每次 read 拿到的是当时存储的内容；
    // 关键校验：每次 read 的回调被精确触发 1 次，没有错位/串线。
    // 进一步要求：每次拿到的数据必须等于"某个" sent 中存在的 payload，且不能为空。
    for (int i = 0; i < N; ++i) {
        if (got[i].empty()) {
            r.detail = "empty response at i=" + std::to_string(i);
            return r;
        }
    }
    r.passed = true;
    r.detail = std::to_string(N) + " round-trips OK";
    return r;
}

// ====== Test 2: 半包/分片接收 ======
TestResult Test_PartialFrame(const std::string& host, uint16_t port) {
    TestResult r{"PartialFrame", false, ""};
    tcp_client::TcpClient client;
    client.SetRequestTimeout(3000);
    client.SetErrorCallback([&](const std::string& e) {
        std::cerr << "  [err] " << e << std::endl;
    });

    WaitFlag connected;
    client.Connect(host, port, [&](bool ok) { if (ok) connected.Set(); });
    std::thread loop([&] { client.Run(); });

    if (!connected.Wait(3000ms)) {
        client.Stop();
        if (loop.joinable()) loop.join();
        r.detail = "connect timeout";
        return r;
    }

    // 步骤1：让服务端"标记"下一次响应要分片
    WaitFlag w1;
    client.WriteData(StrToBytes("__SPLIT__"), [&](bool, uint16_t) { w1.Set(); });
    if (!w1.Wait(3000ms)) {
        client.Stop(); loop.join(); r.detail = "split flag write timeout"; return r;
    }

    // 步骤2：写入业务数据
    std::string payload = "hello-half-packet-test-1234567890";
    WaitFlag w2;
    client.WriteData(StrToBytes(payload), [&](bool, uint16_t) { w2.Set(); });
    if (!w2.Wait(3000ms)) {
        client.Stop(); loop.join(); r.detail = "write timeout"; return r;
    }

    // 步骤3：读，服务端会把响应分两片（间隔 50ms）发出，验证客户端能正确拼接
    WaitFlag w3;
    std::string got;
    bool ok_flag = false;
    client.ReadData([&](bool ok, const std::vector<uint8_t>& data) {
        ok_flag = ok;
        got = BytesToStr(data);
        w3.Set();
    });
    if (!w3.Wait(3000ms)) {
        client.Stop(); loop.join(); r.detail = "read timeout"; return r;
    }

    client.Stop();
    if (loop.joinable()) loop.join();

    if (!ok_flag) {
        r.detail = "read failed";
        return r;
    }
    if (got != payload) {
        r.detail = "data mismatch: got=" + got;
        return r;
    }
    r.passed = true;
    r.detail = "split response reassembled OK";
    return r;
}

// ====== Test 3: 请求超时 ======
TestResult Test_RequestTimeout(const std::string& host, uint16_t port) {
    TestResult r{"RequestTimeout", false, ""};
    tcp_client::TcpClient client;
    client.SetRequestTimeout(800);  // 较短超时
    client.SetErrorCallback([&](const std::string& e) {
        std::cerr << "  [err] " << e << std::endl;
    });

    WaitFlag connected;
    client.Connect(host, port, [&](bool ok) { if (ok) connected.Set(); });
    std::thread loop([&] { client.Run(); });

    if (!connected.Wait(3000ms)) {
        client.Stop(); loop.join(); r.detail = "connect timeout"; return r;
    }

    // 让服务端对下一次 read 延迟 2000ms 响应
    WaitFlag w1;
    client.WriteData(StrToBytes("__DELAY:2000__"), [&](bool, uint16_t) { w1.Set(); });
    if (!w1.Wait(3000ms)) {
        client.Stop(); loop.join(); r.detail = "delay flag timeout"; return r;
    }

    WaitFlag w2;
    bool got_failure = false;
    auto t0 = std::chrono::steady_clock::now();
    client.ReadData([&](bool ok, const std::vector<uint8_t>&) {
        got_failure = !ok;
        w2.Set();
    });
    bool sig = w2.Wait(3000ms);
    auto elapsed = std::chrono::steady_clock::now() - t0;

    client.Stop();
    if (loop.joinable()) loop.join();

    if (!sig) { r.detail = "callback never fired"; return r; }
    if (!got_failure) { r.detail = "expected failure but succeeded"; return r; }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    if (ms > 1500) { r.detail = "timeout fired too late: " + std::to_string(ms) + "ms"; return r; }
    r.passed = true;
    r.detail = "timeout fired in " + std::to_string(ms) + "ms";
    return r;
}

// ====== Test 4: 断线后旧请求被失败回传 ======
TestResult Test_DisconnectFailsPending(const std::string& host, uint16_t port) {
    TestResult r{"DisconnectFailsPending", false, ""};
    tcp_client::TcpClient client;
    client.SetRequestTimeout(5000);
    client.SetErrorCallback([&](const std::string& e) {
        std::cerr << "  [err] " << e << std::endl;
    });

    WaitFlag connected;
    client.Connect(host, port, [&](bool ok) { if (ok) connected.Set(); });
    std::thread loop([&] { client.Run(); });

    if (!connected.Wait(3000ms)) {
        client.Stop(); loop.join(); r.detail = "connect timeout"; return r;
    }

    // 让服务端在收到下一次写之后断开连接
    // 服务端收到 __DROP__ 后立刻关闭，客户端 pending 的写和读都应该被失败回传
    WaitFlag w_write;
    bool write_failed = false;
    client.WriteData(StrToBytes("__DROP__"), [&](bool ok, uint16_t) {
        write_failed = !ok;
        w_write.Set();
    });

    // 同时发一个 read，期望也被失败
    WaitFlag w_read;
    bool read_failed = false;
    client.ReadData([&](bool ok, const std::vector<uint8_t>&) {
        read_failed = !ok;
        w_read.Set();
    });

    bool s1 = w_write.Wait(5000ms);
    bool s2 = w_read.Wait(5000ms);
    client.Stop();
    if (loop.joinable()) loop.join();

    if (!s1 || !s2) { r.detail = "callbacks didn't fire"; return r; }
    if (!write_failed || !read_failed) {
        r.detail = "expected both pending to fail (w=" + std::to_string(write_failed) +
                   " r=" + std::to_string(read_failed) + ")";
        return r;
    }

    r.passed = true;
    r.detail = "both pending requests got failure callback after server drop";
    return r;
}

// ====== Test 5: 自动重连后能恢复读写 ======
TestResult Test_AutoReconnect(const std::string& host, uint16_t port) {
    TestResult r{"AutoReconnect", false, ""};
    tcp_client::TcpClient client;
    client.SetRequestTimeout(3000);
    client.EnableAutoReconnect(true, 5, 200);

    std::atomic<int> reconnect_seen{0};
    client.SetDisconnectCallback([&] {
        std::cerr << "  [info] disconnected, will auto-reconnect" << std::endl;
    });
    client.SetErrorCallback([&](const std::string& e) {
        std::cerr << "  [err] " << e << std::endl;
        if (e.find("Reconnect attempt") != std::string::npos) reconnect_seen++;
    });

    WaitFlag connected;
    client.Connect(host, port, [&](bool ok) { if (ok) connected.Set(); });
    std::thread loop([&] { client.Run(); });

    if (!connected.Wait(3000ms)) {
        client.Stop(); loop.join(); r.detail = "initial connect timeout"; return r;
    }

    // 触发服务端断开（先发响应再 close，确保我们能拿到 write callback）
    WaitFlag w1;
    client.WriteData(StrToBytes("__DROP_AFTER_RESP__"), [&](bool, uint16_t) { w1.Set(); });
    if (!w1.Wait(3000ms)) {
        client.Stop(); loop.join(); r.detail = "drop write timeout"; return r;
    }

    // 等待自动重连（最多 4 秒）
    auto start = std::chrono::steady_clock::now();
    bool reconnected = false;
    while (std::chrono::steady_clock::now() - start < 5s) {
        if (client.IsConnected() && reconnect_seen.load() > 0) {
            // 至少经历过一次断线再连上
            reconnected = true;
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    if (!reconnected) {
        client.Stop(); loop.join(); r.detail = "did not reconnect in time"; return r;
    }

    // 重连后发一个新的写读，验证旧的请求/状态没有污染
    std::string fresh = "after-reconnect-payload";
    WaitFlag w2;
    bool wok = false;
    client.WriteData(StrToBytes(fresh), [&](bool ok, uint16_t) { wok = ok; w2.Set(); });
    if (!w2.Wait(3000ms)) {
        client.Stop(); loop.join(); r.detail = "post-reconnect write timeout"; return r;
    }

    WaitFlag w3;
    std::string got;
    bool rok = false;
    client.ReadData([&](bool ok, const std::vector<uint8_t>& data) {
        rok = ok;
        got = BytesToStr(data);
        w3.Set();
    });
    if (!w3.Wait(3000ms)) {
        client.Stop(); loop.join(); r.detail = "post-reconnect read timeout"; return r;
    }

    // 断开避免后台再触发新的重连
    client.EnableAutoReconnect(false);
    client.Disconnect();
    std::this_thread::sleep_for(200ms);
    client.Stop();
    if (loop.joinable()) loop.join();

    if (!wok || !rok) { r.detail = "post-reconnect rw failed"; return r; }
    if (got != fresh) { r.detail = "post-reconnect data mismatch: " + got; return r; }

    r.passed = true;
    r.detail = "reconnected and round-tripped OK";
    return r;
}

// ====== Test 0: 基础 happy path（兼容原 test.sh 期望的输出）======
TestResult Test_HappyPath(const std::string& host, uint16_t port) {
    TestResult r{"HappyPath", false, ""};
    tcp_client::TcpClient client;
    client.SetRequestTimeout(3000);
    client.SetErrorCallback([&](const std::string& e) {
        std::cerr << "  [err] " << e << std::endl;
    });

    WaitFlag connected;
    client.Connect(host, port, [&](bool ok) {
        if (ok) {
            std::cout << "✓ Connected successfully!" << std::endl;
            connected.Set();
        }
    });
    std::thread loop([&] { client.Run(); });

    if (!connected.Wait(3000ms)) {
        client.Stop(); loop.join(); r.detail = "connect timeout"; return r;
    }

    auto write_data = StrToBytes("Hello, World!");
    WaitFlag w_write;
    bool wok = false;
    client.WriteData(write_data, [&](bool ok, uint16_t sz) {
        wok = ok;
        if (ok) std::cout << "✓ Write successful! Data size: " << sz << " bytes" << std::endl;
        w_write.Set();
    });
    if (!w_write.Wait(3000ms)) {
        client.Stop(); loop.join(); r.detail = "write timeout"; return r;
    }

    WaitFlag w_read;
    std::vector<uint8_t> got;
    bool rok = false;
    client.ReadData([&](bool ok, const std::vector<uint8_t>& data) {
        rok = ok;
        got = data;
        if (ok) std::cout << "✓ Read successful! Data size: " << data.size() << " bytes" << std::endl;
        w_read.Set();
    });
    if (!w_read.Wait(3000ms)) {
        client.Stop(); loop.join(); r.detail = "read timeout"; return r;
    }

    client.Stop();
    if (loop.joinable()) loop.join();

    if (!wok || !rok) { r.detail = "rw failed"; return r; }
    if (got != write_data) { r.detail = "data mismatch"; return r; }

    r.passed = true;
    r.detail = "OK";
    return r;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string host = "test-server";
    uint16_t port = 8888;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = static_cast<uint16_t>(std::stoi(argv[2]));

    std::cout << "=== TCP Client Test Program ===" << std::endl;
    std::cout << "Startup Success" << std::endl;
    std::cout << "Connecting to " << host << ":" << port << std::endl;

    std::vector<TestResult> results;

    auto run_one = [&](const char* tag, std::function<TestResult()> fn) {
        std::cout << "\n========== " << tag << " ==========" << std::endl;
        TestResult res = fn();
        std::cout << (res.passed ? "[PASS] " : "[FAIL] ") << res.name
                  << " - " << res.detail << std::endl;
        results.push_back(res);
    };

    run_one("Test 0: Happy Path",        [&] { return Test_HappyPath(host, port); });
    run_one("Test 1: Continuous RW x20", [&] { return Test_ContinuousReadWrite(host, port); });
    run_one("Test 2: Half-Packet",       [&] { return Test_PartialFrame(host, port); });
    run_one("Test 3: Request Timeout",   [&] { return Test_RequestTimeout(host, port); });
    run_one("Test 4: Disconnect-Fails",  [&] { return Test_DisconnectFailsPending(host, port); });
    run_one("Test 5: Auto-Reconnect",    [&] { return Test_AutoReconnect(host, port); });

    int passed = 0;
    for (auto& r : results) if (r.passed) ++passed;

    std::cout << "\n========== Summary ==========" << std::endl;
    for (auto& r : results) {
        std::cout << (r.passed ? "  PASS  " : "  FAIL  ") << r.name
                  << "  (" << r.detail << ")" << std::endl;
    }
    std::cout << passed << "/" << results.size() << " tests passed" << std::endl;

    if (passed == static_cast<int>(results.size())) {
        std::cout << "\n✓✓✓ All tests passed! Data matches!" << std::endl;
        return 0;
    } else {
        std::cout << "\n✗ Some tests failed!" << std::endl;
        return 1;
    }
}
