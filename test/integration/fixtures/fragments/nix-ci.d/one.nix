{
  test.one = { package = "packages.x86_64-linux.one"; branches = "main"; };
  dependencies."test.packages.x86_64-linux.one" = [ "checks.x86_64-linux.one" ];
}
