{
  # An output that looks at its argument: `self` is only there in a full flake
  # evaluation, so reading this output needs more than flake.nix. The fast path
  # must defer to the full fetch rather than answer from flake.nix alone.
  outputs = { self, ... }: {
    nix-ci = {
      enable = self ? outPath;
    };
  };
}
