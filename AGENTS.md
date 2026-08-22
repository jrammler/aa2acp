# Project conventions

## Local development environment

Keep machine-specific development details—such as hostnames, credentials,
signing keys, copied-closure paths, and cloud-init state—only in local agent
memory. Do not add them to tracked source, documentation, configuration, or
commits.

This repository contains only the Raspberry Pi C++ bridge. Run project tools
through Nix:

```bash
nix develop --command bash -c 'cmake -S . -B build && cmake --build build -j2'
```

Before committing C++ changes, format, build, test, and check the diff:

```bash
nix develop --command bash -c '
  find include src tests -type f \( -name "*.cpp" -o -name "*.hpp" \) \
    -print0 | xargs -0 clang-format -i
  cmake -S . -B build
  cmake --build build -j2
  ctest --test-dir build --output-on-failure
  git diff --check
'
```

Keep commits focused and commit completed work regularly. The production target
is AA2ACP; do not add external head-unit software, emulators, ESP32 experiments,
or unrelated test assets to this repository.
