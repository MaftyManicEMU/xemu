//
// xemu XB.Live integration
//
// Copyright (C) 2026
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#include "common.hh"
#include "xblive-service.hh"

#include "ui/xemu-notifications.h"
#include "ui/xemu-settings.h"
#include "xemu-xbe.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <glib/gstdio.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

constexpr const char *kAuthBaseURL = "https://auth.insigniastats.live/api";
constexpr const char *kXBLiveBaseURL = "https://xb.live/api";
constexpr const char *kCloudSaveURL = "https://xb.live/api/me/xbox-saves";
constexpr const char *kXboxAccountURL = "https://xb.live/api/me/xbox-account";
constexpr const char *kSpecialAccess = "xemu";
constexpr const char *kConsoleIDScheme = "v2";
constexpr int64_t kUploadMaxBytes = 32LL * 1024LL * 1024LL;
constexpr long kLoginTimeoutSeconds = 150;

static std::once_flag curl_init_once;

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string error;

    bool ok() const
    {
        return error.empty() && status >= 200 && status < 300;
    }
};

struct CloudRemoteEntry {
    std::string console_id;
    std::string profile;
    std::string title_id;
    std::string fingerprint;
    std::string content_hash;
    long long save_modified = 0;
    bool no_sync = false;
};

struct EepromIdentity {
    std::vector<uint8_t> data;
    std::string serial;
    std::string hdd_key_hex;
};

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

static void ensure_curl()
{
    std::call_once(curl_init_once, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

static std::string join_url(const char *base, const char *path)
{
    std::string out(base);
    if (!out.empty() && out.back() != '/') {
        out += "/";
    }
    out += path;
    return out;
}

static std::string curl_escape(const std::string &value)
{
    ensure_curl();
    CURL *curl = curl_easy_init();
    if (!curl) {
        return value;
    }

    char *escaped = curl_easy_escape(curl, value.c_str(), (int)value.size());
    std::string out = escaped ? escaped : value;
    if (escaped) {
        curl_free(escaped);
    }
    curl_easy_cleanup(curl);
    return out;
}

static HttpResponse http_request(const std::string &method,
                                 const std::string &url,
                                 const std::vector<std::string> &headers = {},
                                 const std::string &body = {},
                                 long timeout_seconds = 30)
{
    ensure_curl();
    HttpResponse response;
    CURL *curl = curl_easy_init();
    if (!curl) {
        response.error = "Unable to initialize libcurl.";
        return response;
    }

    struct curl_slist *header_list = NULL;
    for (const auto &header : headers) {
        header_list = curl_slist_append(header_list, header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "xemu-xblive/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    } else if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        }
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        response.error = curl_easy_strerror(rc);
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);

    if (header_list) {
        curl_slist_free_all(header_list);
    }
    curl_easy_cleanup(curl);
    return response;
}

static json parse_json(const std::string &body)
{
    json parsed = json::parse(body, nullptr, false);
    return parsed.is_discarded() ? json() : parsed;
}

static std::string json_string(const json &j, const char *key,
                               const std::string &fallback = "")
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return fallback;
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    if (it->is_number_integer()) {
        return std::to_string(it->get<long long>());
    }
    if (it->is_number_float()) {
        return std::to_string(it->get<double>());
    }
    return fallback;
}

static int json_int(const json &j, const char *key, int fallback = 0)
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return fallback;
    }
    if (it->is_number_integer()) {
        return it->get<int>();
    }
    if (it->is_number_float()) {
        return (int)it->get<double>();
    }
    if (it->is_string()) {
        try {
            return std::stoi(it->get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

static double json_double(const json &j, const char *key, double fallback = 0.0)
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return fallback;
    }
    if (it->is_number()) {
        return it->get<double>();
    }
    if (it->is_string()) {
        try {
            return std::stod(it->get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

static bool json_bool(const json &j, const char *key, bool fallback = false)
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return fallback;
    }
    if (it->is_boolean()) {
        return it->get<bool>();
    }
    if (it->is_number_integer()) {
        return it->get<int>() != 0;
    }
    if (it->is_string()) {
        std::string v = it->get<std::string>();
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
            return (char)std::tolower(c);
        });
        return v == "true" || v == "yes" || v == "1" || v == "online";
    }
    return fallback;
}

static int response_count(const json &j, const char *array_key, const char *count_key = "count")
{
    int count = json_int(j, count_key, -1);
    if (count >= 0) {
        return count;
    }

    auto it = j.find(array_key);
    if (it != j.end() && it->is_array()) {
        return (int)it->size();
    }
    return 0;
}

static std::string api_error_message(const HttpResponse &response,
                                     const char *fallback)
{
    if (!response.error.empty()) {
        return response.error;
    }

    json j = parse_json(response.body);
    std::string error = json_string(j, "error");
    if (!error.empty()) {
        return error;
    }

    std::ostringstream oss;
    oss << fallback << " (HTTP " << response.status << ")";
    return oss.str();
}

static std::string session_file_path()
{
    char *path = g_build_filename(xemu_settings_get_base_path(),
                                  "xblive-session.json", nullptr);
    std::string out(path);
    g_free(path);
    return out;
}

static bool read_text_file(const std::string &path, std::string &out)
{
    gchar *contents = NULL;
    gsize len = 0;
    GError *err = NULL;
    if (!g_file_get_contents(path.c_str(), &contents, &len, &err)) {
        if (err) {
            g_error_free(err);
        }
        return false;
    }
    out.assign(contents, len);
    g_free(contents);
    return true;
}

static bool write_text_file(const std::string &path, const std::string &value)
{
    GError *err = NULL;
    bool ok = g_file_set_contents(path.c_str(), value.c_str(), value.size(), &err);
    if (err) {
        g_error_free(err);
    }
    return ok;
}

static void remove_session_file()
{
    std::string path = session_file_path();
    g_unlink(path.c_str());
}

static void save_session_file(const XBLiveServiceState &state)
{
    json stored = {
        {"sessionKey", state.session_key},
        {"username", state.username},
        {"email", state.email},
        {"special_access", kSpecialAccess},
    };
    write_text_file(session_file_path(), stored.dump(2));
}

static std::string normalize_title_id(const std::string &raw, bool lowercase)
{
    std::string out;
    for (unsigned char c : raw) {
        if (std::isxdigit(c)) {
            out.push_back((char)(lowercase ? std::tolower(c) : std::toupper(c)));
        }
    }
    return out.size() == 8 ? out : "";
}

static std::string title_id_from_xbe(struct xbe *xbe)
{
    if (!xbe || !xbe->cert) {
        return "";
    }

    std::ostringstream oss;
    oss << std::hex << std::nouppercase << std::setfill('0') << std::setw(8)
        << (uint32_t)xbe->cert->m_titleid;
    return oss.str();
}

static std::string title_name_from_xbe(struct xbe *xbe)
{
    if (!xbe || !xbe->cert) {
        return "Xbox title";
    }

    gchar *title = g_utf16_to_utf8((const gunichar2 *)xbe->cert->m_title_name,
                                   40, NULL, NULL, NULL);
    std::string out = title && title[0] ? title : "Xbox title";
    g_free(title);
    return out;
}

static std::string base64_bytes(const uint8_t *data, size_t len)
{
    gchar *encoded = g_base64_encode(data, len);
    std::string out = encoded ? encoded : "";
    g_free(encoded);
    return out;
}

static std::string hex_bytes(const uint8_t *data, size_t len)
{
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << (unsigned)data[i];
    }
    return oss.str();
}

struct XboxEEPROMVersion {
    uint32_t first[5];
    uint32_t second[5];
};

static const XboxEEPROMVersion kEepromVersions[] = {
    {{0x85F9E51A, 0xE04613D2, 0x6D86A50C, 0x77C32E3C, 0x4BD717A4},
     {0x5D7A9C6B, 0xE1922BEB, 0xB82CCDBC, 0x3137AB34, 0x486B52B3}},
    {{0x72127625, 0x336472B9, 0xBE609BEA, 0xF55E226B, 0x99958DAC},
     {0x76441D41, 0x4DE82659, 0x2E8EF85E, 0xB256FACA, 0xC4FE2DE8}},
    {{0x39B06E79, 0xC9BD25E8, 0xDBC6B498, 0x40B4389D, 0x86BBD7ED},
     {0x9B49BED3, 0x84B430FC, 0x6B8749CD, 0xEBFE5FE5, 0xD96E7393}},
    {{0x8058763A, 0xF97D4E0E, 0x865A9762, 0x8A3D920D, 0x08995B2C},
     {0x01075307, 0xA2F1E037, 0x1186EEEA, 0x88DA9992, 0x168A5609}},
};

class XboxSHA1 {
public:
    XboxSHA1(const uint32_t initial[5], uint32_t initial_bit_length)
        : m_hash{initial[0], initial[1], initial[2], initial[3], initial[4]},
          m_bit_length(initial_bit_length)
    {
        std::fill(std::begin(m_block), std::end(m_block), 0);
    }

    void input(const uint8_t *data, size_t len)
    {
        for (size_t i = 0; i < len; ++i) {
            m_block[m_block_index++] = data[i];
            m_bit_length += 8;
            if (m_block_index == 64) {
                process();
            }
        }
    }

    std::vector<uint8_t> result()
    {
        pad();
        std::vector<uint8_t> out;
        out.reserve(20);
        for (uint32_t word : m_hash) {
            out.push_back((uint8_t)((word >> 24) & 0xff));
            out.push_back((uint8_t)((word >> 16) & 0xff));
            out.push_back((uint8_t)((word >> 8) & 0xff));
            out.push_back((uint8_t)(word & 0xff));
        }
        return out;
    }

private:
    static uint32_t rol(uint32_t value, int bits)
    {
        return (value << bits) | (value >> (32 - bits));
    }

    void pad()
    {
        m_block[m_block_index++] = 0x80;
        if (m_block_index > 56) {
            while (m_block_index < 64) {
                m_block[m_block_index++] = 0;
            }
            process();
        }
        while (m_block_index < 56) {
            m_block[m_block_index++] = 0;
        }

        m_block[56] = 0;
        m_block[57] = 0;
        m_block[58] = 0;
        m_block[59] = 0;
        m_block[60] = (uint8_t)((m_bit_length >> 24) & 0xff);
        m_block[61] = (uint8_t)((m_bit_length >> 16) & 0xff);
        m_block[62] = (uint8_t)((m_bit_length >> 8) & 0xff);
        m_block[63] = (uint8_t)(m_bit_length & 0xff);
        process();
    }

    void process()
    {
        static const uint32_t constants[] = {
            0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6
        };
        uint32_t words[80] = {0};
        for (int i = 0; i < 16; ++i) {
            int b = i * 4;
            words[i] = ((uint32_t)m_block[b] << 24) |
                       ((uint32_t)m_block[b + 1] << 16) |
                       ((uint32_t)m_block[b + 2] << 8) |
                       (uint32_t)m_block[b + 3];
        }
        for (int i = 16; i < 80; ++i) {
            words[i] = rol(words[i - 3] ^ words[i - 8] ^
                           words[i - 14] ^ words[i - 16], 1);
        }

        uint32_t a = m_hash[0], b = m_hash[1], c = m_hash[2];
        uint32_t d = m_hash[3], e = m_hash[4];
        for (int i = 0; i < 80; ++i) {
            int round = i / 20;
            uint32_t fn;
            if (round == 0) {
                fn = (b & c) | ((~b) & d);
            } else if (round == 2) {
                fn = (b & c) | (b & d) | (c & d);
            } else {
                fn = b ^ c ^ d;
            }
            uint32_t temp = rol(a, 5) + fn + e + words[i] + constants[round];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }

        m_hash[0] += a;
        m_hash[1] += b;
        m_hash[2] += c;
        m_hash[3] += d;
        m_hash[4] += e;
        m_block_index = 0;
        std::fill(std::begin(m_block), std::end(m_block), 0);
    }

    uint32_t m_hash[5];
    uint8_t m_block[64];
    uint32_t m_bit_length;
    int m_block_index = 0;
};

static std::vector<uint8_t> xbox_hmac_sha1(const XboxEEPROMVersion &version,
                                           const uint8_t *data, size_t len)
{
    XboxSHA1 first(version.first, 512);
    first.input(data, len);
    std::vector<uint8_t> first_result = first.result();

    XboxSHA1 second(version.second, 512);
    second.input(first_result.data(), first_result.size());
    return second.result();
}

static void rc4_crypt(const std::vector<uint8_t> &seed, std::vector<uint8_t> &data)
{
    uint8_t state[256];
    for (int i = 0; i < 256; ++i) {
        state[i] = (uint8_t)i;
    }

    int seed_index = 0;
    int swap_index = 0;
    for (int i = 0; i < 256; ++i) {
        swap_index = (seed[seed_index] + state[i] + swap_index) % 256;
        seed_index = (seed_index + 1) % seed.size();
        std::swap(state[i], state[swap_index]);
    }

    int x = 0;
    int y = 0;
    for (auto &byte : data) {
        x = (x + 1) % 256;
        y = (state[x] + y) % 256;
        std::swap(state[x], state[y]);
        byte ^= state[(state[x] + state[y]) % 256];
    }
}

static bool load_eeprom_identity(const char *path, EepromIdentity &identity,
                                 std::string &error)
{
    if (!path || !path[0]) {
        error = "Set an EEPROM path in System settings before cloud sync.";
        return false;
    }

    gchar *contents = NULL;
    gsize len = 0;
    GError *gerr = NULL;
    if (!g_file_get_contents(path, &contents, &len, &gerr)) {
        error = gerr ? gerr->message : "Unable to read EEPROM.";
        if (gerr) {
            g_error_free(gerr);
        }
        return false;
    }

    identity.data.assign((uint8_t *)contents, (uint8_t *)contents + len);
    g_free(contents);

    if (identity.data.size() != 256) {
        std::ostringstream oss;
        oss << "The EEPROM must be 256 bytes; found " << identity.data.size() << ".";
        error = oss.str();
        return false;
    }

    const uint8_t *bytes = identity.data.data();
    for (int i = 0; i < 12; ++i) {
        uint8_t c = bytes[0x34 + i];
        if (c < 0x20 || c > 0x7e) {
            break;
        }
        identity.serial.push_back((char)c);
    }

    std::vector<uint8_t> original_hash(bytes, bytes + 20);
    for (const auto &version : kEepromVersions) {
        std::vector<uint8_t> seed = xbox_hmac_sha1(version, original_hash.data(),
                                                   original_hash.size());
        std::vector<uint8_t> decrypted(bytes + 0x14, bytes + 0x30);
        rc4_crypt(seed, decrypted);
        std::vector<uint8_t> check = xbox_hmac_sha1(version, decrypted.data(),
                                                    decrypted.size());
        if (check == original_hash) {
            identity.hdd_key_hex = hex_bytes(decrypted.data() + 8, 16);
            return true;
        }
    }

    error = "Could not decrypt the EEPROM security section.";
    return false;
}

static uint64_t fnv1a_update(uint64_t hash, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static std::string fingerprint_file(const std::string &path, int64_t size,
                                    std::time_t modified)
{
    uint64_t hash = 14695981039346656037ULL;
    uint64_t size_le = (uint64_t)size;
    uint64_t modified_le = (uint64_t)modified;
    hash = fnv1a_update(hash, (uint8_t *)&size_le, sizeof(size_le));
    hash = fnv1a_update(hash, (uint8_t *)&modified_le, sizeof(modified_le));

    FILE *fp = qemu_fopen(path.c_str(), "rb");
    if (fp) {
        uint8_t buf[64 * 1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
            hash = fnv1a_update(hash, buf, n);
        }
        fclose(fp);
    }

    std::ostringstream oss;
    oss << std::hex << std::nouppercase << std::setfill('0') << std::setw(16)
        << hash;
    return oss.str();
}

static std::vector<std::string> list_local_archives(const std::string &directory)
{
    std::vector<std::string> paths;
    GDir *dir = g_dir_open(directory.c_str(), 0, NULL);
    if (!dir) {
        return paths;
    }

    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        std::string filename = name;
        std::string lower = filename;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return (char)std::tolower(c);
        });
        if (lower.size() <= 6 || lower.substr(lower.size() - 6) != ".dukex") {
            continue;
        }

        std::string title = normalize_title_id(filename.substr(0, filename.size() - 6), false);
        if (title.empty()) {
            continue;
        }

        char *path = g_build_filename(directory.c_str(), filename.c_str(), nullptr);
        paths.emplace_back(path);
        g_free(path);
    }
    g_dir_close(dir);
    std::sort(paths.begin(), paths.end());
    return paths;
}

static std::string basename_no_ext(const std::string &path)
{
    char *base = g_path_get_basename(path.c_str());
    std::string name = base ? base : path;
    g_free(base);
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) {
        name.resize(dot);
    }
    return name;
}

static std::string default_manifest_base64(const std::string &title_id)
{
    json manifest = {
        {"title_id", title_id},
        {"title_name", title_id},
        {"saves", json::array()},
    };
    std::string dumped = manifest.dump();
    return base64_bytes((const uint8_t *)dumped.data(), dumped.size());
}

static std::string upload_console_data(const EepromIdentity &identity,
                                       const std::string &session_key)
{
    json body = {
        {"sessionKey", session_key},
        {"console_id_scheme", kConsoleIDScheme},
        {"serial", identity.serial},
        {"hdd_key_hex", identity.hdd_key_hex},
        {"eeprom_base64", base64_bytes(identity.data.data(), identity.data.size())},
    };

    HttpResponse response = http_request(
        "POST", join_url(kCloudSaveURL, "console-data"),
        {
            "Content-Type: application/json",
            "X-Session-Key: " + session_key,
            std::string("X-Console-Id-Scheme: ") + kConsoleIDScheme,
        },
        body.dump());

    if (!response.ok()) {
        return "unknown";
    }

    json parsed = parse_json(response.body);
    std::string console_id = json_string(parsed, "console_id");
    return console_id.empty() ? "unknown" : console_id;
}

static std::string trimmed_copy(const std::string &value)
{
    size_t first = 0;
    while (first < value.size() && std::isspace((unsigned char)value[first])) {
        first++;
    }

    size_t last = value.size();
    while (last > first && std::isspace((unsigned char)value[last - 1])) {
        last--;
    }

    return value.substr(first, last - first);
}

static bool is_content_hash(const std::string &value)
{
    std::string normalized = trimmed_copy(value);
    if (normalized.size() < 16 || normalized.size() > 128) {
        return false;
    }

    return std::all_of(normalized.begin(), normalized.end(), [](unsigned char c) {
        return std::isxdigit(c);
    });
}

static std::vector<CloudRemoteEntry> parse_manifest_entries(const std::string &body)
{
    std::vector<CloudRemoteEntry> entries;
    std::set<std::string> seen;
    std::istringstream stream(body);
    std::string line;
    while (std::getline(stream, line)) {
        size_t equal = line.find('=');
        if (equal == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, equal);
        std::vector<std::string> parts;
        std::stringstream key_stream(key);
        std::string part;
        while (std::getline(key_stream, part, ':')) {
            parts.push_back(part);
        }

        CloudRemoteEntry entry;
        if (parts.size() >= 3) {
            entry.console_id = parts.front();
            entry.title_id = normalize_title_id(parts.back(), false);
            for (size_t i = 1; i + 1 < parts.size(); ++i) {
                if (!entry.profile.empty()) {
                    entry.profile += ":";
                }
                entry.profile += parts[i];
            }
        } else if (parts.size() == 2) {
            entry.console_id = parts[0];
            entry.title_id = normalize_title_id(parts[1], false);
        } else if (parts.size() == 1) {
            entry.title_id = normalize_title_id(parts[0], false);
        }

        std::string value = line.substr(equal + 1);
        std::vector<std::string> value_parts;
        std::stringstream value_stream(value);
        while (std::getline(value_stream, part, '|')) {
            value_parts.push_back(trimmed_copy(part));
        }

        if (!value_parts.empty()) {
            entry.fingerprint = value_parts[0];
        }
        for (size_t i = 1; i < value_parts.size(); ++i) {
            std::string part_value = value_parts[i];
            std::string lower = part_value;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                return (char)std::tolower(c);
            });
            if (lower == "nosync") {
                entry.no_sync = true;
                continue;
            }
            if (is_content_hash(part_value)) {
                entry.content_hash = lower;
                continue;
            }
            try {
                long long parsed_modified = std::stoll(part_value);
                if (parsed_modified > 0) {
                    entry.save_modified = parsed_modified;
                }
            } catch (...) {
            }
        }

        if (entry.title_id.empty() || entry.no_sync || entry.fingerprint.empty()) {
            continue;
        }

        std::string dedupe = entry.console_id + ":" + entry.profile + ":" + entry.title_id;
        if (seen.insert(dedupe).second) {
            entries.push_back(entry);
        }
    }
    return entries;
}

static void collect_json_entries(const json &j, std::vector<CloudRemoteEntry> &entries)
{
    if (j.is_array()) {
        for (const auto &item : j) {
            collect_json_entries(item, entries);
        }
        return;
    }
    if (!j.is_object()) {
        return;
    }

    std::string title_id;
    for (const char *key : {"title_id", "titleid", "game_title_id"}) {
        title_id = normalize_title_id(json_string(j, key), false);
        if (!title_id.empty()) {
            break;
        }
    }

    if (!title_id.empty()) {
        CloudRemoteEntry entry;
        entry.title_id = title_id;
        entry.console_id = json_string(j, "console_id");
        entry.profile = json_string(j, "profile");
        entries.push_back(entry);
    }

    for (auto it = j.begin(); it != j.end(); ++it) {
        std::string key_title = normalize_title_id(it.key(), false);
        if (!key_title.empty()) {
            CloudRemoteEntry entry;
            entry.title_id = key_title;
            entries.push_back(entry);
        }
        collect_json_entries(it.value(), entries);
    }
}

static std::vector<CloudRemoteEntry> fetch_remote_entries(const std::string &session_key)
{
    HttpResponse manifest = http_request("GET", join_url(kCloudSaveURL, "manifest"),
        {"X-Session-Key: " + session_key});
    if (manifest.ok()) {
        auto entries = parse_manifest_entries(manifest.body);
        if (!entries.empty()) {
            return entries;
        }
    }

    HttpResponse list = http_request("GET", kCloudSaveURL,
        {"X-Session-Key: " + session_key});
    std::vector<CloudRemoteEntry> entries;
    if (list.ok()) {
        json parsed = parse_json(list.body);
        collect_json_entries(parsed, entries);
    }

    std::set<std::string> seen;
    std::vector<CloudRemoteEntry> unique;
    for (const auto &entry : entries) {
        if (entry.title_id.empty()) {
            continue;
        }
        std::string key = entry.console_id + ":" + entry.profile + ":" + entry.title_id;
        if (seen.insert(key).second) {
            unique.push_back(entry);
        }
    }
    return unique;
}

static bool should_skip_upload(const std::vector<CloudRemoteEntry> &manifest_entries,
                               const std::string &title_id,
                               const std::string &console_id,
                               const std::string &fingerprint,
                               long long modified)
{
    for (const auto &entry : manifest_entries) {
        if (entry.title_id != title_id || !entry.profile.empty()) {
            continue;
        }
        if (entry.save_modified > 0 && entry.save_modified >= modified) {
            return true;
        }
        if ((entry.console_id.empty() || entry.console_id == console_id) &&
            !entry.fingerprint.empty() && entry.fingerprint == fingerprint) {
            return true;
        }
    }
    return false;
}

static bool upload_archive(const std::string &archive_path,
                           const EepromIdentity &identity,
                           const std::string &session_key,
                           const std::string &console_id,
                           const std::vector<CloudRemoteEntry> &manifest_entries,
                           bool &skipped,
                           std::string &error)
{
    skipped = false;
    struct stat st;
    if (stat(archive_path.c_str(), &st) != 0) {
        error = g_strerror(errno);
        return false;
    }
    if (st.st_size > kUploadMaxBytes) {
        error = "Archive is over the 32 MiB XB.Live upload limit.";
        return false;
    }

    std::string title_id = normalize_title_id(basename_no_ext(archive_path), false);
    if (title_id.empty()) {
        error = "Archive filename must be an 8-digit title ID with .dukex extension.";
        return false;
    }

    gchar *contents = NULL;
    gsize len = 0;
    GError *gerr = NULL;
    if (!g_file_get_contents(archive_path.c_str(), &contents, &len, &gerr)) {
        error = gerr ? gerr->message : "Unable to read archive.";
        if (gerr) {
            g_error_free(gerr);
        }
        return false;
    }
    std::string body(contents, len);
    g_free(contents);

    std::string fingerprint = fingerprint_file(archive_path, st.st_size, st.st_mtime);
    if (should_skip_upload(manifest_entries, title_id, console_id,
                           fingerprint, (long long)st.st_mtime)) {
        skipped = true;
        return true;
    }

    std::ostringstream url;
    url << join_url(kCloudSaveURL, "game")
        << "?title_id=" << curl_escape(title_id)
        << "&console_id=" << curl_escape(console_id)
        << "&console_id_scheme=" << curl_escape(kConsoleIDScheme)
        << "&serial=" << curl_escape(identity.serial)
        << "&hdd_key_hex=" << curl_escape(identity.hdd_key_hex)
        << "&profile="
        << "&save_count=0"
        << "&total_bytes=" << st.st_size
        << "&fingerprint=" << curl_escape(fingerprint)
        << "&save_modified=" << (long long)st.st_mtime;

    std::string title_b64 = base64_bytes((const uint8_t *)title_id.data(), title_id.size());
    HttpResponse response = http_request(
        "POST", url.str(),
        {
            "Content-Type: application/octet-stream",
            "X-Session-Key: " + session_key,
            std::string("X-Console-Id-Scheme: ") + kConsoleIDScheme,
            "X-Title-Name-B64: " + title_b64,
            "X-Manifest-B64: " + default_manifest_base64(title_id),
        },
        body, 60);

    if (!response.ok()) {
        error = api_error_message(response, "Cloud save upload failed");
        return false;
    }
    return true;
}

static bool download_archive(const CloudRemoteEntry &entry,
                             const std::string &target_console_id,
                             const std::string &session_key,
                             const std::string &directory,
                             std::string &error)
{
    std::ostringstream url;
    url << join_url(kCloudSaveURL, ("download/" + entry.title_id).c_str())
        << "?console_id=" << curl_escape(entry.console_id.empty() ? "legacy" : entry.console_id)
        << "&target_console_id=" << curl_escape(target_console_id)
        << "&profile=" << curl_escape(entry.profile);

    HttpResponse response = http_request("GET", url.str(),
        {"X-Session-Key: " + session_key}, {}, 60);
    if (!response.ok()) {
        error = api_error_message(response, "Cloud save download failed");
        return false;
    }

    if (!response.body.empty() && response.body[0] == '{') {
        error = "XB.Live returned JSON instead of a save archive.";
        return false;
    }

    char *dest = g_build_filename(directory.c_str(),
                                  (entry.title_id + ".dukex").c_str(), nullptr);
    bool ok = write_text_file(dest, response.body);
    if (!ok) {
        error = "Unable to write downloaded archive.";
    }
    g_free(dest);
    return ok;
}

static std::string auth_header(const std::string &session_key)
{
    return "X-Session-Key: " + session_key;
}

} // namespace

XBLiveService &XBLiveService::Get()
{
    static XBLiveService service;
    return service;
}

XBLiveServiceState XBLiveService::Snapshot()
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_state;
}

std::string XBLiveService::DefaultCloudSavesDirectory()
{
    char *path = g_build_filename(xemu_settings_get_base_path(),
                                  "XB.Live Cloud Saves", nullptr);
    std::string out(path);
    g_free(path);
    return out;
}

void XBLiveService::LoadSession()
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_load_attempted) {
            return;
        }
        m_load_attempted = true;
        m_state.session_loaded = true;
    }

    std::string body;
    if (!read_text_file(session_file_path(), body)) {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_state.auth_message = "Not signed in.";
        return;
    }

    json stored = parse_json(body);
    std::string session_key = json_string(stored, "sessionKey");
    if (session_key.empty()) {
        remove_session_file();
        std::lock_guard<std::mutex> guard(m_mutex);
        m_state.auth_message = "Stored XB.Live session was empty.";
        return;
    }

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_state.session_key = session_key;
        m_state.username = json_string(stored, "username");
        m_state.email = json_string(stored, "email");
        m_state.signed_in = true;
        m_state.auth_message = "Restored stored XB.Live session.";
        m_shutdown_pulse_sent = false;
    }

    RefreshProfile(false);
}

void XBLiveService::Shutdown()
{
    std::string session_key;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_shutdown_pulse_sent) {
            return;
        }
        m_shutdown_pulse_sent = true;
        session_key = m_state.session_key;
        m_state.presence_active = false;
        m_state.presence_inflight = false;
        m_state.presence_title_id.clear();
        m_state.presence_game_name.clear();
        m_state.presence_message = "Sending XB.Live exit pulse...";
    }

    if (session_key.empty()) {
        return;
    }

    json body = {
        {"online", false},
    };
    HttpResponse response = http_request("POST",
        join_url(kXBLiveBaseURL, "emulator/presence"),
        {
            "Content-Type: application/json",
            auth_header(session_key),
        },
        body.dump(), 5);

    std::lock_guard<std::mutex> guard(m_mutex);
    if (response.ok()) {
        m_state.presence_message = "XB.Live exit pulse sent.";
    } else {
        m_state.presence_message =
            api_error_message(response, "XB.Live exit pulse failed");
    }
}

void XBLiveService::Login(const std::string &email, const std::string &password)
{
    if (email.empty() || password.empty()) {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_state.auth_message = "Enter an XB.Live email and password.";
        return;
    }

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_state.busy_login) {
            return;
        }
        m_state.busy_login = true;
        m_state.auth_message = "Signing in to XB.Live...";
    }

    std::thread([this, email, password]() {
        json body = {
            {"email", email},
            {"password", password},
            {"special_access", kSpecialAccess},
        };

        HttpResponse response = http_request("POST", join_url(kAuthBaseURL, "auth/login"),
            {"Content-Type: application/json"}, body.dump(), kLoginTimeoutSeconds);

        if (!response.ok()) {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_state.busy_login = false;
            m_state.signed_in = false;
            m_state.auth_message = api_error_message(response, "XB.Live sign in failed");
            return;
        }

        json parsed = parse_json(response.body);
        bool success = json_bool(parsed, "success", true);
        std::string session_key = json_string(parsed, "sessionKey");
        std::string username = json_string(parsed, "username");
        std::string response_email = json_string(parsed, "email", email);
        if (!success || session_key.empty()) {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_state.busy_login = false;
            m_state.signed_in = false;
            m_state.auth_message = json_string(parsed, "error", "XB.Live sign in failed.");
            return;
        }

        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_state.busy_login = false;
            m_state.signed_in = true;
            m_state.session_key = session_key;
            m_state.username = username;
            m_state.email = response_email;
            m_state.auth_message = "Signed in with special_access=xemu.";
            m_shutdown_pulse_sent = false;
            save_session_file(m_state);
        }

        xemu_queue_notification("XB.Live sign in complete");
        RefreshProfile(false);
    }).detach();
}

void XBLiveService::Logout()
{
    XBLiveServiceState snapshot = Snapshot();
    ForcePresenceOffline("logout");

    if (!snapshot.session_key.empty()) {
        json body = {
            {"sessionKey", snapshot.session_key},
        };
        std::thread([body]() {
            http_request("POST", join_url(kAuthBaseURL, "auth/logout"),
                         {"Content-Type: application/json"}, body.dump(), 10);
        }).detach();
    }

    remove_session_file();
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_state = XBLiveServiceState();
        m_state.session_loaded = true;
        m_state.auth_message = "Signed out.";
    }
}

void XBLiveService::RefreshProfile(bool refresh_server_cache)
{
    XBLiveServiceState snapshot = Snapshot();
    if (snapshot.session_key.empty()) {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_state.profile_message = "Sign in to refresh XB.Live profile data.";
        return;
    }

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_state.busy_refresh) {
            return;
        }
        m_state.busy_refresh = true;
        m_state.profile_message = refresh_server_cache ?
            "Refreshing XB.Live service cache..." : "Refreshing XB.Live profile...";
    }

    std::thread([this, snapshot, refresh_server_cache]() {
        XBLiveServiceState updated = snapshot;
        std::string session_key = snapshot.session_key;

        auto fetch_count = [&](const std::string &url,
                               const std::vector<std::string> &headers,
                               const char *array_key,
                               const char *count_key,
                               int &target) {
            HttpResponse response = http_request("GET", url, headers);
            if (!response.ok()) {
                return;
            }

            json parsed = parse_json(response.body);
            if (parsed.is_array()) {
                target = (int)parsed.size();
            } else if (parsed.is_object()) {
                target = response_count(parsed, array_key, count_key);
            }
        };

        const char *profile_path = refresh_server_cache ?
            "auth/refresh/profile" : "auth/profile";
        HttpResponse auth_profile = http_request(refresh_server_cache ? "POST" : "GET",
            join_url(kAuthBaseURL, profile_path), {auth_header(session_key)});
        if (auth_profile.status == 401) {
            remove_session_file();
            std::lock_guard<std::mutex> guard(m_mutex);
            m_state.busy_refresh = false;
            m_state.signed_in = false;
            m_state.session_key.clear();
            m_state.auth_message = "XB.Live session expired. Sign in again.";
            return;
        }
        if (auth_profile.ok()) {
            json profile = parse_json(auth_profile.body);
            updated.username = json_string(profile, "username", updated.username);
            updated.email = json_string(profile, "email", updated.email);
        }

        struct AuthFetch {
            const char *path;
            const char *refresh_path;
            const char *array_key;
            int *target;
        };
        AuthFetch auth_fetches[] = {
            {"auth/friends", "auth/refresh/friends", "friends", &updated.friends_count},
            {"auth/games", "auth/refresh/games", "games", &updated.games_count},
            {"auth/messages", "auth/refresh/messages", "messages", &updated.messages_count},
        };

        for (auto &fetch : auth_fetches) {
            const char *path = refresh_server_cache ? fetch.refresh_path : fetch.path;
            HttpResponse response = http_request(refresh_server_cache ? "POST" : "GET",
                join_url(kAuthBaseURL, path), {auth_header(session_key)});
            if (response.ok()) {
                json parsed = parse_json(response.body);
                *fetch.target = response_count(parsed, fetch.array_key);
            }
        }

        std::string username = updated.username;
        if (!username.empty()) {
            HttpResponse xb_profile = http_request("GET",
                join_url(kXBLiveBaseURL, ("profile/" + curl_escape(username)).c_str()));
            if (xb_profile.ok()) {
                json parsed = parse_json(xb_profile.body);
                updated.username = json_string(parsed, "username", updated.username);
                json profile = parsed.find("profile") != parsed.end() ? parsed["profile"] : json();
                json play_time = parsed.find("playTime") != parsed.end() ? parsed["playTime"] :
                    (parsed.find("play_time") != parsed.end() ? parsed["play_time"] : json());
                updated.linked_gamertag = json_string(profile, "linked_gamertag",
                    json_string(profile, "linkedGamertag", updated.linked_gamertag));
                updated.current_game = json_string(play_time, "currentGame",
                    json_string(play_time, "current_game", updated.current_game));
                updated.last_played_game = json_string(play_time, "lastPlayedGame",
                    json_string(play_time, "last_played_game", updated.last_played_game));
                updated.total_minutes = json_double(play_time, "totalMinutes",
                    json_double(play_time, "total_minutes", updated.total_minutes));
                updated.gamerscore = json_int(parsed, "achievementScore",
                    json_int(parsed, "achievement_score", updated.gamerscore));
                updated.achievements_count = json_int(parsed, "achievementCount",
                    json_int(parsed, "achievement_count", updated.achievements_count));
            }

            HttpResponse achievements = http_request("GET",
                join_url(kXBLiveBaseURL, ("profile/" + curl_escape(username) + "/achievements").c_str()));
            if (achievements.ok()) {
                json parsed = parse_json(achievements.body);
                if (parsed.is_array()) {
                    updated.achievements_count = (int)parsed.size();
                } else {
                    updated.achievements_count = response_count(parsed, "achievements",
                                                                "totalCount");
                    updated.gamerscore = json_int(parsed, "totalScore",
                                                  updated.gamerscore);
                }
            }

            fetch_count(join_url(kXBLiveBaseURL,
                    ("profile/" + curl_escape(username) + "/games-played").c_str()),
                {}, "games", "count", updated.games_played_count);
            fetch_count(join_url(kXBLiveBaseURL,
                    ("datasearch/player/" + curl_escape(username)).c_str()),
                {}, "results", "count", updated.leaderboard_rank_count);
        }

        HttpResponse events = http_request("GET", join_url(kXBLiveBaseURL, "events"));
        if (events.ok()) {
            json parsed = parse_json(events.body);
            updated.events_count = parsed.is_array() ? (int)parsed.size() :
                response_count(parsed, "events");
        }

        HttpResponse online_users = http_request("GET", join_url(kXBLiveBaseURL, "online-users"));
        if (online_users.ok()) {
            json parsed = parse_json(online_users.body);
            updated.active_games_count = parsed.is_object() ? (int)parsed.size() :
                (parsed.is_array() ? (int)parsed.size() : response_count(parsed, "games"));
        }

        fetch_count(join_url(kXBLiveBaseURL, "insignia-stats/online-24h?days=1"),
            {}, "points", "count", updated.activity_24h_points);
        fetch_count(join_url(kXBLiveBaseURL, "insignia-stats/online-24h?days=7"),
            {}, "points", "count", updated.activity_7d_points);

        std::vector<std::string> social_headers = {
            auth_header(session_key),
            "Accept: application/json",
        };
        fetch_count(join_url(kXBLiveBaseURL, "xbl/messages?kind=message"),
            social_headers, "messages", "count", updated.social_inbox_count);
        fetch_count(join_url(kXBLiveBaseURL, "xbl/messages/conversations"),
            social_headers, "conversations", "count", updated.conversations_count);
        fetch_count(join_url(kXBLiveBaseURL, "xbl/messageable-friends"),
            social_headers, "friends", "count", updated.messageable_friends_count);
        fetch_count(join_url(kXBLiveBaseURL, "xbl/friends/requests"),
            social_headers, "requests", "count", updated.friend_requests_count);
        fetch_count(join_url(kXBLiveBaseURL, "xbl/blocks"),
            social_headers, "blocks", "count", updated.blocks_count);

        HttpResponse account_settings = http_request("GET",
            join_url(kXboxAccountURL, ("settings?sessionKey=" + curl_escape(session_key)).c_str()),
            social_headers);
        if (account_settings.ok()) {
            json parsed = parse_json(account_settings.body);
            updated.xbox_live_profile_sync_status =
                json_bool(parsed, "enabled") ? "enabled" : "disabled";
        } else if (updated.xbox_live_profile_sync_status.empty()) {
            updated.xbox_live_profile_sync_status = "unknown";
        }

        updated.profile_message = "XB.Live profile, social, event, and activity data refreshed.";
        updated.last_refresh = std::time(nullptr);

        {
            std::lock_guard<std::mutex> guard(m_mutex);
            std::string keep_session = m_state.session_key;
            m_state = updated;
            m_state.session_key = keep_session.empty() ? updated.session_key : keep_session;
            m_state.busy_refresh = false;
            m_state.signed_in = true;
            m_state.session_loaded = true;
            save_session_file(m_state);
        }
    }).detach();
}

void XBLiveService::PushCloudArchives(const std::string &directory)
{
    XBLiveServiceState snapshot = Snapshot();
    if (snapshot.session_key.empty()) {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_state.cloud_message = "Sign in before syncing cloud saves.";
        return;
    }

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_state.busy_cloud_push) {
            return;
        }
        m_state.busy_cloud_push = true;
        m_state.cloud_message = "Uploading local .dukex archives to XB.Live...";
    }

    std::thread([this, snapshot, directory]() {
        g_mkdir_with_parents(directory.c_str(), 0755);

        std::string error;
        EepromIdentity identity;
        if (!load_eeprom_identity(g_config.sys.files.eeprom_path, identity, error)) {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_state.busy_cloud_push = false;
            m_state.cloud_message = error;
            return;
        }

        std::string console_id = upload_console_data(identity, snapshot.session_key);
        auto archives = list_local_archives(directory);
        if (archives.empty()) {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_state.busy_cloud_push = false;
            m_state.cloud_message = "No .dukex archives found in " + directory + ".";
            return;
        }

        int uploaded = 0;
        int skipped = 0;
        int failed = 0;
        std::string last_error;
        auto manifest_entries = fetch_remote_entries(snapshot.session_key);
        for (const auto &archive : archives) {
            std::string upload_error;
            bool archive_skipped = false;
            if (upload_archive(archive, identity, snapshot.session_key,
                               console_id, manifest_entries, archive_skipped,
                               upload_error)) {
                if (archive_skipped) {
                    skipped++;
                } else {
                    uploaded++;
                }
            } else {
                failed++;
                last_error = upload_error;
            }
        }

        std::ostringstream msg;
        msg << uploaded << " uploaded";
        if (skipped) {
            msg << ", " << skipped << " already current";
        }
        if (failed) {
            msg << ", " << failed << " failed";
            if (!last_error.empty()) {
                msg << ": " << last_error;
            }
        }
        msg << ".";

        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_state.busy_cloud_push = false;
            m_state.cloud_message = msg.str();
        }
        xemu_queue_notification("XB.Live cloud save push complete");
    }).detach();
}

void XBLiveService::PullCloudArchives(const std::string &directory)
{
    XBLiveServiceState snapshot = Snapshot();
    if (snapshot.session_key.empty()) {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_state.cloud_message = "Sign in before syncing cloud saves.";
        return;
    }

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_state.busy_cloud_pull) {
            return;
        }
        m_state.busy_cloud_pull = true;
        m_state.cloud_message = "Downloading remote XB.Live cloud archives...";
    }

    std::thread([this, snapshot, directory]() {
        g_mkdir_with_parents(directory.c_str(), 0755);

        std::string error;
        EepromIdentity identity;
        if (!load_eeprom_identity(g_config.sys.files.eeprom_path, identity, error)) {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_state.busy_cloud_pull = false;
            m_state.cloud_message = error;
            return;
        }

        std::string console_id = upload_console_data(identity, snapshot.session_key);
        auto entries = fetch_remote_entries(snapshot.session_key);
        if (entries.empty()) {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_state.busy_cloud_pull = false;
            m_state.cloud_message = "No remote XB.Live cloud save archives are available.";
            return;
        }

        int downloaded = 0;
        int failed = 0;
        std::string last_error;
        for (const auto &entry : entries) {
            std::string download_error;
            if (download_archive(entry, console_id, snapshot.session_key,
                                 directory, download_error)) {
                downloaded++;
            } else {
                failed++;
                last_error = download_error;
            }
        }

        std::ostringstream msg;
        msg << downloaded << " downloaded to " << directory;
        if (failed) {
            msg << ", " << failed << " failed";
            if (!last_error.empty()) {
                msg << ": " << last_error;
            }
        }
        msg << ".";

        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_state.busy_cloud_pull = false;
            m_state.cloud_message = msg.str();
        }
        xemu_queue_notification("XB.Live cloud save pull complete");
    }).detach();
}

void XBLiveService::ForcePresenceOffline(const char *reason)
{
    XBLiveServiceState snapshot = Snapshot();
    if (!snapshot.presence_active || snapshot.session_key.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_state.presence_active = false;
        m_state.presence_title_id.clear();
        m_state.presence_game_name.clear();
        m_state.presence_message = "Sending XB.Live offline presence...";
        m_next_presence_ping = 0;
    }

    std::string session_key = snapshot.session_key;
    std::string reason_text = reason ? reason : "manual";
    std::thread([this, session_key, reason_text]() {
        json body = {
            {"online", false},
        };
        HttpResponse response = http_request("POST",
            join_url(kXBLiveBaseURL, "emulator/presence"),
            {
                "Content-Type: application/json",
                auth_header(session_key),
            },
            body.dump(), 10);

        std::lock_guard<std::mutex> guard(m_mutex);
        if (response.ok()) {
            m_state.presence_message = "XB.Live offline presence sent (" + reason_text + ").";
        } else {
            m_state.presence_message = api_error_message(response, "XB.Live offline presence failed");
        }
    }).detach();
}

void XBLiveService::TickPresence()
{
    LoadSession();

    XBLiveServiceState snapshot = Snapshot();
    if (!snapshot.signed_in || snapshot.session_key.empty()) {
        return;
    }

    bool running = runstate_is_running();
    std::string title_id;
    std::string game_name;
    if (running) {
        struct xbe *xbe = xemu_get_xbe_info();
        title_id = title_id_from_xbe(xbe);
        game_name = title_name_from_xbe(xbe);
    }

    std::time_t now = std::time(nullptr);
    bool should_send = false;
    bool send_online = false;
    std::string send_title;
    std::string send_game;

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_state.presence_inflight) {
            return;
        }

        if (!title_id.empty()) {
            if (!m_state.presence_active ||
                m_state.presence_title_id != title_id) {
                m_state.presence_active = true;
                m_state.presence_title_id = title_id;
                m_state.presence_game_name = game_name;
                m_next_presence_ping = 0;
            }

            if (now >= m_next_presence_ping) {
                should_send = true;
                send_online = true;
                send_title = title_id;
                send_game = game_name;
                m_state.presence_inflight = true;
                m_state.presence_message = "Sending XB.Live presence heartbeat...";
                m_next_presence_ping = now + 300;
            }
        } else if (m_state.presence_active) {
            should_send = true;
            send_online = false;
            send_game = m_state.presence_game_name;
            m_state.presence_active = false;
            m_state.presence_title_id.clear();
            m_state.presence_game_name.clear();
            m_state.presence_inflight = true;
            m_state.presence_message = "Sending XB.Live offline presence...";
            m_next_presence_ping = 0;
        }
    }

    if (!should_send) {
        return;
    }

    std::string session_key = snapshot.session_key;
    std::thread([this, session_key, send_title, send_game, send_online]() {
        json body;
        body["online"] = send_online;
        if (send_online) {
            body["title_id"] = normalize_title_id(send_title, true);
        }

        HttpResponse response = http_request("POST",
            join_url(kXBLiveBaseURL, "emulator/presence"),
            {
                "Content-Type: application/json",
                auth_header(session_key),
            },
            body.dump(), 10);

        std::lock_guard<std::mutex> guard(m_mutex);
        m_state.presence_inflight = false;
        if (!response.ok()) {
            m_state.presence_message = api_error_message(response,
                "XB.Live emulator presence failed");
            if (response.status == 401) {
                m_state.signed_in = false;
                m_state.session_key.clear();
                remove_session_file();
            }
            return;
        }

        json parsed = parse_json(response.body);
        int recommended = json_int(parsed, "recommendedPingMs", 300000);
        if (recommended < 30000) {
            recommended = 300000;
        }
        m_next_presence_ping = std::time(nullptr) + recommended / 1000;

        std::string credited = json_string(parsed, "creditedMinutes");
        std::string total = json_string(parsed, "totalMinutes");
        if (send_online) {
            m_state.presence_message = "Live on XB.Live: " +
                (json_string(parsed, "gameName", send_game));
            if (!credited.empty() || !total.empty()) {
                m_state.presence_message += " (credited " +
                    (credited.empty() ? "0" : credited) + " min, total " +
                    (total.empty() ? "0" : total) + " min)";
            }
        } else {
            m_state.presence_message = "XB.Live offline presence sent.";
        }
    }).detach();
}
