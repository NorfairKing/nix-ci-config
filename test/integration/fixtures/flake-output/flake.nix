{
  outputs = _: {
    nix-ci = {
      enable = true;
      systems = [ "aarch64-linux" ];
    };
  };
}
