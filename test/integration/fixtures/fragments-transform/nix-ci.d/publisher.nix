{
  test.publisher = { package = "packages.x86_64-linux.publish"; branches = "main"; };
  dependencies."test.packages.x86_64-linux.publish" = [ "checks.x86_64-linux.test" ];
}
