#include "http-settings.hpp"
#include "log.hpp"

#ifdef ZSWAG_KEYCHAIN_SUPPORT
#include <keychain/keychain.h>
#endif
#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <regex>
#include <future>
#include <spdlog/spdlog.h>

using namespace httpcl;
using namespace std::string_literals;

static const std::chrono::minutes KEYCHAIN_TIMEOUT{1};
static const char* KEYCHAIN_PACKAGE = "lib.openapi.zserio.client";

namespace YAML
{

template <>
struct convert<Config::BasicAuthentication>
{
    static Node encode(const Config::BasicAuthentication& a)
    {
        Node node;
        node["user"] = a.user;
        if (!a.password.empty())
            node["password"] = a.password;
        else if (!a.keychain.empty())
            node["keychain"] = a.keychain;

        return node;
    }

    static bool decode(const Node& node, Config::BasicAuthentication& a)
    {
        if (!node.IsMap())
            return false;

        const auto& user = node["user"];
        const auto& password = node["password"];
        const auto& keychain = node["keychain"];

        if (!user)
            return false;

        a.user = user.as<std::string>();

        if (password)
            a.password = password.as<std::string>();
        else if (keychain)
            a.keychain = keychain.as<std::string>();
        else
            return false;

        return true;
    }
};

template <>
struct convert<Config::Proxy>
{
    static Node encode(const Config::Proxy& a)
    {
        Node node;
        node["host"] = a.host;
        node["port"] = a.port;

        if (!a.user.empty()) {
            node["user"] = a.user;
            if (!a.password.empty())
                node["password"] = a.password;
            else if (!a.keychain.empty())
                node["keychain"] = a.keychain;
        }

        return node;
    }

    static bool decode(const Node& node, Config::Proxy& a)
    {
        const auto& host = node["host"];
        const auto& port = node["port"];

        if (!host || !port)
            return false;

        a.host = host.as<std::string>();
        a.port = port.as<int>();

        const auto& user = node["user"];
        const auto& password = node["password"];
        const auto& keychain = node["keychain"];

        if (user) {
            a.user = user.as<std::string>();

            if (password)
                a.password = password.as<std::string>();
            else if (keychain)
                a.keychain = keychain.as<std::string>();
            else
                return false;
        }

        return true;
    }
};
}

std::string secret::load(
        const std::string &service,
        const std::string &user)
{
#ifdef ZSWAG_KEYCHAIN_SUPPORT
    log().debug("Loading secret (service={}, user={}) ...", service, user);
    auto result = std::async(std::launch::async, [=]() {
        keychain::Error error;
        auto password = keychain::getPassword(
                KEYCHAIN_PACKAGE,
                service,
                user,
                error);

        if (error)
            throw std::runtime_error(error.message);
        return password;
    });

    if (result.wait_for(KEYCHAIN_TIMEOUT) == std::future_status::timeout) {
        log().warn("  ... Keychain timed out.");
        return {};
    }

    log().debug("  ...OK.");
    return result.get();
#else
    throw std::runtime_error("[secret::load] zswag was compiled with ZSWAG_KEYCHAIN_SUPPORT OFF.");
#endif
}

std::string secret::store(
        const std::string &service,
        const std::string &user,
        const std::string &password)
{
#ifdef ZSWAG_KEYCHAIN_SUPPORT
    auto randServiceId = []() {
        std::string id(12, '.');
        std::generate(id.begin(), id.end(), []() {
            return "0123456789abcdef"[rand() % 16];
        });
        return id;
    };

    auto newService = service.empty()
                      ? "service password "s + randServiceId()
                      : service;

    log().debug("Storing secret (service={}, user={}) ...", newService, user);

    auto result = std::async(std::launch::async, [=]() {
        keychain::Error error;
        keychain::setPassword(KEYCHAIN_PACKAGE,
                              newService,
                              user,
                              password,
                              error);

        if (error)
            throw std::runtime_error(error.message);
    });

    if (result.wait_for(KEYCHAIN_TIMEOUT) == std::future_status::timeout) {
        log().warn("  ... Keychain timed out!");
        return {};
    }

    log().debug("  ...OK.");
    return newService;
#else
    throw std::runtime_error("[secret::store] zswag was compiled with ZSWAG_KEYCHAIN_SUPPORT OFF.");
#endif
}

bool secret::remove(
        const std::string &service,
        const std::string &user)
{
#ifdef ZSWAG_KEYCHAIN_SUPPORT
    log().debug("Deleting secret (service={}, user={}) ...", service, user);

    auto result = std::async(std::launch::async, [=]() {
        keychain::Error error;
        keychain::deletePassword(KEYCHAIN_PACKAGE,
                                 service,
                                 user,
                                 error);

        return error;
    });

    if (result.wait_for(KEYCHAIN_TIMEOUT) == std::future_status::timeout) {
        log().warn("  ... Keychain timeout!");
        return false;
    }

    log().debug("  ...OK.");
    return result.get();
#else
    throw std::runtime_error("[secret::remove] zswag was compiled with ZSWAG_KEYCHAIN_SUPPORT OFF.");
#endif
}

Settings::Settings()
{
    load();
}

void Settings::load()
{
    settings.clear();

    auto cookieJar = std::getenv("HTTP_SETTINGS_FILE");
    if (!cookieJar || strcmp(cookieJar, "") == 0) {
        log().debug("HTTP_SETTINGS_FILE environment variable is empty.");
        return;
    }

    if (!httplib::detail::is_file(cookieJar)) {
        log().debug("The HTTP_SETTINGS_FILE path '{}' is not a file.", cookieJar);
        return;
    }

    try {
        log().debug("Loading HTTP settings from '{}'...", cookieJar);
        auto node = YAML::LoadFile(cookieJar);
        uint32_t idx = 0;

        for (auto const& entry : node.as<std::vector<YAML::Node>>()) {
            Config conf;
            std::string urlPattern;

            if (auto entryParam = entry["url"])
                urlPattern = entryParam.as<std::string>();
            else
                throw std::runtime_error(
                    "Settings: Failed to read 'url' of entry #"s + std::to_string(idx) +
                    " in " + cookieJar);

            if (auto cookies = entry["cookies"])
                conf.cookies = cookies.as<std::map<std::string, std::string>>();

            if (auto headers = entry["headers"]) {
                auto headersMap = headers.as<std::map<std::string, std::string>>();
                conf.headers.insert(headersMap.begin(), headersMap.end());
            }

            if (auto query = entry["query"]) {
                auto queryMap = query.as<std::map<std::string, std::string>>();
                conf.query.insert(queryMap.begin(), queryMap.end());
            }

            if (auto basicAuth = entry["basic-auth"])
                conf.auth = basicAuth.as<Config::BasicAuthentication>();

            if (auto proxy = entry["proxy"])
                conf.proxy = proxy.as<Config::Proxy>();

            if (auto apiKey = entry["api-key"])
                conf.apiKey = apiKey.as<std::string>();

            settings[urlPattern] = std::move(conf);
            ++idx;
        }

        log().debug("  ...Done.");
    } catch (const YAML::BadFile& e) {
        log().error("Failed to parse HTTP settings at '{}': {}", cookieJar, e.what());
    } catch (const std::exception& e) {
        log().error("Failed to read http-settings from '{}': {}", cookieJar, e.what());
    }
}

void Settings::store()
{
    auto cookieJar = std::getenv("HTTP_SETTINGS_FILE");
    if (!cookieJar) {
        log().warn("HTTP_SETTINGS_FILE is not set, cannot save HTTP settings.");
        return;
    }

    try {
        auto node = YAML::Node();

        for (const auto& pair : settings) {
            auto settingsNode = YAML::Node();

            settingsNode["url"] = pair.first;
            const auto& entry = pair.second;

            if (!entry.cookies.empty())
                settingsNode["cookies"] = entry.cookies;

            if (!entry.headers.empty())
                settingsNode["headers"] = std::map<std::string, std::string>{
                    entry.headers.begin(), entry.headers.end()};

            if (!entry.query.empty())
                settingsNode["query"] = std::map<std::string, std::string>{
                    entry.query.begin(), entry.query.end()};

            if (entry.auth)
                settingsNode["basic-auth"] = *entry.auth;

            if (entry.proxy)
                settingsNode["proxy"] = *entry.proxy;

            if (entry.apiKey)
                settingsNode["api-key"] = *entry.apiKey;

            node.push_back(settingsNode);
        }

        log().debug("Saving HTTP settings to '{}'...", cookieJar);
        std::ofstream os(cookieJar);
        os << node;
        log().debug("  ...Done.", cookieJar);
    } catch (const std::exception& e) {
        log().error("Failed to write http-settings to '{}': {}", cookieJar, e.what());
    }
}

Config Settings::operator[] (const std::string &url) const
{
    Config result;

    for (auto const& [pattern, config] : settings)
    {
        if (!std::regex_match(url, std::regex(pattern)))
            continue;
        result |= config;
    }

    return result;
}

void Config::apply(httplib::Client &cl) const
{
    // Headers
    httplib::Headers httpLibHeaders{headers.begin(), headers.end()};

    // Cookies
    std::string cookieHeaderValue;
    for (const auto& cookie : cookies) {
        if (!cookieHeaderValue.empty())
            cookieHeaderValue += "; ";
        cookieHeaderValue += cookie.first + "=" + cookie.second;
    }
    if (!cookieHeaderValue.empty())
        httpLibHeaders.insert({"Cookie", cookieHeaderValue});

    // Basic Authentication
    if (auth) {
        auto password = auth->password;
        if (!auth->keychain.empty()) {
            password = secret::load(auth->keychain, auth->user);
        }
        httpLibHeaders.insert(
            httplib::make_basic_authentication_header(auth->user, password));
    }

    // Proxy Settings
    if (proxy) {
        cl.set_proxy(proxy->host.c_str(), proxy->port);

        auto password = proxy->password;
        if (!proxy->keychain.empty())
            password = secret::load(proxy->keychain, proxy->user);

        if (!proxy->user.empty())
            cl.set_proxy_basic_auth(
                proxy->user.c_str(), password.c_str());
    }

    cl.set_default_headers(httpLibHeaders);
}

Config& Config::operator |= (Config const& other) {
    cookies.insert(other.cookies.begin(), other.cookies.end());
    headers.insert(other.headers.begin(), other.headers.end());
    query.insert(other.query.begin(), other.query.end());
    if (other.auth)
        auth = other.auth;
    if (other.proxy)
        proxy = other.proxy;
    if (other.apiKey)
        apiKey = other.apiKey;
    return *this;
}
