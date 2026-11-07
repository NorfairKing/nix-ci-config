#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include <nix/util/ref.hh>
#include <nix/util/source-accessor.hh>

#include "pure.hh"

namespace nixciconfig {

// Thrown when the sources could not be read the fast way for a reason that
// says nothing about the repository: a request that failed, or a response the
// contents API would not inline.
//
// It carries no evaluation meaning. An evaluation that raises it has not
// failed, it has only turned out to be unreadable this way, and is redone
// against Nix's own fetcher.
struct FallbackNeeded : std::exception {
    std::string reason;
    explicit FallbackNeeded(std::string reason) : reason(std::move(reason)) {}
    const char * what() const noexcept override { return this->reason.c_str(); }
};

// Thrown when the sources could not be read at all: reaching the flake or one
// of its inputs failed.
//
// Distinct from an evaluation error because it says nothing about the
// repository and may well succeed a minute later, so a job that meets it is
// worth retrying rather than failing outright.
struct FetchFailed : std::exception {
    std::string message;
    explicit FetchFailed(std::string message) : message(std::move(message)) {}
    const char * what() const noexcept override { return this->message.c_str(); }
};

// Thrown when a configuration reads a file that is not the configuration.
//
// Reading the whole repository does not permit imports between configuration
// files: each root or fragment is evaluated with its own access policy.
struct NotSelfContained : std::exception {
    std::string path;
    std::string message;
    explicit NotSelfContained(std::string path)
        : path(std::move(path)), message("the configuration reads " + this->path) {}
    const char * what() const noexcept override { return this->message.c_str(); }
};

// What a configuration file is allowed to read, and what it read that it was
// not.
//
// Shared by whichever accessor the sources came through, so that the rule is
// the same one whether they were read a file at a time or cloned. The
// violation is recorded as well as thrown because the evaluator may wrap an
// exception raised underneath it, so what was recorded decides rather than
// what came out.
// [check:tag SelfContainedConfiguration] The documentation tells users a
// configuration files cannot import each other. Keep it true.
struct AccessPolicy {
    std::set<std::string> allowed;
    std::optional<std::string> violation;

    explicit AccessPolicy(std::set<std::string> allowed) : allowed(std::move(allowed)) {}

    // Answers whether the path may be read, recording the first one that may
    // not.
    bool permits(const std::string & relative);
};

// A read-only view of the files a NixCI configuration is made of, read one per
// request through the forge's contents API, with batched GitHub fragment reads.
//
// The loader may list nix-ci.d and admit its regular .nix files. Evaluation
// gets a further restricted accessor for each individual file.
struct ForgeAccessor : nix::SourceAccessor {
    // What the forge said about each path. Held behind a shared pointer so that
    // a second accessor reading the same repository -- the flake output is read
    // through one of its own -- reuses what the first already fetched rather
    // than asking again.
    using Cache = std::map<std::string, ContentsResponse>;

    ForgeAccessor(
        ForgeRepo repo,
        std::optional<std::string> token,
        std::shared_ptr<AccessPolicy> policy,
        std::shared_ptr<Cache> cache = {});

    std::optional<Stat> maybeLstat(const nix::CanonPath & path) override;
    void readFile(
        const nix::CanonPath & path,
        nix::Sink & sink,
        nix::fun<void(uint64_t)> sizeCallback = [](uint64_t) {}) override;
    DirEntries readDirectory(const nix::CanonPath & path) override;
    std::string readLink(const nix::CanonPath & path) override;

    // Whether anything asked of this accessor could not be served. Checked
    // after an evaluation rather than only where 'FallbackNeeded' is thrown,
    // because the evaluator may wrap an exception raised underneath it.
    bool fallbackNeeded() const { return fallbackNeeded_; }

    // Why it could not be served, for a run that has to say why it took the
    // slow way round.
    const std::string & fallbackReason() const { return fallbackReason_; }

    // Record that the sources have to be read another way, without raising:
    // for a caller that has decided the answer it got cannot be trusted rather
    // than one that could not get an answer at all.
    void noteFallback(const std::string & reason);

    // How many requests were made, for the run's own report.
    size_t requestCount() const { return requestCount_; }

private:
    ForgeRepo repo;
    std::optional<std::string> token;
    std::shared_ptr<AccessPolicy> policy;
    // An evaluation stats a file before reading it, so without this every file
    // would cost two requests.
    std::shared_ptr<Cache> cache;
    bool fallbackNeeded_ = false;
    std::string fallbackReason_;
    size_t requestCount_ = 0;

    const ContentsResponse & fetch(const nix::CanonPath & path);
    void fetchGitHubFragments(const std::vector<std::string> & files);
    [[noreturn]] void fallback(const std::string & reason);
};

// Holds evaluation to the same allowed files regardless of source transport.
//
// The rule is about what a configuration may be made of, not about how its
// sources happened to arrive, so it applies to a repository Nix fetched
// exactly as it does to files read over a forge API. Without this a
// configuration that imports another file would be rejected on a forge this
// has an API for and accepted everywhere else.
struct RestrictedAccessor : nix::SourceAccessor {
    RestrictedAccessor(nix::ref<nix::SourceAccessor> inner, std::shared_ptr<AccessPolicy> policy);

    std::optional<Stat> maybeLstat(const nix::CanonPath & path) override;
    void readFile(
        const nix::CanonPath & path,
        nix::Sink & sink,
        nix::fun<void(uint64_t)> sizeCallback = [](uint64_t) {}) override;
    DirEntries readDirectory(const nix::CanonPath & path) override;
    std::string readLink(const nix::CanonPath & path) override;

private:
    nix::ref<nix::SourceAccessor> inner;
    std::shared_ptr<AccessPolicy> policy;

    void require(const nix::CanonPath & path);
};

} // namespace nixciconfig
