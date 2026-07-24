{
  description = "nix-ci-config: read a repository's NixCI configuration without cloning it";

  nixConfig = {
    extra-substituters = "https://nix-ci.cachix.org";
    extra-trusted-public-keys = "nix-ci.cachix.org-1:c8XSlMMNP/rw15P2ByIwOzscge/IWStW/mie3LjU85Q=";
  };

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      # The tool is proprietary (all rights reserved), so it is marked unfree.
      pkgs = import nixpkgs {
        inherit system;
        config.allowUnfree = true;
      };
    in
    {
      overlays.default = _final: _prev: {
        nix-ci-config = pkgs.callPackage ./package.nix { };
      };

      packages.${system} = {
        nix-ci-config = pkgs.callPackage ./package.nix { };
        default = self.packages.${system}.nix-ci-config;
      };

      checks.${system} = {
        # Compiles the tool and runs its unit tests and both integration
        # suites, which the package does as part of its own check phase.
        nix-ci-config = self.packages.${system}.nix-ci-config;
      };

      devShells.${system}.default = pkgs.mkShell {
        inputsFrom = [ self.packages.${system}.nix-ci-config ];
        packages = [ pkgs.clang-tools ];
      };
    };
}
