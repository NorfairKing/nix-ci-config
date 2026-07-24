{
  # An input, and an `outputs` that destructures it and `self`, but a nix-ci
  # output that uses neither. Reading it must not fetch the input.
  inputs.dep.url = "path:./dep";
  outputs = { self, dep, ... }: {
    nix-ci = {
      enable = true;
      systems = [ "x86_64-linux" ];
    };
  };
}
