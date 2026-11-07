#include "forge_accessor.hh"

#include <algorithm>
#include <nlohmann/json.hpp>

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

    auto url = contentsUrl(this->repo, relative);
    if (relative == "nix-ci.d" && this->repo.forge == Forge::GitLab) {
        url = this->repo.scheme + "://" + this->repo.host + "/api/v4/projects/"
            + percentEncode(this->repo.owner + "/" + this->repo.repo)
            + "/repository/tree?path=nix-ci.d&per_page=100&ref=" + percentEncode(this->repo.rev);
    }
    nix::FileTransferRequest request{nix::VerbatimURL(url)};
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

void ForgeAccessor::fetchGitHubFragments(const std::vector<std::string> & files) {
    using nlohmann::json;
    // Bound aliases per query; do not turn a failed batch into one REST call per file.
    constexpr size_t batchSize = 64;
    for (size_t start = 0; start < files.size(); start += batchSize) {
        auto end = std::min(start + batchSize, files.size());
        std::string query = "query { repository(owner:" + json(this->repo.owner).dump()
            + ",name:" + json(this->repo.repo).dump() + ") {";
        for (size_t i = start; i < end; ++i) {
            auto expression = this->repo.rev + ":nix-ci.d/" + files[i];
            query += " f" + std::to_string(i) + ":object(expression:" + json(expression).dump()
                + ") { __typename ... on Blob { text byteSize isBinary isTruncated } }";
        }
        query += " } }";
        auto endpoint = this->repo.host == "github.com"
            ? "https://api.github.com/graphql"
            : this->repo.scheme + "://" + this->repo.host + "/api/graphql";
        nix::FileTransferRequest request{nix::VerbatimURL(endpoint)};
        request.method = nix::HttpMethod::Post;
        request.mimeType = "application/json";
        auto body = json{{"query", query}}.dump();
        nix::StringSource source(body);
        request.data.emplace(source);
        if (this->token) {
            request.headers.push_back(authHeader(this->repo.forge, *this->token));
        }
        ++this->requestCount_;
        Cache batch;
        try {
            auto response = json::parse(nix::getFileTransfer()->download(request).data);
            if (response.contains("errors") && !response.at("errors").empty()) {
                this->fallback("GitHub fragment batch returned errors");
            }
            const auto & repository = response.at("data").at("repository");
            for (size_t i = start; i < end; ++i) {
                const auto & blob = repository.at("f" + std::to_string(i));
                if (blob.at("__typename") != "Blob" || blob.at("isBinary") != false
                    || blob.at("isTruncated") != false || !blob.at("text").is_string()
                    || !blob.at("byteSize").is_number_unsigned()) {
                    this->fallback("GitHub fragment batch did not return complete text blobs");
                }
                auto text = blob.at("text").get<std::string>();
                if (text.size() > 1024 * 1024 || blob.at("byteSize").get<size_t>() != text.size()) {
                    this->fallback("GitHub fragment batch returned oversized or incomplete contents");
                }
                batch.emplace("/nix-ci.d/" + files[i], ContentsFile{std::move(text)});
            }
        } catch (const json::exception &) {
            this->fallback("GitHub fragment batch returned an invalid response");
        } catch (const nix::FileTransferError &) {
            this->fallback("could not read GitHub fragment batch");
        }
        this->cache->insert(batch.begin(), batch.end());
    }
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
    if (std::holds_alternative<ContentsDirectory>(response)) {
        Stat stat;
        stat.type = tDirectory;
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
    if (path.abs() == "/nix-ci.d") {
        const auto & response = this->fetch(path);
        if (const auto * directory = std::get_if<ContentsDirectory>(&response)) {
            DirEntries entries;
            for (const auto & name : directory->files) {
                entries.emplace(name, tRegular);
                this->policy->allowed.insert("nix-ci.d/" + name);
            }
            if (this->repo.forge == Forge::GitHub) {
                std::vector<std::string> missing;
                for (const auto & name : directory->files) {
                    if (!this->cache->contains("/nix-ci.d/" + name)) {
                        missing.push_back(name);
                    }
                }
                this->fetchGitHubFragments(missing);
            }
            return entries;
        }
        throw nix::Error("nix-ci.d is not a directory");
    }
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
    auto prefix = path.abs().substr(1) + "/";
    for (const auto & file : this->policy->allowed) {
        if (file.starts_with(prefix)) {
            auto stat = this->inner->maybeLstat(path);
            if (stat && stat->type == tDirectory) {
                return stat;
            }
        }
    }
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
