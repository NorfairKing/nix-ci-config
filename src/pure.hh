#pragma once

// Everything here is free of the Nix libraries, so it can be unit-tested on
// its own: argument parsing, the forge file APIs, and the output lines.

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace nixciconfig {

// The file API of a forge, as far as reading one file at one revision goes.
// All three expose a "contents" endpoint that answers with base64 in JSON, so
// one response parser serves them and only the URL and the credential header
// differ.
enum class Forge {
    GitHub,
    GitLab,
    Forgejo,
};

struct Args {
    std::string flakeRef;
    // Which forge serves the host, for an instance this cannot recognise from
    // its host name alone. Nothing about correctness turns on it: it is what
    // lets a self-hosted instance take the fast path rather than the slow one.
    std::optional<Forge> forgeType;
    // Evaluate the configuration file impurely, which lets it read the host's
    // environment and files (`builtins.getEnv`, `builtins.currentSystem`, an
    // absolute path). Off by default so that an untrusted configuration cannot
    // reach outside the repository; asked for where a configuration is known to
    // need it. Only the configuration file is affected: a flake output is
    // always evaluated purely, as it is under `nix eval`.
    bool impure = false;
};

struct ArgsError {
    std::string message;
};

// The arguments asked for the usage rather than for a run.
struct ArgsHelp {};

std::variant<Args, ArgsHelp, ArgsError> parseArgs(const std::vector<std::string> & argv);

std::string usage();

// A repository on a forge, at one revision.
struct ForgeRepo {
    Forge forge;
    std::string host;
    std::string owner;
    std::string repo;
    std::string rev;
    // The scheme the repository itself is named with, so that reading a file
    // out of it is no more exposed than cloning it would have been. Every
    // forge worth the name is https; a test server on loopback is not.
    std::string scheme = "https";
};

// Which forge, if any, serves a file API for this host.
//
// Only the public instances are recognised, because guessing that an unknown
// host speaks some forge's API would turn a wrong guess into a failed config
// job rather than a slow one. A self-hosted instance is told rather than
// guessed, with @--forge-type@; without that it is served by the Nix strategy,
// which is correct everywhere and merely slower.
std::optional<Forge> forgeForHost(const std::string & host);

// The forge a @--forge-type@ value names, if it names one.
std::optional<Forge> parseForge(const std::string & name);

// The contents endpoint for one path in a repository at a revision.
std::string contentsUrl(const ForgeRepo & repo, const std::string & path);

// The header that authenticates a request, given the access token for the
// host. Each forge spells this differently.
std::pair<std::string, std::string> authHeader(Forge forge, const std::string & token);

// The access token for a repository, from Nix's `access-tokens` setting.
//
// Nix matches a token against a host optionally followed by a path prefix, so
// the most specific entry wins: a token for one repository beats one for its
// owner, which beats one for the whole host.
std::optional<std::string> tokenFor(const std::map<std::string, std::string> & accessTokens, const ForgeRepo & repo);

// What a contents response says a path is.
struct ContentsFile {
    std::string contents;
};

// The path is there but is neither of those, or the response withheld the
// contents (GitHub declines to inline a file over 1 MiB).
struct ContentsUnservable {
    std::string reason;
};

struct ContentsAbsent {};

using ContentsResponse = std::variant<ContentsFile, ContentsUnservable, ContentsAbsent>;

// Interpret a 200 response body from a contents endpoint.
ContentsResponse parseContentsResponse(Forge forge, const std::string & body);

std::string percentEncode(const std::string & s);

// Decode base64, ignoring the newlines the forges wrap it in. Returns nullopt
// on anything that is not base64.
std::optional<std::string> base64Decode(const std::string & s);

// What a run concluded, written as the single JSON object on stdout.
//
// A discriminated union: every object carries a "result" that names which of
// these it is, so a consumer dispatches on what the run says it did rather
// than on which other fields happen to be present. The exit code says only
// whether the tool ran; this says what it found.

// The configuration was determined. The value is the configuration, or
// nullopt for a repository that has none (rendered as null), which means the
// defaults apply. An empty configuration file evaluates to {} and is a
// configuration, distinct from null.
struct SuccessResult {
    std::optional<std::string> configJson;
};

// The repository has no flake.nix, so it fails the config job: removing the
// flake must not be a way to make CI pass.
struct MissingFlakeResult {};

// The configuration read a file that is not the configuration. Its own result
// rather than an evaluation error, because it is a rule this imposes rather
// than something Nix objected to, and what to do about it is different.
struct NotSelfContainedResult {
    std::string path;
};

// The configuration did not evaluate. The repository's problem.
struct EvalErrorResult {
    std::string message;
};

// The sources could not be read, which says nothing about the repository and
// is worth retrying.
struct FetchErrorResult {
    std::string message;
};

using Result =
    std::variant<SuccessResult, MissingFlakeResult, NotSelfContainedResult, EvalErrorResult, FetchErrorResult>;

// Render a result as the single JSON object a run writes to stdout.
std::string renderResult(const Result & result);

// The file a repository configures NixCI with.
extern const char * configFileName;

// The file whose absence fails a config job, so that deleting it cannot pass
// for a suite that has nothing to do.
extern const char * requiredFileName;

// The flake output consulted when there is no configuration file.
extern const char * fallbackAttrName;

} // namespace nixciconfig
