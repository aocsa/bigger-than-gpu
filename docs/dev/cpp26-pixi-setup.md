# C++26 Development Environment on macOS with Pixi

Isolated C++26 toolchain setup using [Pixi](https://pixi.sh) — no changes to system
dev tools (Xcode / Command Line Tools stay untouched). The compiler, CMake, and Ninja
live inside the project and are pinned in `pixi.lock`, so every machine gets the
exact same toolchain.

Verified with **pixi 0.73.0** on **macOS (Apple Silicon, osx-arm64)**.

## Why Pixi over plain conda

| | Pixi | conda |
|---|---|---|
| Isolation scope | Per-project (`.pixi/` in repo) | Global named envs |
| Activation | Not needed (`pixi run ...`) | `conda activate` every shell |
| Reproducibility | `pixi.lock` committed next to code | Manual `environment.yml` export |
| Speed | Fast (rattler resolver) | Slower |

## 1. Install Pixi

```bash
curl -fsSL https://pixi.sh/install.sh | bash
```

Restart your shell (or `source ~/.zshrc`) so `~/.pixi/bin` is on `PATH`.

```bash
pixi --version   # expect 0.73.0 or newer
```

## 2. Initialize the workspace

From the project root:

```bash
pixi init
```

This creates `pixi.toml`. Confirm the channel and platform:

```toml
[workspace]
name = "bigger-than-gpu"
channels = ["conda-forge"]
platforms = ["osx-arm64"]   # use "osx-64" on Intel Macs
```

> **Note (pixi ≥ 0.50):** channels are configured in `pixi.toml`, not on the
> command line. `pixi add --channel ...` was removed. To add a channel later:
> `pixi workspace channel add <name>`.

## 3. Add the toolchain

```bash
pixi add clang_osx-arm64 clangxx_osx-arm64 cmake ninja
```

On Intel Macs use `clang_osx-64 clangxx_osx-64` instead.

These conda-forge compiler packages ship a recent Clang (18+) plus an activation
script that exports `CC`, `CXX`, and macOS-SDK-aware flags inside the pixi
environment — nothing leaks into your system shell.

Verify (use `$CXX` — the conda-forge package ships target-prefixed binaries like
`arm64-apple-darwin20.0.0-clang++`, so a bare `clang++` still resolves to Apple's):

```bash
pixi run -- sh -c '$CXX --version'   # expect "clang version 22.x", not "Apple clang"
```

## 4. Configure CMake tasks

Add to `pixi.toml`:

```toml
[tasks]
configure = "cmake -B build -G Ninja -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX -DCMAKE_CXX_STANDARD=26"
build = "cmake --build build"
test = "ctest --test-dir build --output-on-failure"
```

In `CMakeLists.txt`, request the standard explicitly:

```cmake
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

## 5. Build and test

```bash
pixi run configure
pixi run build
pixi run test
```

`pixi run` executes inside the isolated environment — `$CC`/`$CXX` point at the
conda-forge Clang, never at `/usr/bin/clang`.

## 6. Smoke-test C++26

```bash
pixi run -- clang++ -std=c++26 -x c++ - <<'EOF'
#include <print>
int main() { std::println("C++26 toolchain OK"); }
EOF
./a.out && rm a.out
```

## C++26 feature availability (Clang 18/19, conda-forge)

| Feature | Status |
|---|---|
| `-std=c++26` flag accepted | ✅ |
| `std::print` / `std::println` (C++23) | ✅ |
| `std::expected` (C++23) | ✅ |
| `static_assert` with user-generated message | ✅ |
| Pack indexing | ✅ (Clang 19) |
| `= delete("reason")` | ✅ (Clang 19) |
| Reflection (`std::meta`) | ❌ not in any released compiler |
| Contracts | ❌ not yet |

C++26 support is partial in every compiler today; the flag works and the
already-landed features above are usable. Track progress at
[cppreference compiler support](https://en.cppreference.com/w/cpp/compiler_support/26).

## Troubleshooting

- **`error: unexpected argument '--channel' found`** — you're on pixi ≥ 0.50;
  channels belong in `pixi.toml` (see step 2), just run `pixi add <pkgs>`.
- **CMake picks up Apple Clang** — you ran `cmake` outside `pixi run`, so
  `$CC`/`$CXX` weren't set. Reconfigure from scratch: `rm -rf build && pixi run configure`.
- **Linker errors about SDK / `libc++`** — make sure both `clang_osx-arm64`
  and `clangxx_osx-arm64` are installed; the `clangxx` package carries the
  C++ activation flags.
- **New machine / teammate setup** — just `pixi install`; `pixi.lock`
  reproduces the exact toolchain.

## Files to commit

- `pixi.toml` — manifest (channels, platforms, deps, tasks)
- `pixi.lock` — pinned solve, guarantees identical toolchains
- Add `.pixi/` to `.gitignore` — it's the local environment cache
