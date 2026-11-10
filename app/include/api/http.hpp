/*
    Copyright 2023 dragonflylee
*/

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <fstream>
#include <sstream>
#include <curl/system.h>

#include <borealis/core/event.hpp>

class HTTP {
public:
    using Header = std::vector<std::string>;
    using Form = std::unordered_map<std::string, std::string>;
    // For cancellable requests
    using Cancel = std::shared_ptr<std::atomic_bool>;

    inline static bool PROXY_STATUS = false;
    inline static std::string PROXY;

    struct Timeout {
        long timeout = TIMEOUT;
    };

    HTTP();
    HTTP(const HTTP& other) = delete;
    ~HTTP();

    /// While one exists, background image downloads wait, so a request that
    /// must not fail (sending a chat message) is not competing for a TLS
    /// connection.
    class PriorityRequest {
    public:
        PriorityRequest();
        ~PriorityRequest();
        PriorityRequest(const PriorityRequest&) = delete;
        PriorityRequest& operator=(const PriorityRequest&) = delete;
    };

    /// Closes any connection this handle kept open.
    void dropConnection();

    /// Points a curl handle at the app-wide DNS cache, so a new connection
    /// does not repeat a lookup another one just made.
    static void shareDnsCache(void* easy);

    static std::string encode_form(const Form& form);
    void _get(const std::string& url, std::ostream* out);
    std::string _post(const std::string& url, const std::string& data);
    /// Marks a ThreadPool worker's handle, whose image downloads share fewer
    /// connection slots so Kick API calls always have one free.
    void set_pooled(bool value) { pooled = value; }
    void _delete(const std::string& url, std::ostream* out);

    template <typename... Ts>
    static void set_option(HTTP& s, Ts&&... ts) {
        s.set_option_internal<Ts...>(std::forward<Ts>(ts)...);
    }

    // Get methods
    template <typename... Ts>
    static std::string get(const std::string& url, Ts&&... ts) {
        HTTP s;
        std::ostringstream body;
        set_option(s, std::forward<Ts>(ts)...);
        s._get(url, &body);
        return body.str();
    }

    // Post methods
    template <typename... Ts>
    static std::string post(const std::string& url, const std::string& data, Ts&&... ts) {
        HTTP s;
        set_option(s, std::forward<Ts>(ts)...);
        return s._post(url, data);
    }

    template <typename... Ts>
    static std::string post(const std::string& url, const Form& form, Ts&&... ts) {
        HTTP s;
        set_option(s, std::forward<Ts>(ts)...);
        return s._post(url, s.encode_form(form));
    }

    // Delete methods
    template <typename... Ts>
    static std::string del(const std::string& url, Ts&&... ts) {
        HTTP s;
        std::ostringstream body;
        set_option(s, std::forward<Ts>(ts)...);
        s._delete(url, &body);
        return body.str();
    }

    inline static long TIMEOUT = 3000L;

private:
    void configureHandle();

    static size_t easy_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata);
    static int easy_progress_cb(
        void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);
    int perform(std::ostream* body);

    void set_option(const Header& hs);
    void set_option(const Timeout& t);
    void set_option(const Cancel& c);

    template <typename CT>
    void set_option_internal(CT&& option) {
        set_option(std::forward<CT>(option));
    }

    template <typename CT, typename... Ts>
    void set_option_internal(CT&& option, Ts&&... ts) {
        set_option_internal<CT>(std::forward<CT>(option));
        set_option_internal<Ts...>(std::forward<Ts>(ts)...);
    }

    void* easy;
    struct curl_slist* chunk;
    Cancel is_cancel;
    bool pooled = false;
};
