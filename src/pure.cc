#include "pure.hh"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace nixciconfig {

const char * configFileName = "nix-ci.nix";
const char * requiredFileName = "flake.nix";
const char * fallbackAttrName = "nix-ci";

namespace {

const char * base64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool isUnreservedUrlChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~';
}

// Percent-encode every segment of a path but leave the separators alone, for
// the forges that take the path as part of the URL path.
std::string percentEncodeKeepingSlashes(const std::string & s) {
    std::string out;
    for (char c : s) {
        if (c == '/') {
            out += c;
        } else if (isUnreservedUrlChar(c)) {
            out += c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            out += buf;
        }
    }
    return out;
}

// The body of a contents response, once it is known to be a JSON object.
ContentsResponse parseContentsObject(const nlohmann::json & object) {
    // GitLab's endpoint answers only for files, so it reports no type.
    // Where a type is reported, anything but a file is not a configuration.
    auto typeIt = object.find("type");
    if (typeIt != object.end() && typeIt->is_string() && typeIt->get<std::string>() != "file") {
        return ContentsUnservable{"not a regular file: " + typeIt->get<std::string>()};
    }

    auto encodingIt = object.find("encoding");
    if (encodingIt == object.end() || !encodingIt->is_string()) {
        return ContentsUnservable{"contents response carried no encoding"};
    }
    auto encoding = encodingIt->get<std::string>();
    if (encoding != "base64") {
        // GitHub declines to inline a file over 1 MiB and says so with an
        // encoding of "none". A configuration that large is not what this is
        // for, and the Nix strategy reads it without trouble.
        return ContentsUnservable{"contents were not inlined (encoding: " + encoding + ")"};
    }

    auto contentIt = object.find("content");
    if (contentIt == object.end() || !contentIt->is_string()) {
        return ContentsUnservable{"contents response carried no content"};
    }

    auto decoded = base64Decode(contentIt->get<std::string>());
    if (!decoded) {
        return ContentsUnservable{"contents were not decodable base64"};
    }
    return ContentsFile{*decoded};
}

} // namespace

std::string usage() {
    return "Usage: nix-ci-config --flake <ref> [--forge-type <forge>] [--impure]\n"
           "\n"
           "Read a repository's NixCI configuration, without cloning it.\n"
           "\n"
           "  --flake <ref>            Flake reference to read the configuration from (required)\n"
           "  --forge-type <forge>     github, gitlab or forgejo; needed only for an instance\n"
           "                           whose host name does not say which it runs\n"
           "  --impure                 Evaluate the configuration file with access to the\n"
           "                           host's environment and files (off by default)\n"
           "  --help                   Show this and exit\n"
           "\n"
           "Writes one JSON object to stdout, carrying a \"result\" of success,\n"
           "missing-flake, not-self-contained, eval-error or fetch-error. The exit code\n"
           "is zero unless the tool itself failed.\n";
}

std::variant<Args, ArgsHelp, ArgsError> parseArgs(const std::vector<std::string> & argv) {
    Args args;
    bool sawFlake = false;

    for (const auto & arg : argv) {
        if (arg == "--help" || arg == "-h") {
            return ArgsHelp{};
        }
    }

    for (size_t i = 0; i < argv.size(); ++i) {
        const auto & arg = argv[i];
        auto next = [&](const char * what) -> std::optional<std::string> {
            if (i + 1 >= argv.size()) {
                return std::nullopt;
            }
            (void) what;
            return argv[++i];
        };

        if (arg == "--flake") {
            auto value = next("--flake");
            if (!value) {
                return ArgsError{"--flake needs a flake reference"};
            }
            args.flakeRef = *value;
            sawFlake = true;
        } else if (arg == "--forge-type") {
            auto value = next("--forge-type");
            if (!value) {
                return ArgsError{"--forge-type needs a forge"};
            }
            args.forgeType = parseForge(*value);
            if (!args.forgeType) {
                return ArgsError{"unknown forge: " + *value};
            }
        } else if (arg == "--impure") {
            args.impure = true;
        } else {
            return ArgsError{"unknown argument: " + arg};
        }
    }

    if (!sawFlake) {
        return ArgsError{"--flake is required"};
    }
    if (args.flakeRef.empty()) {
        return ArgsError{"--flake needs a non-empty flake reference"};
    }

    return args;
}

std::optional<Forge> parseForge(const std::string & name) {
    if (name == "github") {
        return Forge::GitHub;
    }
    if (name == "gitlab") {
        return Forge::GitLab;
    }
    // Codeberg runs Forgejo, and so does every other instance of it this can
    // read; Gitea, which Forgejo forked from, serves the same endpoint.
    if (name == "forgejo") {
        return Forge::Forgejo;
    }
    return std::nullopt;
}

std::optional<Forge> forgeForHost(const std::string & host) {
    if (host == "github.com") {
        return Forge::GitHub;
    }
    if (host == "gitlab.com") {
        return Forge::GitLab;
    }
    if (host == "codeberg.org") {
        return Forge::Forgejo;
    }
    return std::nullopt;
}

std::string contentsUrl(const ForgeRepo & repo, const std::string & path) {
    auto origin = repo.scheme + "://" + repo.host;
    switch (repo.forge) {
    case Forge::GitHub:
        // The contents API rather than raw.githubusercontent.com, because the
        // latter takes no credentials and so cannot read a private repository.
        //
        // github.com serves it from its own hostname, where a self-hosted
        // instance serves it from a path under the one the repositories are
        // on.
        return (repo.host == "github.com" ? repo.scheme + "://api.github.com" : origin + "/api/v3") + "/repos/" +
               percentEncode(repo.owner) + "/" + percentEncode(repo.repo) + "/contents/" +
               percentEncodeKeepingSlashes(path) + "?ref=" + percentEncode(repo.rev);
    case Forge::GitLab:
        return origin + "/api/v4/projects/" + percentEncode(repo.owner + "/" + repo.repo) + "/repository/files/" +
               percentEncode(path) + "?ref=" + percentEncode(repo.rev);
    case Forge::Forgejo:
        return origin + "/api/v1/repos/" + percentEncode(repo.owner) + "/" + percentEncode(repo.repo) +
               "/contents/" + percentEncodeKeepingSlashes(path) + "?ref=" + percentEncode(repo.rev);
    }
    return "";
}

std::pair<std::string, std::string> authHeader(Forge forge, const std::string & token) {
    switch (forge) {
    case Forge::GitHub:
        return {"Authorization", "Bearer " + token};
    case Forge::GitLab: {
        // GitLab takes an OAuth2 token and a personal access token in
        // different headers, and rejects each in the other's. Nix says which
        // is which by prefixing the token with its type, so a token that came
        // from Nix's own `access-tokens` is read the same way here.
        //
        // An untyped token is treated as OAuth2, because that is what NixCI
        // has: it clones GitLab over https as the `oauth2` user.
        auto separator = token.find(':');
        if (separator != std::string::npos) {
            auto type = token.substr(0, separator);
            auto value = token.substr(separator + 1);
            if (type == "PAT") {
                return {"Private-token", value};
            }
            if (type == "OAuth2") {
                return {"Authorization", "Bearer " + value};
            }
        }
        return {"Authorization", "Bearer " + token};
    }
    case Forge::Forgejo:
        return {"Authorization", "token " + token};
    }
    return {"", ""};
}

std::optional<std::string> tokenFor(
    const std::map<std::string, std::string> & accessTokens, const ForgeRepo & repo) {
    for (const auto & key : {repo.host + "/" + repo.owner + "/" + repo.repo, repo.host + "/" + repo.owner, repo.host}) {
        auto it = accessTokens.find(key);
        if (it != accessTokens.end()) {
            return it->second;
        }
    }
    return std::nullopt;
}

ContentsResponse parseContentsResponse(Forge forge, const std::string & body) {
    (void) forge;
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(body);
    } catch (const nlohmann::json::exception & e) {
        return ContentsUnservable{std::string("contents response was not JSON: ") + e.what()};
    }

    // A directory comes back as an array of its entries, which is not
    // something a configuration file is.
    if (parsed.is_array()) {
        return ContentsUnservable{"not a regular file: directory"};
    }
    if (!parsed.is_object()) {
        return ContentsUnservable{"contents response was neither an object nor an array"};
    }
    return parseContentsObject(parsed);
}

std::string percentEncode(const std::string & s) {
    std::string out;
    for (char c : s) {
        if (isUnreservedUrlChar(c)) {
            out += c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            out += buf;
        }
    }
    return out;
}

std::optional<std::string> base64Decode(const std::string & s) {
    // Built once, and only from the alphabet above, so every byte that is not
    // in it decodes as invalid rather than as some other character's value.
    static const auto decodeTable = [] {
        std::array<int8_t, 256> table{};
        table.fill(-1);
        for (int8_t i = 0; i < 64; ++i) {
            table[static_cast<unsigned char>(base64Chars[i])] = i;
        }
        return table;
    }();

    std::string out;
    uint32_t bits = 0;
    int bitCount = 0;
    size_t padding = 0;

    for (char c : s) {
        // The forges wrap the base64 at a fixed width.
        if (c == '\n' || c == '\r') {
            continue;
        }
        if (c == '=') {
            ++padding;
            continue;
        }
        if (padding > 0) {
            return std::nullopt;
        }
        auto digit = decodeTable[static_cast<unsigned char>(c)];
        if (digit < 0) {
            return std::nullopt;
        }
        bits = (bits << 6) | static_cast<uint32_t>(digit);
        bitCount += 6;
        if (bitCount >= 8) {
            bitCount -= 8;
            out += static_cast<char>((bits >> bitCount) & 0xff);
        }
    }

    // Whatever is left over must be padding bits, which are zero.
    if (bitCount >= 6 || (bits & ((1u << bitCount) - 1)) != 0) {
        return std::nullopt;
    }

    return out;
}

std::string renderResult(const Result & result) {
    // ordered_json rather than json so keys keep the order they are inserted
    // in rather than being sorted: the "result" is inserted first and so reads
    // first, where the discriminator is worth seeing first.
    nlohmann::ordered_json object;
    std::visit(
        [&object](const auto & r) {
            using T = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<T, SuccessResult>) {
                object["result"] = "success";
                object["config"] =
                    r.configJson ? nlohmann::ordered_json::parse(*r.configJson) : nlohmann::ordered_json(nullptr);
            } else if constexpr (std::is_same_v<T, MissingFlakeResult>) {
                object["result"] = "missing-flake";
            } else if constexpr (std::is_same_v<T, NotSelfContainedResult>) {
                object["result"] = "not-self-contained";
                object["path"] = r.path;
            } else if constexpr (std::is_same_v<T, EvalErrorResult>) {
                object["result"] = "eval-error";
                object["message"] = r.message;
            } else if constexpr (std::is_same_v<T, FetchErrorResult>) {
                object["result"] = "fetch-error";
                object["message"] = r.message;
            }
        },
        result);
    return object.dump();
}

} // namespace nixciconfig
