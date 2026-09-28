#include "update_check.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace olas {

/* ---- build-time constants (from CMake) ---- */

#ifndef OLAS_GIT_SHA
#define OLAS_GIT_SHA "unknown"
#endif
#ifndef OLAS_GIT_BRANCH
#define OLAS_GIT_BRANCH "unknown"
#endif
#ifndef OLAS_BUILD_DATE
#define OLAS_BUILD_DATE "unknown"
#endif
#ifndef OLAS_UPDATE_REPO
#define OLAS_UPDATE_REPO ""
#endif

const char* build_commit_sha(void) { return OLAS_GIT_SHA; }
const char* build_git_branch(void) { return OLAS_GIT_BRANCH; }
const char* build_timestamp(void)  { return OLAS_BUILD_DATE; }

/* ---- UTF-8 <-> UTF-16 ---- */

static std::wstring u8_to_w(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

/* ---- WinHTTP GET ---- */
/* Returns the response body on HTTP 200, or an empty string on any failure.
 * The error string (if `err_out` is non-null) is populated with a short
 * human-readable description. */

static std::string winhttp_get(const std::wstring& host,
                               const std::wstring& path,
                               int timeout_ms,
                               std::string* err_out)
{
    auto fail = [&](const char* msg) -> std::string {
        if (err_out) *err_out = msg;
        return {};
    };

    HINTERNET session = WinHttpOpen(
        L"OLAS-UpdateCheck/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return fail("WinHttpOpen failed");

    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET conn = WinHttpConnect(session, host.c_str(),
                                    INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        return fail("WinHttpConnect failed");
    }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(),
                                       nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return fail("WinHttpOpenRequest failed");
    }

    /* GitHub API requires a User-Agent; WinHTTP sends one by default, but
     * we set it explicitly so we control what shows up in their logs. */
    WinHttpAddRequestHeaders(req, L"User-Agent: OLAS-UpdateCheck/1.0\r\n",
                             (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    /* Ask for the classic REST v3 API. */
    WinHttpAddRequestHeaders(req,
        L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
        WINHTTP_ADDREQ_FLAG_ADD);

    std::string result;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return fail("WinHttpSendRequest failed");
    }

    if (!WinHttpReceiveResponse(req, nullptr)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return fail("WinHttpReceiveResponse failed");
    }

    DWORD status = 0, status_sz = sizeof(status);
    WinHttpQueryHeaders(req,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz,
        WINHTTP_NO_HEADER_INDEX);

    if (status == 404) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return fail("repo not found or not public");
    }
    if (status != 200) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "HTTP %lu", (unsigned long)status);
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return fail(buf);
    }

    /* Read body in chunks. */
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::vector<char> buf(avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &got) || got == 0) break;
        result.append(buf.data(), got);
        if (result.size() > 1u * 1024 * 1024) break; /* sanity cap: 1 MB */
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);

    if (err_out) err_out->clear();
    return result;
}

/* ---- Minimal JSON string extraction ---- */
/* Looks for the first "key":"value" and returns value.  Sufficient for
 * GitHub's commits API where `sha` appears once at the top level before any
 * nested occurrences. */

static std::string json_first_string(const std::string& json,
                                     const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};

    pos += needle.size();
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' ||
            json[pos] == '\r' || json[pos] == '\n')) ++pos;
    if (pos >= json.size() || json[pos] != ':') return {};
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' ||
            json[pos] == '\r' || json[pos] == '\n')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;

    size_t end = pos;
    while (end < json.size() && json[end] != '"') {
        if (json[end] == '\\' && end + 1 < json.size()) end += 2;
        else ++end;
    }
    if (end >= json.size()) return {};
    return json.substr(pos, end - pos);
}

/* ---- public API ---- */

std::string repo_web_url(const std::string& owner_repo) {
    return "https://github.com/" + owner_repo;
}

UpdateInfo check_for_updates(const std::string& owner_repo, int timeout_ms) {
    UpdateInfo info;

    /* No repo configured -> silently skip. */
    if (owner_repo.empty()) {
        info.error = "update check disabled (no repo configured)";
        return info;
    }

    /* Built without git -> we don't know our own SHA, skip. */
    std::string local_sha = build_commit_sha();
    if (local_sha.empty() || local_sha == "unknown") {
        info.error = "local build has no git SHA";
        return info;
    }

    /* Find the default branch tip.  We use the commits API for `main`; if
     * your repo's default branch is `master`, change this string. */
    std::wstring host = L"api.github.com";
    std::string path_utf8 = "/repos/" + owner_repo + "/commits/main";
    std::wstring path = u8_to_w(path_utf8);

    std::string err;
    std::string body = winhttp_get(host, path, timeout_ms, &err);
    if (body.empty()) {
        info.error = err.empty() ? "empty response" : err;
        return info;
    }

    std::string remote = json_first_string(body, "sha");
    if (remote.empty()) {
        info.error = "could not parse 'sha' from response";
        return info;
    }

    info.completed  = true;
    info.remote_sha = remote;
    info.html_url   = repo_web_url(owner_repo);
    info.outdated   = (remote != local_sha);

    return info;
}

} // namespace olas