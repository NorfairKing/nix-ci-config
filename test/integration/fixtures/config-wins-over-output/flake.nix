{
  # This flake also exposes a nix-ci output, with a different value, to assert
  # that the nix-ci.nix file is preferred over it rather than the fallback
  # running when the file is present.
  outputs = _: {
    nix-ci = {
      enable = true;
      systems = [ "aarch64-linux" ];
    };
  };
}
