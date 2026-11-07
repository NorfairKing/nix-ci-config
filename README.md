# nix-ci-config

Read a repository's NixCI configuration from a flake reference.

```console
$ nix run github:NorfairKing/nix-ci-config -- --flake github:NorfairKing/autodocodec
```

The output is one JSON object, carrying a `result` that says what the run
found. The exit code says only whether the tool ran.

A configuration that was read is `success`, with `config` the configuration (or
`null` for a repository that has none):

```json
{
  "result": "success",
  "config": {
    "cachix": {
      "name": "autodocodec"
    }
  }
}
```

A repository with no `flake.nix` is `missing-flake`, which fails the job so that
deleting the flake cannot pass for a suite with nothing to do:

```json
{
  "result": "missing-flake"
}
```

A configuration file that reads another file is `not-self-contained`,
naming the forbidden `path`:

```json
{
  "result": "not-self-contained",
  "path": "systems.nix"
}
```

A configuration that does not evaluate is `eval-error`, the repository's own
problem:

```json
{
  "result": "eval-error",
  "message": "error: this configuration is broken"
}
```

Sources that could not be read are `fetch-error`, the one result worth retrying:

```json
{
  "result": "fetch-error",
  "message": "error: Failed to fetch git repository 'https://github.com/owner/repo'"
}
```

It reads `nix-ci.nix` if the repository has one, the flake's `nix-ci` output
otherwise. With `nix-ci.nix`, it also discovers regular `nix-ci.d/*.nix`
fragments at the same revision, in filename order. Fragments may define only
`test` and `dependencies` attribute sets. Duplicate keys fail evaluation and
name both defining files; entries are never silently overwritten.

For policy shared by every service, the root may instead be a function of
the merged fragment attribute set. It must return the complete configuration:

```nix
fragments:
fragments // {
  enable = true;
  test = builtins.mapAttrs (_: job: job // { enable = false; }) fragments.test;
}
```

Duplicate fragment keys are rejected before calling the function. The root
function has the same file-access restrictions as an attribute-set root.
Older readers reject function roots, rather than running jobs without policy.
Deploy a function-capable reader before switching the root to this form.

On GitHub, fragment contents are read through GraphQL in batches of at most
64 files after directory discovery. A 52-fragment configuration uses three
REST requests and one GraphQL request, rather than 55 REST requests.
GitLab and Forgejo retain per-file reads. Incomplete or failed batches fall
back to the repository fetcher, never to per-fragment REST requests.

Files cannot import each other. The root may inspect `nix-ci.d`; fragments
cannot read other files or directories. Discovery is not recursive and
symlink fragments are rejected. An absent directory preserves single-file
behavior. Flake-output fallback does not discover fragments.

Start a split configuration with `assert builtins.pathExists ./nix-ci.d;`
so older readers reject it rather than silently omit its jobs. Deploy the
fragment-capable reader before migrating repositories; restore the
single-file configuration before rolling back the reader.
