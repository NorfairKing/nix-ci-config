// Dependency-free unit tests for the pure helpers. Exits non-zero on the first
// failed assertion so `meson test` reports it.

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "pure.hh"

namespace {

int failures = 0;

void expectEq(const std::string & what, const std::string & got, const std::string & want) {
    if (got != want) {
        failures++;
        fprintf(stderr, "FAIL %s:\n  got  \"%s\"\n  want \"%s\"\n", what.c_str(), got.c_str(), want.c_str());
    }
}

void expectBool(const std::string & what, bool got, bool want) {
    if (got != want) {
        failures++;
        fprintf(stderr, "FAIL %s: got %s, want %s\n", what.c_str(), got ? "true" : "false", want ? "true" : "false");
    }
}

bool rejects(const std::vector<std::string> & arguments) {
    return std::holds_alternative<nixciconfig::ArgsError>(nixciconfig::parseArgs(arguments));
}

bool asksForHelp(const std::vector<std::string> & arguments) {
    return std::holds_alternative<nixciconfig::ArgsHelp>(nixciconfig::parseArgs(arguments));
}

nixciconfig::Args accept(const std::vector<std::string> & arguments) {
    auto parsed = nixciconfig::parseArgs(arguments);
    if (auto * args = std::get_if<nixciconfig::Args>(&parsed)) {
        return *args;
    }
    failures++;
    fprintf(stderr, "FAIL parsing was rejected: %s\n", std::get<nixciconfig::ArgsError>(parsed).message.c_str());
    return nixciconfig::Args{};
}

void testArgs() {
    expectEq("flake reference", accept({"--flake", "github:o/r"}).flakeRef, "github:o/r");

    expectBool("no forge type by default", accept({"--flake", "."}).forgeType.has_value(), false);
    expectBool(
        "github forge type",
        accept({"--flake", ".", "--forge-type", "github"}).forgeType == nixciconfig::Forge::GitHub,
        true);
    expectBool(
        "gitlab forge type",
        accept({"--flake", ".", "--forge-type", "gitlab"}).forgeType == nixciconfig::Forge::GitLab,
        true);
    expectBool(
        "forgejo forge type",
        accept({"--flake", ".", "--forge-type", "forgejo"}).forgeType == nixciconfig::Forge::Forgejo,
        true);

    // Pure by default, so a configuration cannot reach the host unless the
    // caller asks for it.
    expectBool("pure by default", accept({"--flake", "."}).impure, false);
    expectBool("impure when asked", accept({"--flake", ".", "--impure"}).impure, true);

    expectBool("no arguments", rejects({}), true);
    expectBool("unknown forge type", rejects({"--flake", ".", "--forge-type", "cvs"}), true);
    expectBool("forge type without a value", rejects({"--flake", ".", "--forge-type"}), true);
    expectBool("flake without a value", rejects({"--flake"}), true);
    expectBool("empty flake reference", rejects({"--flake", ""}), true);
    // How the sources are read is not a choice a caller makes, so naming one
    // is naming an option that does not exist.
    expectBool("a fetch strategy", rejects({"--flake", ".", "--fetch-strategy", "nix"}), true);
    expectBool("unknown argument", rejects({"--flake", ".", "--turbo"}), true);
    // A flake reference is the one thing this cannot guess, so a bare one is
    // not silently taken as the positional argument it does not have.
    expectBool("positional flake reference", rejects({"github:o/r"}), true);

    // Asking for the usage is answered whatever else was passed, including
    // nothing, so a caller that got the arguments wrong is told how rather
    // than told off.
    expectBool("--help", asksForHelp({"--help"}), true);
    expectBool("-h", asksForHelp({"-h"}), true);
    expectBool("--help beside other arguments", asksForHelp({"--flake", ".", "--help"}), true);
    expectBool("--help beside a bad argument", asksForHelp({"--turbo", "--help"}), true);
    expectBool("no help asked for", asksForHelp({"--flake", "."}), false);

    // Every option the usage documents is one this accepts, or the usage is a
    // list of ways to be told off.
    for (const auto & documented : {"--flake", "--forge-type", "--impure", "--help"}) {
        expectBool(
            std::string("the usage documents ") + documented,
            nixciconfig::usage().find(documented) != std::string::npos,
            true);
    }
}

void testForgeForHost() {
    expectBool("github.com", nixciconfig::forgeForHost("github.com") == nixciconfig::Forge::GitHub, true);
    expectBool("gitlab.com", nixciconfig::forgeForHost("gitlab.com") == nixciconfig::Forge::GitLab, true);
    expectBool("codeberg.org", nixciconfig::forgeForHost("codeberg.org") == nixciconfig::Forge::Forgejo, true);
    // A self-hosted instance is served by the Nix strategy rather than by a
    // guess at which forge it runs.
    expectBool("a self-hosted host", nixciconfig::forgeForHost("git.example.com").has_value(), false);
    expectBool("an empty host", nixciconfig::forgeForHost("").has_value(), false);
}

void testContentsUrl() {
    nixciconfig::ForgeRepo gitHub{nixciconfig::Forge::GitHub, "github.com", "NorfairKing", "nix-ci", "deadbeef"};
    expectEq(
        "github contents url",
        nixciconfig::contentsUrl(gitHub, "nix-ci.nix"),
        "https://api.github.com/repos/NorfairKing/nix-ci/contents/nix-ci.nix?ref=deadbeef");
    // The separators of a nested path stay separators, so the request names
    // the file rather than one whose name contains slashes.
    expectEq(
        "github nested path",
        nixciconfig::contentsUrl(gitHub, "ci/deps.nix"),
        "https://api.github.com/repos/NorfairKing/nix-ci/contents/ci/deps.nix?ref=deadbeef");

    nixciconfig::ForgeRepo gitLab{nixciconfig::Forge::GitLab, "gitlab.com", "group", "project", "deadbeef"};
    // GitLab takes the project and the path as single encoded components, so
    // both of their separators are escaped.
    expectEq(
        "gitlab contents url",
        nixciconfig::contentsUrl(gitLab, "ci/deps.nix"),
        "https://gitlab.com/api/v4/projects/group%2Fproject/repository/files/ci%2Fdeps.nix?ref=deadbeef");

    nixciconfig::ForgeRepo gitea{nixciconfig::Forge::Forgejo, "codeberg.org", "owner", "repo", "deadbeef"};
    expectEq(
        "gitea contents url",
        nixciconfig::contentsUrl(gitea, "nix-ci.nix"),
        "https://codeberg.org/api/v1/repos/owner/repo/contents/nix-ci.nix?ref=deadbeef");

    nixciconfig::ForgeRepo odd{nixciconfig::Forge::GitHub, "github.com", "o w", "r+p", "deadbeef"};
    expectEq(
        "an owner and repository needing escaping",
        nixciconfig::contentsUrl(odd, "nix-ci.nix"),
        "https://api.github.com/repos/o%20w/r%2Bp/contents/nix-ci.nix?ref=deadbeef");

    // A self-hosted GitHub serves its API from a path on the same host, where
    // github.com serves it from a hostname of its own.
    nixciconfig::ForgeRepo enterprise{nixciconfig::Forge::GitHub, "git.example.com", "owner", "repo", "deadbeef"};
    expectEq(
        "self-hosted github contents url",
        nixciconfig::contentsUrl(enterprise, "nix-ci.nix"),
        "https://git.example.com/api/v3/repos/owner/repo/contents/nix-ci.nix?ref=deadbeef");

    nixciconfig::ForgeRepo selfHostedLab{nixciconfig::Forge::GitLab, "git.example.com", "group", "project", "deadbeef"};
    expectEq(
        "self-hosted gitlab contents url",
        nixciconfig::contentsUrl(selfHostedLab, "nix-ci.nix"),
        "https://git.example.com/api/v4/projects/group%2Fproject/repository/files/nix-ci.nix?ref=deadbeef");

    nixciconfig::ForgeRepo selfHostedForgejo{
        nixciconfig::Forge::Forgejo, "git.example.com", "owner", "repo", "deadbeef"};
    expectEq(
        "self-hosted forgejo contents url",
        nixciconfig::contentsUrl(selfHostedForgejo, "nix-ci.nix"),
        "https://git.example.com/api/v1/repos/owner/repo/contents/nix-ci.nix?ref=deadbeef");

    // A GitLab namespace nests, and the projects endpoint takes the whole one
    // as a single encoded component.
    nixciconfig::ForgeRepo subgroup{nixciconfig::Forge::GitLab, "gitlab.com", "group/sub", "project", "deadbeef"};
    expectEq(
        "gitlab subgroup contents url",
        nixciconfig::contentsUrl(subgroup, "nix-ci.nix"),
        "https://gitlab.com/api/v4/projects/group%2Fsub%2Fproject/repository/files/nix-ci.nix?ref=deadbeef");
}

void testParseForge() {
    expectBool("github", nixciconfig::parseForge("github") == nixciconfig::Forge::GitHub, true);
    expectBool("gitlab", nixciconfig::parseForge("gitlab") == nixciconfig::Forge::GitLab, true);
    expectBool("forgejo", nixciconfig::parseForge("forgejo") == nixciconfig::Forge::Forgejo, true);
    // Named for the software, not for the instance that made it worth naming.
    expectBool("codeberg", nixciconfig::parseForge("codeberg").has_value(), false);
    expectBool("an unknown forge", nixciconfig::parseForge("cvs").has_value(), false);
    expectBool("an empty name", nixciconfig::parseForge("").has_value(), false);
}

void testAuthHeader() {
    auto gitHub = nixciconfig::authHeader(nixciconfig::Forge::GitHub, "tok");
    expectEq("github auth header name", gitHub.first, "Authorization");
    expectEq("github auth header value", gitHub.second, "Bearer tok");

    // GitLab rejects an OAuth2 token sent as a personal access token and the
    // other way round, so which header it goes in decides whether a private
    // repository can be read at all. NixCI's tokens are OAuth2, which is what
    // an untyped one is taken to be.
    auto gitLab = nixciconfig::authHeader(nixciconfig::Forge::GitLab, "tok");
    expectEq("gitlab auth header name", gitLab.first, "Authorization");
    expectEq("gitlab auth header value", gitLab.second, "Bearer tok");

    // Nix says which kind a token is by prefixing it with the type, so a token
    // that came from its `access-tokens` is read the same way here.
    auto gitLabOAuth = nixciconfig::authHeader(nixciconfig::Forge::GitLab, "OAuth2:tok");
    expectEq("gitlab oauth2 header name", gitLabOAuth.first, "Authorization");
    expectEq("gitlab oauth2 header value", gitLabOAuth.second, "Bearer tok");

    auto gitLabPat = nixciconfig::authHeader(nixciconfig::Forge::GitLab, "PAT:tok");
    expectEq("gitlab pat header name", gitLabPat.first, "Private-token");
    expectEq("gitlab pat header value", gitLabPat.second, "tok");

    // A token with a colon in it that names no type this knows is a token,
    // not a type and a token.
    auto gitLabOdd = nixciconfig::authHeader(nixciconfig::Forge::GitLab, "glpat-a:b");
    expectEq("gitlab untyped header name", gitLabOdd.first, "Authorization");
    expectEq("gitlab untyped header value", gitLabOdd.second, "Bearer glpat-a:b");

    auto gitea = nixciconfig::authHeader(nixciconfig::Forge::Forgejo, "tok");
    expectEq("gitea auth header name", gitea.first, "Authorization");
    expectEq("gitea auth header value", gitea.second, "token tok");
}

void testTokenFor() {
    nixciconfig::ForgeRepo repo{nixciconfig::Forge::GitHub, "github.com", "owner", "repo", "deadbeef"};

    expectEq(
        "a token for the host", nixciconfig::tokenFor({{"github.com", "host"}}, repo).value_or("none"), "host");
    expectEq(
        "a token for the owner",
        nixciconfig::tokenFor({{"github.com/owner", "owner"}}, repo).value_or("none"),
        "owner");
    expectEq(
        "a token for the repository",
        nixciconfig::tokenFor({{"github.com/owner/repo", "repo"}}, repo).value_or("none"),
        "repo");
    // The most specific entry wins, so a token scoped to one repository is not
    // shadowed by a broader one for the same host.
    expectEq(
        "the most specific token",
        nixciconfig::tokenFor(
            {{"github.com", "host"}, {"github.com/owner", "owner"}, {"github.com/owner/repo", "repo"}}, repo)
            .value_or("none"),
        "repo");
    expectBool("no token at all", nixciconfig::tokenFor({}, repo).has_value(), false);
    expectBool(
        "a token for another host", nixciconfig::tokenFor({{"gitlab.com", "other"}}, repo).has_value(), false);
    // A prefix that is not a path component boundary must not match, or a
    // token for one owner would be sent to another whose name starts the same.
    expectBool(
        "a token for a similarly named owner",
        nixciconfig::tokenFor({{"github.com/owner2", "other"}}, repo).has_value(),
        false);
}

// The contents of a file, or a description of why it could not be served.
std::string describeContents(nixciconfig::Forge forge, const std::string & body) {
    auto response = nixciconfig::parseContentsResponse(forge, body);
    if (auto * file = std::get_if<nixciconfig::ContentsFile>(&response)) {
        return "file:" + file->contents;
    }
    if (std::holds_alternative<nixciconfig::ContentsAbsent>(response)) {
        return "absent";
    }
    return "unservable";
}

void testParseContentsResponse() {
    // "eyB9" is "{ }".
    expectEq(
        "a github file",
        describeContents(
            nixciconfig::Forge::GitHub, R"({"type":"file","encoding":"base64","content":"eyB9"})"),
        "file:{ }");
    // The forges wrap the base64 at a fixed width, so a newline in it is not a
    // reason to give up on the file.
    expectEq(
        "base64 wrapped over lines",
        describeContents(
            nixciconfig::Forge::GitHub, "{\"type\":\"file\",\"encoding\":\"base64\",\"content\":\"eyB\\n9\"}"),
        "file:{ }");
    // GitLab's endpoint answers only for files, so it reports no type.
    expectEq(
        "a gitlab file without a type",
        describeContents(nixciconfig::Forge::GitLab, R"({"encoding":"base64","content":"eyB9"})"),
        "file:{ }");

    // A directory comes back as an array of its entries, and a configuration
    // file is not one.
    expectEq(
        "a directory as an array",
        describeContents(nixciconfig::Forge::GitHub, R"([{"name":"a"}])"),
        "unservable");
    expectEq(
        "a directory by its type",
        describeContents(nixciconfig::Forge::Forgejo, R"({"type":"dir"})"),
        "unservable");
    expectEq(
        "a symlink",
        describeContents(
            nixciconfig::Forge::Forgejo, R"({"type":"symlink","encoding":"base64","content":"eyB9"})"),
        "unservable");
    // GitHub declines to inline a file over 1 MiB.
    expectEq(
        "contents that were not inlined",
        describeContents(nixciconfig::Forge::GitHub, R"({"type":"file","encoding":"none","content":""})"),
        "unservable");
    expectEq(
        "contents that were not base64",
        describeContents(nixciconfig::Forge::GitHub, R"({"type":"file","encoding":"base64","content":"!!!"})"),
        "unservable");
    expectEq("a body that was not json", describeContents(nixciconfig::Forge::GitHub, "not json"), "unservable");
    expectEq("a body that was a string", describeContents(nixciconfig::Forge::GitHub, "\"hello\""), "unservable");
}

void testBase64Decode() {
    expectEq("empty", nixciconfig::base64Decode("").value_or("<invalid>"), "");
    expectEq("one padded byte", nixciconfig::base64Decode("YQ==").value_or("<invalid>"), "a");
    expectEq("two padded bytes", nixciconfig::base64Decode("YWI=").value_or("<invalid>"), "ab");
    expectEq("three unpadded bytes", nixciconfig::base64Decode("YWJj").value_or("<invalid>"), "abc");
    expectEq(
        "a longer string",
        nixciconfig::base64Decode("eyBlbmFibGUgPSB0cnVlOyB9").value_or("<invalid>"),
        "{ enable = true; }");
    // High bytes must survive, since a configuration file may hold any UTF-8.
    expectEq("high bytes", nixciconfig::base64Decode("w6k=").value_or("<invalid>"), "\xc3\xa9");

    expectBool("a character outside the alphabet", nixciconfig::base64Decode("!!!!").has_value(), false);
    expectBool("content after the padding", nixciconfig::base64Decode("YQ==YQ==").has_value(), false);
    // A single leftover character cannot be a whole byte, so it is not a
    // truncation to decode as far as it goes.
    expectBool("a truncated group", nixciconfig::base64Decode("YWJjY").has_value(), false);
}

void testRenderResult() {
    // "result" comes first, whatever the other keys, so the discriminator is
    // the first thing a reader sees.
    expectEq(
        "a configuration",
        nixciconfig::renderResult(nixciconfig::SuccessResult{R"({"enable":true})"}),
        R"({"result":"success","config":{"enable":true}})");
    // A repository with no configuration: config is null, which means the
    // defaults apply. Distinct from an empty configuration, which is {}.
    expectEq(
        "no configuration",
        nixciconfig::renderResult(nixciconfig::SuccessResult{std::nullopt}),
        R"({"result":"success","config":null})");
    expectEq(
        "an empty configuration is not the same as none",
        nixciconfig::renderResult(nixciconfig::SuccessResult{"{}"}),
        R"({"result":"success","config":{}})");
    expectEq(
        "no flake",
        nixciconfig::renderResult(nixciconfig::MissingFlakeResult{}),
        R"({"result":"missing-flake"})");
    expectEq(
        "not self-contained",
        nixciconfig::renderResult(nixciconfig::NotSelfContainedResult{"systems.nix"}),
        R"({"result":"not-self-contained","path":"systems.nix"})");
    expectEq(
        "eval error",
        nixciconfig::renderResult(nixciconfig::EvalErrorResult{"boom"}),
        R"({"result":"eval-error","message":"boom"})");
    expectEq(
        "fetch error",
        nixciconfig::renderResult(nixciconfig::FetchErrorResult{"boom"}),
        R"({"result":"fetch-error","message":"boom"})");
}

} // namespace

int main() {
    testArgs();
    testForgeForHost();
    testParseForge();
    testContentsUrl();
    testAuthHeader();
    testTokenFor();
    testParseContentsResponse();
    testBase64Decode();
    testRenderResult();

    if (failures > 0) {
        fprintf(stderr, "%d assertion(s) failed\n", failures);
        return 1;
    }
    printf("all pure tests passed\n");
    return 0;
}
