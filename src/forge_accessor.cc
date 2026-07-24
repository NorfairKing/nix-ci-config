#include "forge_accessor.hh"

#include <nix/store/filetransfer.hh>
#include <nix/util/serialise.hh>

namespace nixciconfig {

bool AccessPolicy::permits(const std::string & relative) {
    if (this->allowed.find(relative) != this->allowed.end()) {
        return true;
    }
    // The first one is what the configuration did wrong; anything read while
    // unwinding from it is a consequence.
    if (!this->violation) {
        this->violation = relative;
    }
    return false;
}

ForgeAccessor::ForgeAccessor(
    ForgeRepo repo,
    std::optional<std::string> token,
    std::shared_ptr<AccessPolicy> policy,
    std::shared_ptr<Cache> cache)
    : repo(std::move(repo)),
      token(std::move(token)),
      policy(std::move(policy)),
      cache(cache ? std::move(cache) : std::make_shared<Cache>()) {
    this->displayPrefix = "«" + this->repo.host + ":" + this->repo.owner + "/" + this->repo.repo + "»";
}

void ForgeAccessor::noteFallback(const std::string & reason) {
    if (!this->fallbackNeeded_) {
        this->fallbackNeeded_ = true;
        this->fallbackReason_ = reason;
    }
}

void ForgeAccessor::fallback(const std::string & reason) {
    // The first reason is the one that decided it; anything raised while
    // unwinding is a consequence of this one.
    if (!this->fallbackNeeded_) {
        this->fallbackNeeded_ = true;
        this->fallbackReason_ = reason;
    }
    throw FallbackNeeded(reason);
}

const ContentsResponse & ForgeAccessor::fetch(const nix::CanonPath & path) {
    auto key = path.abs();
    auto cached = this->cache->find(key);
    if (cached != this->cache->end()) {
        return cached->second;
    }

    // CanonPath has already resolved away any `..`, so a path that would leave
    // the repository cannot reach the request. Its leading slash is not part of
    // a path within the repository.
    auto relative = key.substr(1);

    if (!this->policy->permits(relative)) {
        throw NotSelfContained(relative);
    }

    nix::FileTransferRequest request{nix::VerbatimURL(contentsUrl(this->repo, relative))};
    request.headers.push_back({"Accept", "application/json"});
    if (this->token) {
        request.headers.push_back(authHeader(this->repo.forge, *this->token));
    }

    ++this->requestCount_;

    std::string body;
    try {
        body = nix::getFileTransfer()->download(request).data;
    } catch (const nix::FileTransferError & e) {
        if (e.error == nix::FileTransfer::NotFound) {
            return this->cache->emplace(key, ContentsAbsent{}).first->second;
        }
        // Anything else (an expired token, a rate limit, a network fault) says
        // nothing about whether the file is there, so it must not be reported
        // as absent.
        this->fallback("could not read " + relative + ": " + std::string(e.what()));
    }

    auto response = parseContentsResponse(this->repo.forge, body);
    if (const auto * unservable = std::get_if<ContentsUnservable>(&response)) {
        this->fallback(relative + ": " + unservable->reason);
    }
    return this->cache->emplace(key, std::move(response)).first->second;
}

std::optional<nix::SourceAccessor::Stat> ForgeAccessor::maybeLstat(const nix::CanonPath & path) {
    // The repository root is a directory whatever else is in it, and every
    // path this serves is resolved through it.
    if (path.isRoot()) {
        Stat stat;
        stat.type = tDirectory;
        return stat;
    }

    const auto & response = this->fetch(path);
    if (const auto * file = std::get_if<ContentsFile>(&response)) {
        Stat stat;
        stat.type = tRegular;
        stat.fileSize = file->contents.size();
        return stat;
    }
    return std::nullopt;
}

void ForgeAccessor::readFile(const nix::CanonPath & path, nix::Sink & sink, nix::fun<void(uint64_t)> sizeCallback) {
    const auto & response = this->fetch(path);
    const auto * file = std::get_if<ContentsFile>(&response);
    if (file == nullptr) {
        throw nix::Error("file '%s' does not exist", this->showPath(path));
    }
    sizeCallback(file->contents.size());
    sink(file->contents);
}

nix::SourceAccessor::DirEntries ForgeAccessor::readDirectory(const nix::CanonPath & path) {
    // Nothing this serves is a directory, so whatever wants a listing wants
    // more of the repository than a configuration may read.
    this->policy->permits(path.abs());
    throw NotSelfContained(path.abs());
}

std::string ForgeAccessor::readLink(const nix::CanonPath & path) {
    this->policy->permits(path.abs());
    throw NotSelfContained(path.abs());
}

RestrictedAccessor::RestrictedAccessor(nix::ref<nix::SourceAccessor> inner, std::shared_ptr<AccessPolicy> policy)
    : inner(std::move(inner)), policy(std::move(policy)) {
    this->displayPrefix = this->inner->displayPrefix;
    this->displaySuffix = this->inner->displaySuffix;
}

void RestrictedAccessor::require(const nix::CanonPath & path) {
    // Every path this serves is resolved through the root, which is a
    // directory whatever the configuration reads.
    if (path.isRoot()) {
        return;
    }
    auto relative = path.abs().substr(1);
    if (!this->policy->permits(relative)) {
        throw NotSelfContained(relative);
    }
}

std::optional<nix::SourceAccessor::Stat> RestrictedAccessor::maybeLstat(const nix::CanonPath & path) {
    this->require(path);
    return this->inner->maybeLstat(path);
}

void RestrictedAccessor::readFile(
    const nix::CanonPath & path, nix::Sink & sink, nix::fun<void(uint64_t)> sizeCallback) {
    this->require(path);
    this->inner->readFile(path, sink, sizeCallback);
}

nix::SourceAccessor::DirEntries RestrictedAccessor::readDirectory(const nix::CanonPath & path) {
    this->require(path);
    return this->inner->readDirectory(path);
}

std::string RestrictedAccessor::readLink(const nix::CanonPath & path) {
    this->require(path);
    return this->inner->readLink(path);
}

} // namespace nixciconfig
