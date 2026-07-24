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

A configuration that is not a single self-contained file is `not-self-contained`,
naming the `path` it read that is not the configuration:

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
otherwise. A configuration has to be a single self-contained file; one that
imports another, reads a directory or follows a symlink is `not-self-contained`.
