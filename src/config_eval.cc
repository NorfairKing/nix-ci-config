#include "config_eval.hh"

#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <vector>

#include <nlohmann/json.hpp>

#include <nix/cmd/common-eval-args.hh>
#include <nix/expr/eval-gc.hh>
#include <nix/expr/eval-settings.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/value-to-json.hh>
#include <nix/expr/value.hh>
#include <nix/fetchers/attrs.hh>
#include <nix/fetchers/fetch-settings.hh>
#include <nix/fetchers/fetchers.hh>
#include <nix/flake/flake.hh>
#include <nix/flake/flakeref.hh>
#include <nix/flake/settings.hh>
#include <nix/main/shared.hh>
#include <nix/store/filetransfer.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/canon-path.hh>
#include <nix/util/configuration.hh>
#include <nix/util/source-path.hh>
#include <nix/util/terminal.hh>
#include <nix/util/url.hh>

#include "forge_accessor.hh"

namespace nixciconfig {

namespace {

nix::SourcePath repoPath(const nix::ref<nix::SourceAccessor> & accessor, const char * name) {
    return nix::SourcePath{accessor, nix::CanonPath{std::string("/") + name}};
}

// Strip the ".git" a clone URL may carry, which is not part of the repository
// name any forge API takes.
std::string withoutGitSuffix(const std::string & repo) {
    const std::string suffix = ".git";
    if (repo.size() > suffix.size() && repo.compare(repo.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return repo.substr(0, repo.size() - suffix.size());
    }
    return repo;
}

// The repository a git URL names, if it names one at a host with a file API.
std::optional<ForgeRepo> forgeRepoFromGitUrl(
    const std::string & url, const std::string & rev, const std::optional<Forge> & forgeType) {
    nix::ParsedURL parsed;
    try {
        parsed = nix::parseURL(url);
    } catch (const nix::Error &) {
        return std::nullopt;
    }
    if (!parsed.authority) {
        return std::nullopt;
    }
    // What the caller says the host runs settles it; recognising a host is for
    // when nobody said.
    auto forge = forgeType ? forgeType : forgeForHost(parsed.authority->host);
    if (!forge) {
        return std::nullopt;
    }

    // With an authority the path starts with an empty component, and what
    // follows is the repository.
    if (parsed.path.size() < 3 || !parsed.path[0].empty()) {
        return std::nullopt;
    }
    // A flake reference names a git URL with a "git+" in front of the scheme
    // the host actually speaks. A forge serves its API over https however its
    // repositories are cloned, so only a reference that says plain http asks
    // for anything else: the worker clones over https and a user reproducing
    // it clones over ssh, and both have to read the same API.
    auto scheme = parsed.scheme;
    const std::string gitPrefix = "git+";
    if (scheme.compare(0, gitPrefix.size(), gitPrefix) == 0) {
        scheme = scheme.substr(gitPrefix.size());
    }
    if (scheme != "http") {
        scheme = "https";
    }

    auto repo = withoutGitSuffix(parsed.path.back());
    // GitLab nests namespaces, and its projects endpoint takes the whole one
    // as a single encoded component, so everything above the repository is the
    // owner. The other two do not nest, so a deeper path there is not a
    // repository this can name.
    if (*forge != Forge::GitLab && parsed.path.size() != 3) {
        return std::nullopt;
    }
    std::string owner;
    for (size_t i = 1; i + 1 < parsed.path.size(); ++i) {
        if (!owner.empty()) {
            owner += "/";
        }
        owner += parsed.path[i];
    }

    // A port belongs to the host as far as addressing it goes. No forge worth
    // the name is on one, so this only ever carries a test server's.
    auto host = parsed.authority->host;
    if (parsed.authority->port) {
        host += ":" + std::to_string(*parsed.authority->port);
    }

    return ForgeRepo{*forge, host, owner, repo, rev, scheme};
}

// The repository a flake reference names, if its file API can serve this
// evaluation: a forge this knows, at a revision, named by owner and
// repository.
std::optional<ForgeRepo> forgeRepoFor(const nix::FlakeRef & flakeRef, const std::optional<Forge> & forgeType) {
    const auto & input = flakeRef.input;

    // Without a revision there is nothing to pin the file requests to, and
    // resolving a branch to one would cost the round trip this saves.
    auto rev = input.getRev();
    if (!rev) {
        return std::nullopt;
    }
    auto revString = rev->gitRev();

    auto type = input.getType();
    if (type == "github" || type == "gitlab") {
        auto owner = nix::fetchers::maybeGetStrAttr(input.attrs, "owner");
        auto repo = nix::fetchers::maybeGetStrAttr(input.attrs, "repo");
        if (!owner || !repo) {
            return std::nullopt;
        }
        auto host = nix::fetchers::maybeGetStrAttr(input.attrs, "host")
                        .value_or(type == "github" ? "github.com" : "gitlab.com");
        auto forge = forgeType ? forgeType : forgeForHost(host);
        if (!forge) {
            return std::nullopt;
        }
        return ForgeRepo{*forge, host, *owner, withoutGitSuffix(*repo), revString};
    }

    if (type == "git") {
        auto url = nix::fetchers::maybeGetStrAttr(input.attrs, "url");
        if (!url) {
            return std::nullopt;
        }
        return forgeRepoFromGitUrl(*url, revString, forgeType);
    }

    return std::nullopt;
}

// The access tokens Nix was configured with, which is where the credentials
// for a private repository already are: whatever lets the rest of a job clone
// it lets this read one file out of it.
std::map<std::string, std::string> configuredAccessTokens() {
    std::map<std::string, std::string> tokens;
    for (const auto & [host, token] : nix::fetchSettings.accessTokens.get()) {
        tokens.emplace(host, token);
    }
    return tokens;
}

nix::ref<nix::EvalState> makeEvalState(const nix::ref<nix::Store> & store) {
    return nix::make_ref<nix::EvalState>(nix::LookupPath{}, store, nix::fetchSettings, nix::evalSettings);
}

std::string renderValueAsJSON(nix::EvalState & state, nix::Value & value) {
    nix::NixStringContext context;
    return nix::printValueAsJSON(state, true, value, nix::noPos, context).dump();
}

// Evaluate the configuration file, which is a plain Nix file rather than a
// flake output.
//
// Pure by default, so a configuration read out of an untrusted repository
// cannot reach the host's environment or files. A caller that knows a
// configuration needs `builtins.currentSystem` or the like asks for @--impure@,
// which is what `nix eval --file` does.
// [check:tag ConfigFileImpure]
std::string evaluateConfigFile(const nix::ref<nix::Store> & store, const nix::SourcePath & path, bool impure) {
    nix::evalSettings.pureEval = !impure;
    auto state = makeEvalState(store);
    nix::Value value;
    state->evalFile(path, value);
    return renderValueAsJSON(*state, value);
}

// Evaluate the flake's configuration output, for a repository that has no
// configuration file.
//
// This costs a full fetch of the flake and its inputs however the sources were
// reached, because an output is not a file that can be read on its own.
//
// [check:tag ConfigEvalFallback] Where NixCI looks for a configuration, and in
// what order, is decided here. Keep what the repro command tells a user in
// step with it.
std::optional<std::string> evaluateFlakeFallback(const nix::ref<nix::Store> & store, const nix::FlakeRef & flakeRef) {
    // Flake evaluation is pure, as it is under `nix eval .#nix-ci`.
    nix::evalSettings.pureEval = true;
    auto state = makeEvalState(store);

    nix::flake::LockFlags lockFlags;
    // The sources are read-only wherever they came from, so a lock file that
    // needs updating is updated in memory and never written back.
    lockFlags.writeLockFile = false;
    // Locking is where the flake and its inputs are fetched. A failure here is
    // the sources not being reachable, which says nothing about the
    // configuration and is worth retrying, so it is told apart from an
    // evaluation error that the flake output itself would raise below.
    auto locked = [&] {
        try {
            return nix::flake::lockFlake(nix::flakeSettings, *state, flakeRef, lockFlags);
        } catch (const nix::Error & e) {
            throw FetchFailed(nix::filterANSIEscapes(e.what(), true));
        }
    }();

    auto * outputs = state->allocValue();
    nix::flake::callFlake(*state, locked, *outputs);
    state->forceAttrs(*outputs, nix::noPos, "the flake outputs");

    const auto * attr = outputs->attrs()->get(state->symbols.create(fallbackAttrName));
    if (attr == nullptr) {
        return std::nullopt;
    }
    return renderValueAsJSON(*state, *attr->value);
}

// A value that raises the moment it is forced.
nix::Value * throwOnForce(nix::EvalState & state) {
    // Every field is named because the toolchain treats a skipped one as an
    // error, and `impl` in particular has no default to fall back on.
    auto * op = new nix::PrimOp{
        .name = "nixCiConfigFlakeInput",
        .args = {},
        .arity = 1,
        .doc = std::nullopt,
        .addTrace = true,
        .impl =
            [](nix::EvalState &, const nix::PosIdx, nix::Value **, nix::Value &) {
                throw FallbackNeeded("the flake output reads its inputs or `self`");
            },
        .experimentalFeature = std::nullopt,
        .internal = false,
    };

    auto * prim = state.allocValue();
    prim->mkPrimOp(op);
    auto * dummy = state.allocValue();
    dummy->mkNull();
    auto * value = state.allocValue();
    value->mkApp(prim, dummy);
    return value;
}

// The argument the flake's `outputs` function is applied to for the fast read.
//
// It is an attribute set carrying `self` and every declared input, so a
// `{ self, nixpkgs, ... }:` pattern destructures against it, but each of those
// values throws the moment it is forced. So the output evaluates only as far as
// it is independent of `self` and its inputs -- which the `nix-ci` output
// almost always is, since it is a description rather than a build -- and that
// makes a fast result safe to trust: it is bit for bit what the full flake
// evaluation would have produced. An output that does reach for `self` or an
// input forces a throw and lands in the fallback instead.
nix::Value * throwingInputsArg(nix::EvalState & state, nix::Value & flake) {
    const auto * inputsAttr = flake.attrs()->get(state.symbols.create("inputs"));
    size_t inputCount = 0;
    if (inputsAttr != nullptr) {
        // Forcing this reads the input declarations, which is enough to name
        // them; it does not fetch them, which is the whole point.
        state.forceAttrs(*inputsAttr->value, nix::noPos, "the flake inputs");
        inputCount = inputsAttr->value->attrs()->size();
    }

    auto bindings = state.buildBindings(inputCount + 1);
    bindings.insert(state.symbols.create("self"), throwOnForce(state));
    if (inputsAttr != nullptr) {
        for (const auto & input : *inputsAttr->value->attrs()) {
            bindings.insert(input.name, throwOnForce(state));
        }
    }

    auto * arg = state.allocValue();
    arg->mkAttrs(bindings);
    return arg;
}

// Evaluate the flake's configuration output reading only flake.nix, which is
// enough whenever the output is independent of `self` and the flake's inputs,
// as the `nix-ci` output almost always is. This is what spares a repository
// with no configuration file the full fetch that reading its output would
// otherwise cost.
//
// The three answers are distinct: nullopt is "this could not stand in for the
// full evaluation", so read the sources in full; an inner nullopt is "there is
// no output, decided from flake.nix alone", which is a real answer; and a value
// is the configuration the output produced.
//
// [check:ref ConfigEvalFallback]
std::optional<std::optional<std::string>> evaluateFlakeOutputFast(
    const nix::ref<nix::Store> & store, const nix::ref<nix::SourceAccessor> & accessor) {
    try {
        // Flake evaluation is pure, as it is under `nix eval .#nix-ci`.
        nix::evalSettings.pureEval = true;
        auto state = makeEvalState(store);

        nix::Value flake;
        state->evalFile(repoPath(accessor, requiredFileName), flake);
        state->forceAttrs(flake, nix::noPos, "the flake");
        const auto * outputsAttr = flake.attrs()->get(state->symbols.create("outputs"));
        if (outputsAttr == nullptr) {
            // Not a flake this understands; let the full evaluation be the
            // judge of what it is.
            return std::nullopt;
        }

        nix::Value outputs;
        state->callFunction(*outputsAttr->value, *throwingInputsArg(*state, flake), outputs, nix::noPos);
        state->forceAttrs(outputs, nix::noPos, "the flake outputs");
        const auto * attr = outputs.attrs()->get(state->symbols.create(fallbackAttrName));
        if (attr == nullptr) {
            return std::optional<std::string>(std::nullopt);
        }
        return std::optional<std::string>(renderValueAsJSON(*state, *attr->value));
    } catch (...) {
        // The output looked at its inputs or `self`, read another file, or would
        // not evaluate this way at all: none of those is an answer, only a
        // reason to read the sources in full and evaluate them the ordinary way.
        return std::nullopt;
    }
}

// Read the configuration through one accessor. Answers nullopt when the
// accessor turned out not to be able to serve the evaluation, which is not a
// failure but a reason to try again with another one.
std::optional<Result> attempt(
    const nix::ref<nix::Store> & store,
    const nix::FlakeRef & flakeRef,
    const nix::ref<nix::SourceAccessor> & accessor,
    const std::shared_ptr<AccessPolicy> & policy,
    ForgeAccessor * forgeAccessor,
    const std::function<std::optional<std::optional<std::string>>()> & tryFastOutput,
    bool impure,
    bool * fullFetch) {
    auto fellBack = [&] { return forgeAccessor != nullptr && forgeAccessor->fallbackNeeded(); };
    // A configuration that read something it may not is the repository's
    // problem, so it is reported rather than retried, and reported the same
    // way whichever accessor caught it.
    auto rejected = [&]() -> std::optional<Result> {
        if (!policy->violation) {
            return std::nullopt;
        }
        return Result{NotSelfContainedResult{*policy->violation}};
    };

    std::optional<Result> success;
    try {
        // A repository without a flake fails the config job rather than
        // passing it with nothing to do, so that removing the flake cannot be
        // a way to make CI green.
        if (!repoPath(accessor, requiredFileName).pathExists()) {
            if (fellBack()) {
                return std::nullopt;
            }
            if (forgeAccessor != nullptr) {
                // A forge answers "not found" for a repository the
                // credentials cannot see exactly as it does for a file that is
                // not there, and the two mean opposite things: one is the
                // repository's fault and fails the job, the other is ours and
                // is worth retrying. Fetching the repository tells them apart,
                // because reading it needs the same permission and says so
                // when it does not have it.
                //
                // This costs a fetch only where the file is reported absent,
                // which is a repository that is failing either way.
                forgeAccessor->noteFallback(
                    std::string(requiredFileName) + " was not found, which a forge also says for a repository it "
                                                    "will not show us");
                return std::nullopt;
            }
            return Result{MissingFlakeResult{}};
        }

        auto configPath = repoPath(accessor, configFileName);
        if (configPath.pathExists()) {
            success = Result{SuccessResult{evaluateConfigFile(store, configPath, impure)}};
        } else {
            // No configuration file: the configuration is the flake's `nix-ci`
            // output. Where the sources are behind a forge API, that output is
            // tried against the flake.nix already in hand first, and only a full
            // fetch is done when reading it turns out to need more than that
            // file.
            std::optional<std::optional<std::string>> fromOutput;
            if (tryFastOutput) {
                fromOutput = tryFastOutput();
            }
            if (!fromOutput) {
                if (fullFetch != nullptr) {
                    *fullFetch = true;
                }
                fromOutput.emplace(evaluateFlakeFallback(store, flakeRef));
            }
            success = Result{SuccessResult{*fromOutput}};
        }
    } catch (const NotSelfContained &) {
        return rejected();
    } catch (const FallbackNeeded &) {
        return std::nullopt;
    } catch (const FetchFailed & e) {
        // Reaching the flake or an input failed while reading its output; that
        // is worth retrying, and unlike an evaluation error it is not the
        // repository's fault.
        return Result{FetchErrorResult{e.message}};
    } catch (const nix::Error & e) {
        // Either of those may surface as whatever the evaluator wrapped it in,
        // so what was recorded decides, not the exception that came out.
        if (auto rejection = rejected()) {
            return rejection;
        }
        if (fellBack()) {
            return std::nullopt;
        }
        return Result{EvalErrorResult{nix::filterANSIEscapes(e.what(), true)}};
    }

    // The evaluation may have read a forbidden file, or exhausted the forge
    // accessor, without the exception surfacing here, so what the accessor
    // recorded decides over the value it produced.
    if (auto rejection = rejected()) {
        return rejection;
    }
    if (fellBack()) {
        return std::nullopt;
    }
    return success;
}

} // namespace

void initNixRuntime() {
    nix::initNix();
    nix::initGC();

    // This tool reads flake references and may evaluate a flake output, so it
    // enables the features that needs whatever the ambient configuration says.
    // Appended, so ambient features are preserved.
    nix::experimentalFeatureSettings.experimentalFeatures.set("flakes nix-command", true);

    nix::flakeSettings.configureEvalSettings(nix::evalSettings);

    // Reading a configuration never builds anything, and a build hook plus
    // import-from-derivation can turn an evaluation into an "unexpected EOF".
    nix::settings.getWorkerSettings().builders = "";
}

int run(const Args & args, std::ostream & out, std::ostream & err) {
    // Exactly one JSON object goes to stdout, whatever the run concluded. The
    // exit code is 0 as long as the tool ran; a non-zero exit is reserved for
    // the tool itself breaking, which reaches this from an uncaught exception
    // rather than from here.
    auto emit = [&out](const Result & result) { out << renderResult(result) << "\n" << std::flush; };

    try {
        auto flakeRef =
            nix::parseFlakeRef(nix::fetchSettings, args.flakeRef, std::filesystem::current_path());
        auto store = nix::openStore();

        auto forgeRepo = forgeRepoFor(flakeRef, args.forgeType);

        // The two files a configuration is made of, and the only two anything
        // here will read: the configuration itself and the flake whose
        // presence is asserted.
        // [check:ref SelfContainedConfiguration]
        auto configurationFiles = [] {
            return std::make_shared<AccessPolicy>(std::set<std::string>{requiredFileName, configFileName});
        };

        if (forgeRepo) {
            auto token = tokenFor(configuredAccessTokens(), *forgeRepo);
            auto cache = std::make_shared<ForgeAccessor::Cache>();
            auto policy = configurationFiles();
            auto forgeAccessor = nix::make_ref<ForgeAccessor>(*forgeRepo, token, policy, cache);

            // Reading the flake's output over the file API reads flake.nix
            // through an accessor of its own, so that an output that reaches for
            // another file falls back cleanly rather than being mistaken for a
            // configuration that read something it may not. It shares the cache,
            // so the flake.nix already fetched is not fetched again.
            auto tryFastOutput = [&]() -> std::optional<std::optional<std::string>> {
                auto outputPolicy = std::make_shared<AccessPolicy>(std::set<std::string>{requiredFileName});
                auto outputAccessor = nix::make_ref<ForgeAccessor>(*forgeRepo, token, outputPolicy, cache);
                return evaluateFlakeOutputFast(store, outputAccessor);
            };

            bool fullFetch = false;
            if (auto result =
                    attempt(store, flakeRef, forgeAccessor, policy, &(*forgeAccessor), tryFastOutput, args.impure, &fullFetch)) {
                // How the sources were read is a diagnostic, not part of what
                // the run concluded, so it goes to stderr where the job log
                // shows it without touching the stdout object.
                if (fullFetch) {
                    // The forge served flake.nix, but reading its output needed
                    // more than that one file, so the flake was fetched in full
                    // after all.
                    err << "Read flake.nix over the forge file API in " << forgeAccessor->requestCount()
                        << " request(s), then fetched the flake in full to evaluate its " << fallbackAttrName
                        << " output.\n";
                } else {
                    err << "Read the configuration over the forge file API in " << forgeAccessor->requestCount()
                        << " request(s).\n";
                }
                emit(*result);
                return 0;
            }
            err << "The forge file API could not serve this evaluation (" << forgeAccessor->fallbackReason()
                << "); fetching the repository instead.\n";
        }

        auto [fetched, _lockedInput] = flakeRef.input.getAccessor(nix::fetchSettings, *store);
        auto policy = configurationFiles();
        // Held to the same one file as the fast path, so that what a
        // configuration may be made of does not depend on where it was read
        // from.
        nix::ref<nix::SourceAccessor> accessor = nix::make_ref<RestrictedAccessor>(fetched, policy);
        if (auto result = attempt(store, flakeRef, accessor, policy, nullptr, {}, args.impure, nullptr)) {
            emit(*result);
            return 0;
        }
        // Only a forge accessor asks to fall back, and there is nothing left
        // to fall back to.
        emit(Result{FetchErrorResult{"could not read the sources"}});
        return 0;
    } catch (const nix::Error & e) {
        // Everything reading a configuration can raise is caught inside
        // 'attempt', so what reaches here is reaching the sources at all:
        // parsing the reference, opening the store, or fetching.
        emit(Result{FetchErrorResult{nix::filterANSIEscapes(e.what(), true)}});
        return 0;
    }
}

} // namespace nixciconfig
