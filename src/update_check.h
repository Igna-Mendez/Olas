#pragma once

#include <string>

namespace olas {

/* The commit SHA this binary was built from. "unknown" if the build did not
 * have git info (e.g. built from a source zip). */
const char* build_commit_sha();
const char* build_git_branch();
const char* build_timestamp();

/* Result of an update check. */
struct UpdateInfo {
    bool        completed   = false;  /* did the HTTP round-trip finish?  */
    bool        outdated    = false;  /* remote SHA differs from ours?    */
    std::string remote_sha;           /* 40-char hex, empty on error      */
    std::string html_url;             /* browse URL for the user          */
    std::string error;                /* human-readable, empty on success */
};

/* Blocking.  Call from a worker thread.  timeout_ms applies to each of
 * connect / send / receive separately.  Returns an UpdateInfo with
 * completed=false and a populated error on failure. */
UpdateInfo check_for_updates(const std::string& owner_repo,
                             int timeout_ms = 5000);

/* The GitHub HTML URL for browsing the repo (no trailing slash). */
std::string repo_web_url(const std::string& owner_repo);

} // namespace olas