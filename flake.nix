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
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forEachSystem = nixpkgs.lib.genAttrs systems;
      # The tool is proprietary (all rights reserved), so it is marked unfree.
      pkgsFor = sys: import nixpkgs {
        system = sys;
        config.allowUnfree = true;
      };

      # The dev shell is only built where it is developed, so it stays a
      # single-system output while the tool itself is built for both.
      devSystem = "x86_64-linux";
    in
    {
      # Built with the consumer's package set, so the tool links the same nix
      # as whatever else that package set builds.
      overlays.default = final: _prev: {
        nix-ci-config = final.callPackage ./package.nix { };
      };

      packages = forEachSystem (sys:
        let pkgs = pkgsFor sys; in
        {
          nix-ci-config = pkgs.callPackage ./package.nix { };
          default = self.packages.${sys}.nix-ci-config;
        });

      checks = forEachSystem (sys:
        {
          # Compiles the tool and runs its unit tests and both integration
          # suites, which the package does as part of its own check phase.
          nix-ci-config = self.packages.${sys}.nix-ci-config;
        });

      devShells.${devSystem}.default =
        let pkgs = pkgsFor devSystem; in
        pkgs.mkShell {
          inputsFrom = [ self.packages.${devSystem}.nix-ci-config ];
          packages = [ pkgs.clang-tools ];
        };
    };
}
