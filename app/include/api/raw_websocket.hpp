#pragma once

// A minimal TLS WebSocket client over libcurl's CONNECT_ONLY mode, shared by
// Kick chat and the viewer (watch time) connection.

#include <curl/curl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace kick {
namespace websocket_detail {

inline std::string base64Encode(const uint8_t* input, size_t size) {
    static constexpr char TABLE[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((size + 2) / 3) * 4);

    for (size_t index = 0; index < size; index += 3) {
        const uint32_t first = input[index];
        const uint32_t second = index + 1 < size ? input[index + 1] : 0;
        const uint32_t third = index + 2 < size ? input[index + 2] : 0;
        const uint32_t value = (first << 16) | (second << 8) | third;

        result.push_back(TABLE[(value >> 18) & 0x3F]);
        result.push_back(TABLE[(value >> 12) & 0x3F]);
        result.push_back(index + 1 < size ? TABLE[(value >> 6) & 0x3F] : '=');
        result.push_back(index + 2 < size ? TABLE[value & 0x3F] : '=');
    }
    return result;
}

struct ParsedUrl {
    std::string curlUrl;
    std::string host;
    std::string path;
};

inline ParsedUrl parseWebSocketUrl(std::string url) {
    ParsedUrl result;

    if (url.rfind("wss://", 0) == 0) {
        result.curlUrl = "https://" + url.substr(6);
        url.erase(0, 6);
    } else if (url.rfind("https://", 0) == 0) {
        result.curlUrl = url;
        url.erase(0, 8);
    } else {
        throw std::runtime_error("Unsupported Kick WebSocket URL");
    }

    const auto slash = url.find('/');
    result.host = slash == std::string::npos ? url : url.substr(0, slash);
    result.path = slash == std::string::npos ? "/" : url.substr(slash);
    if (result.host.empty()) throw std::runtime_error("Invalid Kick WebSocket host");
    return result;
}

inline void sleepBriefly() {
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
}

class RawWebSocket {
public:
    RawWebSocket(const std::string& url, std::atomic_bool& stopRequested) {
        const ParsedUrl parsed = parseWebSocketUrl(url);
        host = parsed.host;
        path = parsed.path;

        easy = curl_easy_init();
        if (!easy) throw std::runtime_error("Could not initialize chat transport");

        // A constructor that throws never reaches the destructor, so free the
        // handle (and its TLS connection) here when connecting fails.
        try {
            connectAndHandshake(parsed, stopRequested);
        } catch (...) {
            curl_easy_cleanup(easy);
            easy = nullptr;
            throw;
        }
    }

    ~RawWebSocket() {
        if (easy) curl_easy_cleanup(easy);
    }

    RawWebSocket(const RawWebSocket&) = delete;
    RawWebSocket& operator=(const RawWebSocket&) = delete;

    void sendText(const std::string& payload) {
        sendFrame(0x1, payload);
    }

    bool nextText(
        std::string& text,
        std::atomic_bool& stopRequested,
        const std::function<void()>& onIdleTick = nullptr,
        std::chrono::seconds tickEvery = std::chrono::seconds(50)) {
        auto lastPingCheck = std::chrono::steady_clock::now();
        while (!stopRequested.load()) {
            if (parseFrames(text)) return true;

            const auto now = std::chrono::steady_clock::now();
            if (onIdleTick && now - lastPingCheck >= tickEvery) {
                lastPingCheck = now;
                onIdleTick();
            }

            std::array<uint8_t, 8192> data{};
            size_t received = 0;
            const CURLcode code =
                curl_easy_recv(easy, data.data(), data.size(), &received);

            if (code == CURLE_AGAIN) {
                sleepBriefly();
                continue;
            }
            if (code != CURLE_OK)
                throw std::runtime_error(
                    std::string("Chat receive failed: ") +
                    curl_easy_strerror(code));
            if (received == 0)
                throw std::runtime_error("Kick closed the chat connection");

            receiveBuffer.insert(
                receiveBuffer.end(),
                data.begin(),
                data.begin() + static_cast<std::ptrdiff_t>(received));
        }
        return false;
    }

    void close() {
        try {
            sendFrame(0x8, "");
        } catch (...) {
        }
    }

private:
    static int abortWhenStopped(
        void* stopRequested, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        return static_cast<std::atomic_bool*>(stopRequested)->load() ? 1 : 0;
    }

    void connectAndHandshake(const ParsedUrl& parsed, std::atomic_bool& stopRequested) {
        curl_easy_setopt(easy, CURLOPT_URL, parsed.curlUrl.c_str());
        curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 1L);
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
        // The Switch has no IPv6; the HTTP class sets the same.
        curl_easy_setopt(easy, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
        // Leaving the stream while chat connects must not wait out the
        // connect timeout: abort the TLS connect once stop is requested.
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, abortWhenStopped);
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &stopRequested);

        const CURLcode connected = curl_easy_perform(easy);
        if (connected != CURLE_OK)
            throw std::runtime_error(
                std::string("Chat connection failed: ") +
                curl_easy_strerror(connected));

        std::array<uint8_t, 16> nonce{};
        std::random_device random;
        for (auto& byte : nonce)
            byte = static_cast<uint8_t>(random() & 0xFF);

        const std::string key = base64Encode(nonce.data(), nonce.size());
        const std::string request =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + "\r\n"
            "Origin: https://kick.com\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.0.0 Safari/537.36\r\n\r\n";

        sendRaw(
            reinterpret_cast<const uint8_t*>(request.data()),
            request.size());

        std::string response;
        std::array<uint8_t, 2048> buffer{};
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);

        while (response.find("\r\n\r\n") == std::string::npos) {
            if (stopRequested.load())
                throw std::runtime_error("Chat stopped");
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("Timed out during chat WebSocket handshake");

            size_t received = 0;
            const CURLcode code =
                curl_easy_recv(easy, buffer.data(), buffer.size(), &received);
            if (code == CURLE_AGAIN) {
                sleepBriefly();
                continue;
            }
            if (code != CURLE_OK)
                throw std::runtime_error(
                    std::string("Chat handshake failed: ") +
                    curl_easy_strerror(code));

            response.append(
                reinterpret_cast<const char*>(buffer.data()),
                received);
        }

        if (response.find(" 101 ") == std::string::npos)
            throw std::runtime_error(
                "Kick rejected the chat WebSocket handshake");

        const auto end = response.find("\r\n\r\n");
        if (end != std::string::npos && end + 4 < response.size()) {
            const auto* first =
                reinterpret_cast<const uint8_t*>(response.data() + end + 4);
            receiveBuffer.insert(
                receiveBuffer.end(),
                first,
                first + response.size() - end - 4);
        }
    }

    void sendRaw(const uint8_t* data, size_t size) {
        size_t offset = 0;
        while (offset < size) {
            size_t sent = 0;
            const CURLcode code =
                curl_easy_send(easy, data + offset, size - offset, &sent);
            if (code == CURLE_AGAIN) {
                sleepBriefly();
                continue;
            }
            if (code != CURLE_OK)
                throw std::runtime_error(
                    std::string("Chat send failed: ") +
                    curl_easy_strerror(code));
            offset += sent;
        }
    }

    void sendFrame(uint8_t opcode, const std::string& payload) {
        std::vector<uint8_t> frame;
        frame.push_back(static_cast<uint8_t>(0x80 | opcode));

        const size_t length = payload.size();
        if (length <= 125) {
            frame.push_back(static_cast<uint8_t>(0x80 | length));
        } else if (length <= 0xFFFF) {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(length & 0xFF));
        } else {
            frame.push_back(0x80 | 127);
            for (int shift = 56; shift >= 0; shift -= 8)
                frame.push_back(
                    static_cast<uint8_t>((length >> shift) & 0xFF));
        }

        std::random_device random;
        std::array<uint8_t, 4> mask = {
            static_cast<uint8_t>(random() & 0xFF),
            static_cast<uint8_t>(random() & 0xFF),
            static_cast<uint8_t>(random() & 0xFF),
            static_cast<uint8_t>(random() & 0xFF),
        };
        frame.insert(frame.end(), mask.begin(), mask.end());

        for (size_t index = 0; index < payload.size(); ++index)
            frame.push_back(
                static_cast<uint8_t>(payload[index]) ^ mask[index % 4]);

        sendRaw(frame.data(), frame.size());
    }

    bool parseFrames(std::string& text) {
        while (receiveBuffer.size() >= 2) {
            const uint8_t first = receiveBuffer[0];
            const uint8_t second = receiveBuffer[1];
            const bool final = (first & 0x80) != 0;
            const uint8_t opcode = first & 0x0F;
            const bool masked = (second & 0x80) != 0;

            uint64_t length = second & 0x7F;
            size_t header = 2;

            if (length == 126) {
                if (receiveBuffer.size() < 4) return false;
                length =
                    (static_cast<uint64_t>(receiveBuffer[2]) << 8) |
                    receiveBuffer[3];
                header = 4;
            } else if (length == 127) {
                if (receiveBuffer.size() < 10) return false;
                length = 0;
                for (size_t index = 2; index < 10; ++index)
                    length = (length << 8) | receiveBuffer[index];
                header = 10;
            }

            const size_t maskBytes = masked ? 4 : 0;
            if (length > 8 * 1024 * 1024)
                throw std::runtime_error("Chat frame is too large");
            if (receiveBuffer.size() < header + maskBytes + length)
                return false;

            std::array<uint8_t, 4> mask{};
            if (masked) {
                for (size_t index = 0; index < 4; ++index)
                    mask[index] = receiveBuffer[header + index];
            }

            const size_t payloadOffset = header + maskBytes;
            std::string payload;
            payload.resize(static_cast<size_t>(length));
            for (size_t index = 0; index < length; ++index) {
                uint8_t value = receiveBuffer[payloadOffset + index];
                if (masked) value ^= mask[index % 4];
                payload[index] = static_cast<char>(value);
            }

            receiveBuffer.erase(
                receiveBuffer.begin(),
                receiveBuffer.begin() +
                    static_cast<std::ptrdiff_t>(
                        payloadOffset + static_cast<size_t>(length)));

            if (opcode == 0x8) {
                throw std::runtime_error("Kick closed the chat session");
            }
            if (opcode == 0x9) {
                sendFrame(0xA, payload);
                continue;
            }
            if (opcode == 0xA) continue;

            if (opcode == 0x1) {
                fragment = payload;
                fragmentOpcode = opcode;
            } else if (opcode == 0x0 && fragmentOpcode == 0x1) {
                fragment += payload;
            } else {
                continue;
            }

            if (final) {
                text = std::move(fragment);
                fragment.clear();
                fragmentOpcode = 0;
                return true;
            }
        }
        return false;
    }

    CURL* easy = nullptr;
    std::string host;
    std::string path;
    std::vector<uint8_t> receiveBuffer;
    std::string fragment;
    uint8_t fragmentOpcode = 0;
};

}  // namespace websocket_detail

using websocket_detail::RawWebSocket;

}  // namespace kick
