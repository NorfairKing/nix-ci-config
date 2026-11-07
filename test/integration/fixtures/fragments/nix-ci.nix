assert builtins.pathExists ./nix-ci.d;
{ enable = true; systems = [ "x86_64-linux" ]; }
