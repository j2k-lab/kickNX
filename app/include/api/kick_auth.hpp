#pragma once

#include "api/kick.hpp"
#include "api/http.hpp"

#include <functional>
#include <string>
#include <vector>

namespace kick {

struct TVLinkSession {
    std::string uuid;
    std::string code;
    std::string verificationUrl;
    int intervalSeconds = 3;
};

struct OAuthSession {
    std::string accessToken;
    std::string refreshToken;
    std::vector<std::string> scopes;
    std::string userId;
    std::string login;
    std::string displayName;
    std::string profileImageUrl;
    long long expiresAt = 0;

    bool signedIn() const { return !accessToken.empty() && !userId.empty(); }
};

using AuthCallback = std::function<void(OAuthSession)>;

OAuthSession loadOAuth();
void saveOAuth(const OAuthSession& session);
void clearOAuth();
OAuthSession ensureOAuth(const Config& config);

TVLinkSession createTVLinkSession();

void pollTVLinkAuthAsync(
    const Config& config,
    const TVLinkSession& session,
    AuthCallback success,
    std::function<void(bool pending, const std::string& message)> statusCallback,
    HTTP::Cancel cancel = nullptr);

void loginWithTokenAsync(
    const Config& config,
    const std::string& bearerToken,
    AuthCallback success,
    ErrorCallback failure);

void logoutAsync(
    const Config& config,
    std::function<void()> complete = {});

}  // namespace kick

