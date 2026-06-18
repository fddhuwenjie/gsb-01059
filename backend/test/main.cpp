#include "tcp_client.h"
#include <iostream>
#include <thread>
#include <chrono>

int main(int argc, char* argv[]) {
    std::string host = "test-server";
    uint16_t port = 8888;
    
    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = static_cast<uint16_t>(std::stoi(argv[2]));
    }
    
    std::cout << "=== TCP Client Test Program ===" << std::endl;
    std::cout << "Startup Success" << std::endl;
    std::cout << "Connecting to " << host << ":" << port << std::endl;
    
    tcp_client::TcpClient client;
    
    bool connected = false;
    bool test_completed = false;
    bool test_success = false;
    
    // 测试数据（在lambda外部定义）
    std::vector<uint8_t> write_data = {'H', 'e', 'l', 'l', 'o', ',', ' ', 'W', 'o', 'r', 'l', 'd', '!'};
    
    // 设置错误回调
    client.SetErrorCallback([&](const std::string& error) {
        std::cerr << "Error: " << error << std::endl;
    });
    
    // 连接
    client.Connect(host, port, [&](bool success) {
        if (success) {
            std::cout << "✓ Connected successfully!" << std::endl;
            connected = true;
            
            // 等待一下确保连接稳定
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            // 测试1: 写入数据
            std::cout << "\n[Test 1] Writing data..." << std::endl;
            client.WriteData(write_data, [&](bool success, uint16_t dataSize) {
                if (success) {
                    std::cout << "✓ Write successful! Data size: " << dataSize << " bytes" << std::endl;
                    
                    // 测试2: 读取数据
                    std::cout << "\n[Test 2] Reading data..." << std::endl;
                    client.ReadData([&](bool success, const std::vector<uint8_t>& data) {
                        if (success) {
                            std::cout << "✓ Read successful! Data size: " << data.size() << " bytes" << std::endl;
                            std::cout << "Data content: ";
                            for (uint8_t byte : data) {
                                std::cout << static_cast<char>(byte);
                            }
                            std::cout << std::endl;
                            
                            // 验证数据是否正确
                            if (data == write_data) {
                                std::cout << "\n✓✓✓ All tests passed! Data matches!" << std::endl;
                                test_success = true;
                            } else {
                                std::cout << "\n✗ Data mismatch!" << std::endl;
                                std::cout << "Expected size: " << write_data.size() << ", Actual size: " << data.size() << std::endl;
                            }
                        } else {
                            std::cerr << "✗ Read failed!" << std::endl;
                        }
                        test_completed = true;
                        client.Stop();
                    });
                } else {
                    std::cerr << "✗ Write failed!" << std::endl;
                    test_completed = true;
                    client.Stop();
                }
            });
        } else {
            std::cerr << "✗ Connection failed!" << std::endl;
            test_completed = true;
            client.Stop();
        }
    });
    
    // 运行事件循环
    std::thread loop_thread([&]() {
        client.Run();
    });
    
    // 等待测试完成（最多10秒）
    auto start = std::chrono::steady_clock::now();
    while (!test_completed) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > 10) {
            std::cerr << "Test timeout!" << std::endl;
            client.Stop();
            break;
        }
    }
    
    if (loop_thread.joinable()) {
        loop_thread.join();
    }
    
    client.Disconnect();
    
    return test_success ? 0 : 1;
}
