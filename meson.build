project(
  'nix-ci-config',
  'cpp',
  version : '0.0.0',
  default_options : ['cpp_std=c++20', 'warning_level=2', 'werror=true'],
)

json_dep = dependency('nlohmann_json')

nix_deps = [
  dependency('nix-expr'),
  dependency('nix-store'),
  dependency('nix-flake'),
  dependency('nix-main'),
  dependency('nix-cmd'),
  dependency('nix-fetchers'),
  dependency('nix-util'),
  json_dep,
]

# The argument parsing, the forge file APIs and the output lines, free of the
# Nix libraries so they can be unit-tested on their own.
core_lib = static_library(
  'nix-ci-config-core',
  ['src/pure.cc'],
  dependencies : [json_dep],
)

# Everything that links the Nix evaluator.
nix_lib = static_library(
  'nix-ci-config-nix',
  ['src/forge_accessor.cc', 'src/config_eval.cc'],
  dependencies : nix_deps,
)

executable(
  'nix-ci-config',
  'src/main.cc',
  link_with : [core_lib, nix_lib],
  dependencies : nix_deps,
  install : true,
)

pure_test = executable(
  'pure-test',
  'test/pure_test.cc',
  link_with : [core_lib],
  include_directories : include_directories('src'),
  dependencies : [json_dep],
)
test('pure', pure_test)
