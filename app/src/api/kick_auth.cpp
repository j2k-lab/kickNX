#include "api/kick_auth.hpp"

#include "api/http.hpp"
#include "utils/thread.hpp"

#include <borealis.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace kick {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

struct RawResponse {
    long status = 0;
    std::string body;
};

std::string oauthPath() {
#if defined(__SWITCH__)
    return "sdmc:/config/KickNX/oauth.json";
#else
    return "oauth.json";
#endif
}

std::string jsonIdToString(const json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key)) return {};
    const auto& val = obj[key];
    if (val.is_string()) return val.get<std::string>();
    if (val.is_number_integer()) return std::to_string(val.get<long long>());
    if (val.is_number_unsigned()) return std::to_string(val.get<unsigned long long>());
    return {};
}

size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    const size_t count = size * nmemb;
    body->append(ptr, count);
    return count;
}

class AuthHttpClient {
public:
    AuthHttpClient() {
        curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        }
    }

    ~AuthHttpClient() {
        if (curl) curl_easy_cleanup(curl);
    }

    bool valid() const { return curl != nullptr; }

    std::string getCookie(const std::string& name) const {
        if (!curl) return {};
        struct curl_slist* cookies = nullptr;
        if (curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &cookies) != CURLE_OK || !cookies)
            return {};
        std::string result;
        for (struct curl_slist* item = cookies; item; item = item->next) {
            if (!item->data) continue;
            std::string line(item->data);
            std::istringstream iss(line);
            std::string col;
            std::vector<std::string> cols;
            while (std::getline(iss, col, '\t')) {
                cols.push_back(col);
            }
            if (cols.size() >= 7 && cols[5] == name) {
                result = cols[6];
                break;
            }
        }
        curl_slist_free_all(cookies);
        return result;
    }

    std::string unescape(const std::string& str) const {
        if (!curl || str.empty()) return str;
        int outLen = 0;
        char* unescaped = curl_easy_unescape(curl, str.c_str(), static_cast<int>(str.size()), &outLen);
        if (!unescaped) return str;
        std::string res(unescaped, outLen);
        curl_free(unescaped);
        return res;
    }

    RawResponse request(
        const std::string& method,
        const std::string& url,
        const std::string& body = "",
        const std::vector<std::string>& headers = {},
        const std::string& userAgent = "") {
        RawResponse resp;
        if (!curl) return resp;

        resp.body.clear();
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        } else {
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        }

        const std::string ua = userAgent.empty()
            ? "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
              "(KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36"
            : userAgent;
        curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());

        curl_slist* headerList = nullptr;
        for (const auto& h : headers) {
            headerList = curl_slist_append(headerList, h.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);

        const CURLcode rc = curl_easy_perform(curl);
        if (rc == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
        } else {
            resp.status = -1;
            resp.body = curl_easy_strerror(rc);
        }

        if (headerList) curl_slist_free_all(headerList);
        return resp;
    }

    std::string exportCookies() const {
        if (!curl) return {};
        struct curl_slist* cookies = nullptr;
        if (curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &cookies) != CURLE_OK || !cookies)
            return {};
        std::string result;
        for (struct curl_slist* item = cookies; item; item = item->next) {
            if (item->data) {
                result += item->data;
                result += "\n";
            }
        }
        curl_slist_free_all(cookies);
        return result;
    }

    void importCookies(const std::string& cookieData) {
        if (!curl || cookieData.empty()) return;
        std::istringstream iss(cookieData);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            curl_easy_setopt(curl, CURLOPT_COOKIELIST, line.c_str());
        }
    }

private:
    CURL* curl = nullptr;
};

RawResponse postFormRaw(const std::string& url, const HTTP::Form& form) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Could not initialize OAuth network request");

    RawResponse response;
    const std::string data = HTTP::encode_form(form);
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(data.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "kickNX/0.4.0 (Nintendo Switch)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    const CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        const std::string error = curl_easy_strerror(result);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        throw std::runtime_error("OAuth request failed: " + error);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

RawResponse postJsonRaw(
    const std::string& url,
    const std::string& jsonBody,
    const HTTP::Header& extraHeaders = {}) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Could not initialize Kick auth network request");

    RawResponse response;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "x-app-platform: web");
    headers = curl_slist_append(headers, "Origin: https://kick.com");
    headers = curl_slist_append(headers, "Referer: https://kick.com/");
    for (const auto& header : extraHeaders) {
        headers = curl_slist_append(headers, header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonBody.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    const CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        const std::string error = curl_easy_strerror(result);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        throw std::runtime_error("Kick auth request failed: " + error);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

std::string normalizeBearerToken(std::string token) {
    auto isSpaceOrQuote = [](unsigned char c) {
        return std::isspace(c) || c == '"' || c == '\'' || c == '\r' || c == '\n';
    };
    while (!token.empty() && isSpaceOrQuote(static_cast<unsigned char>(token.front())))
        token.erase(token.begin());
    while (!token.empty() && isSpaceOrQuote(static_cast<unsigned char>(token.back())))
        token.pop_back();

    const std::vector<std::string> prefixes = {
        "Authorization: Bearer ",
        "Authorization: bearer ",
        "Authorization: ",
        "Bearer ",
        "bearer ",
        "session_token=",
        "session_token: ",
        "token=",
        "token: ",
    };
    for (const auto& prefix : prefixes) {
        if (token.size() > prefix.size() &&
            token.compare(0, prefix.size(), prefix) == 0) {
            token.erase(0, prefix.size());
            while (!token.empty() && isSpaceOrQuote(static_cast<unsigned char>(token.front())))
                token.erase(token.begin());
            break;
        }
    }

    // Browser session_token cookies URL-encode the pipe separator as %7C.
    for (const std::string& encoded : {"%7C", "%7c"}) {
        size_t pos = 0;
        while ((pos = token.find(encoded, pos)) != std::string::npos) {
            token.replace(pos, encoded.size(), "|");
            pos += 1;
        }
    }
    return token;
}

void populateUserFromJson(const json& user, OAuthSession& session) {
    if (!user.is_object()) return;
    const std::string id = jsonIdToString(user, "id");
    if (!id.empty()) session.userId = id;
    std::string username = user.value("username", "");
    if (username.empty()) username = user.value("slug", "");
    if (!username.empty()) {
        session.login = username;
        session.displayName = user.value("username", username);
    }
    if (user.contains("profile_pic") && user["profile_pic"].is_string())
        session.profileImageUrl = user["profile_pic"].get<std::string>();
    else if (user.contains("profilepic") && user["profilepic"].is_string())
        session.profileImageUrl = user["profilepic"].get<std::string>();
}

std::vector<std::string> parseScopes(const json& value) {
    std::vector<std::string> scopes;
    if (value.is_array()) {
        for (const auto& item : value)
            if (item.is_string()) scopes.push_back(item.get<std::string>());
    } else if (value.is_string()) {
        scopes.push_back(value.get<std::string>());
    }
    return scopes;
}

OAuthSession parseTokenResponse(const std::string& body) {
    const json root = json::parse(body);
    OAuthSession session;
    session.accessToken = root.value("access_token", "");
    session.refreshToken = root.value("refresh_token", "");
    session.scopes = parseScopes(root.value("scope", json::array()));
    const int expiresIn = root.value("expires_in", 0);
    session.expiresAt = expiresIn > 0
        ? static_cast<long long>(std::time(nullptr)) + expiresIn
        : 0LL;
    if (session.accessToken.empty())
        throw std::runtime_error("Kick returned no access token");
    return session;
}

OAuthSession fetchIdentity(const Config&, OAuthSession session) {
    if (session.accessToken.empty()) return session;

    HTTP::Header headers = {
        "Authorization: Bearer " + session.accessToken,
        "Cookie: session_token=" + session.accessToken,
        "Accept: application/json",
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.0.0 Safari/537.36",
        "x-app-platform: web",
        "Origin: https://kick.com",
        "Referer: https://kick.com/",
    };

    std::string response;
    try {
        response = HTTP::get(
            "https://kick.com/api/v1/user", headers, HTTP::Timeout{15000});
    } catch (const std::exception& error) {
        brls::Logger::warning("Kick /api/v1/user failed: {}", error.what());
        try {
            response = HTTP::get(
                "https://kick.com/api/internal/v1/user", headers, HTTP::Timeout{15000});
        } catch (...) {
            throw;
        }
    }

    json root;
    try {
        root = json::parse(response);
    } catch (...) {
        throw std::runtime_error("Kick returned an invalid response for the user profile");
    }

    if (root.is_object() && root.contains("message") && root["message"].is_string()) {
        const std::string msg = root["message"].get<std::string>();
        if (msg == "Unauthenticated." || msg.find("Unauthenticated") != std::string::npos) {
            throw std::runtime_error(
                "Session is invalid or expired. Please sign in again using the TV QR code.");
        }
    }

    json user = json::object();
    if (root.is_object()) {
        if (root.contains("data") && root["data"].is_object()) {
            user = root["data"];
        } else if (root.contains("user") && root["user"].is_object()) {
            user = root["user"];
        } else {
            user = root;
        }
    }

    session.userId = jsonIdToString(user, "id");
    if (session.userId.empty()) session.userId = jsonIdToString(user, "user_id");
    session.login = user.value("username", "");
    if (session.login.empty()) session.login = user.value("slug", "");
    session.displayName = user.value("username", session.login);
    if (user.contains("profile_pic") && user["profile_pic"].is_string())
        session.profileImageUrl = user["profile_pic"].get<std::string>();
    else if (user.contains("profilepic") && user["profilepic"].is_string())
        session.profileImageUrl = user["profilepic"].get<std::string>();

    if (session.userId.empty() && !session.login.empty())
        session.userId = session.login;
    if (session.userId.empty())
        throw std::runtime_error(
            "Session did not return account info. Please sign in again using the TV QR code.");
    return session;
}

OAuthSession refreshOAuth(const Config& config, const OAuthSession& current) {
    if (current.refreshToken.empty()) return current;

    const RawResponse response = postFormRaw(
        "https://id.kick.com/oauth/token",
        {{"client_id", config.oauthClientId},
         {"grant_type", "refresh_token"},
         {"refresh_token", current.refreshToken}});
    if (response.status >= 400) {
        std::string message;
        try {
            message = json::parse(response.body).value("message", "");
        } catch (...) {
        }
        if (message.empty()) message = "HTTP " + std::to_string(response.status);
        throw std::runtime_error("Kick session refresh failed: " + message);
    }

    OAuthSession refreshed = parseTokenResponse(response.body);
    refreshed.userId = current.userId;
    refreshed.login = current.login;
    refreshed.displayName = current.displayName;
    refreshed.profileImageUrl = current.profileImageUrl;
    refreshed = fetchIdentity(config, std::move(refreshed));
    saveOAuth(refreshed);
    return refreshed;
}

}  // namespace

OAuthSession loadOAuth() {
    std::ifstream input(oauthPath());
    OAuthSession session;
    if (input.is_open()) {
        try {
            const json root = json::parse(input);
            session.accessToken = root.value("access_token", "");
            session.refreshToken = root.value("refresh_token", "");
            session.scopes = parseScopes(root.value("scopes", json::array()));
            session.userId = jsonIdToString(root, "user_id");
            session.login = root.value("login", "");
            session.displayName = root.value("display_name", "");
            session.profileImageUrl = root.value("profile_image_url", "");
            session.expiresAt = root.value("expires_at", 0LL);
        } catch (...) {
        }
    }

    return session;
}

void saveOAuth(const OAuthSession& session) {
    const fs::path path(oauthPath());
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    const json root = {
        {"access_token", session.accessToken},
        {"refresh_token", session.refreshToken},
        {"scopes", session.scopes},
        {"user_id", session.userId},
        {"login", session.login},
        {"display_name", session.displayName},
        {"profile_image_url", session.profileImageUrl},
        {"expires_at", session.expiresAt},
    };
    std::ofstream output(path, std::ios::trunc);
    output << root.dump(2);
}

void clearOAuth() {
    std::error_code error;
    fs::remove(oauthPath(), error);
}

OAuthSession ensureOAuth(const Config& config) {
    OAuthSession session = loadOAuth();
    if (!session.accessToken.empty() && session.userId.empty()) {
        try {
            session = fetchIdentity(config, std::move(session));
            saveOAuth(session);
        } catch (const std::exception& error) {
            brls::Logger::warning("Kick identity check failed: {}", error.what());
        }
    }

    if (!session.signedIn()) {
        // Kick public v1/v2 endpoints and Pusher chat work without OAuth;
        // return an empty session for guest mode.
        return {};
    }

    const auto now = static_cast<long long>(std::time(nullptr));
    if (session.expiresAt > 0 && session.expiresAt <= now + 60 && !session.refreshToken.empty()) {
        try {
            session = refreshOAuth(config, session);
        } catch (const std::exception& error) {
            brls::Logger::warning("Kick token refresh failed: {}", error.what());
        }
    }
    return session;
}

static std::mt19937& getTvLinkRng() {
    static std::mt19937 gen([]() {
        uint64_t seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
#if defined(__SWITCH__)
        seed ^= armGetSystemTick();
#endif
        std::seed_seq seq{
            static_cast<uint32_t>(seed & 0xFFFFFFFF),
            static_cast<uint32_t>(seed >> 32),
            static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()),
            0x9E3779B9u
        };
        return std::mt19937(seq);
    }());
    return gen;
}

TVLinkSession createTVLinkSession() {
    auto& gen = getTvLinkRng();
    std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);
    std::uniform_int_distribution<int> digitDis(0, 9);

    uint32_t d0 = dis(gen);
    uint32_t d1 = dis(gen);
    uint32_t d2 = dis(gen);
    uint32_t d3 = dis(gen);

    char uuidBuf[37];
    snprintf(uuidBuf, sizeof(uuidBuf), "%08x-%04x-%04x-%04x-%04x%08x",
             d0,
             (d1 >> 16) & 0xFFFF,
             ((d1 & 0x0FFF) | 0x4000), // Version 4
             ((d2 >> 16) & 0x3FFF) | 0x8000, // Variant 1
             d2 & 0xFFFF,
             d3);

    std::string code;
    for (int i = 0; i < 6; ++i) {
        code += std::to_string(digitDis(gen));
    }

    TVLinkSession session;
    session.uuid = uuidBuf;
    session.code = code;
    session.verificationUrl = "https://kick.com/tv/login?uuid=" + session.uuid + "&code=" + session.code;
    session.intervalSeconds = 3;
    return session;
}

void pollTVLinkAuthAsync(
    const Config& config,
    const TVLinkSession& session,
    AuthCallback success,
    std::function<void(bool pending, const std::string& message)> statusCallback,
    HTTP::Cancel cancel) {
    ThreadPool::instance().submit(
        [config, session, success = std::move(success),
            statusCallback = std::move(statusCallback),
            cancel = std::move(cancel)](HTTP&) mutable {
            try {
                if (cancel && cancel->load()) return;

                AuthHttpClient client;
                if (!client.valid()) {
                    if (statusCallback) {
                        brls::sync([statusCallback]() {
                            statusCallback(false, "Could not initialize network client");
                        });
                    }
                    return;
                }

                const std::string url = "https://kick.com/api/tv/link/authenticate/" + session.uuid;
                json reqBody = {{"key", session.code}};
                const std::string payload = reqBody.dump();

                std::vector<std::string> headers = {
                    "Content-Type: application/json",
                    "Accept: application/json",
                    "Origin: https://kick.com",
                    "Referer: https://kick.com/tv/login"
                };

                RawResponse resp = client.request(
                    "POST",
                    url,
                    payload,
                    headers,
                    "Mozilla/5.0 (Nintendo Switch; kickNX/0.4.0)");

                if (cancel && cancel->load()) return;

                brls::Logger::info("kickNX: TV Link authenticate status: {} body: {}", resp.status, resp.body);

                if (resp.status == 200) {
                    json root = json::parse(resp.body);
                    std::string token;
                    if (root.is_object()) {
                        if (root.contains("token") && root["token"].is_string()) {
                            token = root["token"].get<std::string>();
                        } else if (root.contains("data") && root["data"].is_object() &&
                                   root["data"].contains("token") && root["data"]["token"].is_string()) {
                            token = root["data"]["token"].get<std::string>();
                        }
                    }

                    if (token.empty()) {
                        throw std::runtime_error("Kick TV login response did not contain a session token");
                    }

                    OAuthSession userSession;
                    userSession.accessToken = token;
                    userSession = fetchIdentity(config, std::move(userSession));
                    saveOAuth(userSession);

                    if (success) {
                        brls::sync([success, userSession]() {
                            success(userSession);
                        });
                    }
                    return;
                }

                // If 403 or 404 or 422: check if it's the expected "waiting for authorization" state
                if (resp.status == 403 || resp.status == 422 || resp.status == 404) {
                    if (statusCallback) {
                        brls::sync([statusCallback]() {
                            statusCallback(true, "Waiting for approval at kick.com/tv/login…");
                        });
                    }
                    return;
                }

                std::string errorMsg = "HTTP " + std::to_string(resp.status);
                try {
                    json errObj = json::parse(resp.body);
                    if (errObj.contains("message") && errObj["message"].is_string()) {
                        errorMsg = errObj["message"].get<std::string>();
                    }
                } catch (...) {}

                if (statusCallback) {
                    brls::sync([statusCallback, errorMsg]() {
                        statusCallback(false, errorMsg);
                    });
                }
            } catch (const std::exception& ex) {
                if (statusCallback) {
                    std::string msg = ex.what();
                    brls::sync([statusCallback, msg]() {
                        statusCallback(false, msg);
                    });
                }
            } catch (...) {
                if (statusCallback) {
                    brls::sync([statusCallback]() {
                        statusCallback(false, "Unknown TV link authentication error");
                    });
                }
            }
        });
}

void loginWithTokenAsync(
    const Config& config,
    const std::string& bearerToken,
    AuthCallback success,
    ErrorCallback failure) {
    ThreadPool::instance().submit(
        [config, bearerToken, success = std::move(success),
            failure = std::move(failure)](HTTP&) mutable {
            try {
                OAuthSession session = loadOAuth();
                std::string tokenToUse = normalizeBearerToken(bearerToken);
                if (!tokenToUse.empty()) {
                    session.accessToken = tokenToUse;
                    session.userId.clear();
                }

                if (session.accessToken.empty()) {
                    throw std::runtime_error(
                        "A valid Kick session token is required to sign in.");
                }

                session = fetchIdentity(config, std::move(session));
                saveOAuth(session);
                brls::sync(std::bind(success, session));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(failure, std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(failure, std::string("Unknown Kick token login error")));
            }
        });
}

void logoutAsync(
    const Config&,
    std::function<void()> complete) {
    clearOAuth();
    if (complete) brls::sync(complete);
}

}  // namespace kick
