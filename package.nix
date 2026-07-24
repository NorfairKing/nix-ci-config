# Build of the nix-ci-config configuration reader.
#
# It dynamically links the LGPL-2.1 Nix evaluator libraries, which is what the
# LGPL permits.
{ lib
, stdenv
, meson
, ninja
, pkg-config
, nix
, nlohmann_json
, jq
, python3
}:
stdenv.mkDerivation {
  pname = "nix-ci-config";
  version = "0.0.0";

  src = ./.;

  # nixpkgs' meson hook defaults to --buildtype=plain, which adds no
  # optimisation flags at all. Build the shipped binary optimised.
  mesonBuildType = "release";

  nativeBuildInputs = [ meson ninja pkg-config ];
  buildInputs = [ nix nlohmann_json ];

  doCheck = true;
  # python3 serves the stand-in forge the file-API test reads from.
  nativeCheckInputs = [ jq python3 ];

  # The integration tests drive the built binary against the fixtures, which
  # is a thing meson has no binary to hand for, so they run after the unit
  # tests rather than as one of them.
  postCheck = ''
    export NIX_CI_CONFIG_BIN="$PWD/nix-ci-config"
    bash ../test/integration/run.sh
    bash ../test/forge/run.sh
  '';

  meta = {
    description = "Read a repository's NixCI configuration from a flake reference";
    mainProgram = "nix-ci-config";
    license = lib.licenses.unfree;
    platforms = lib.platforms.linux;
  };
}
