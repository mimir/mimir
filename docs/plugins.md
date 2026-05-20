# Plugins {#plugins}

[TOC]

Check out the [demo](@ref demo) plugin for a minimal example.
It uses our custom [`add_mim_plugin`](@ref add_mim_plugin_cmake) CMake command.
A plugin generally consists of two halves with the same name: a `<plugin>.mim` file that declares the public annexes and a shared library that registers the runtime behavior.

Plugin names may only contain letters, digits, and underscores, and are limited to 8 characters.

## Plugin Registry

The [MimIR Plugin Registry](https://mimir.github.io/plugins) is the central hub for discovering, sharing, and maintaining third-party MimIR plugins.
The registry lists available plugins and provides guidance on how to find and use them.
If you've created a plugin you'd like to share with the community, please consider submitting it to the registry.

## Create a New In-Tree Plugin

Create a new in-tree plugin `foobar` based on the [demo](@ref demo) plugin:

```sh
./scripts/new_plugin.py foobar
```

The script also supports `-h`/`--help` and prints the same usage text when called incorrectly.

By default, the script creates an in-tree plugin and updates `src/mim/plug/CMakeLists.txt`.
The generated files are:

- `src/mim/plug/<plugin>/<plugin>.mim`
- `src/mim/plug/<plugin>/CMakeLists.txt`
- `src/mim/plug/<plugin>/<plugin>.cpp`
- `src/mim/plug/<plugin>/normalizers.cpp`
- `include/mim/plug/<plugin>/<plugin>.h`
- `lit/<plugin>/const.mim`

## Create a Third-Party Plugin

To create a self-contained third-party plugin repository in `extra/`, use:

```sh
./scripts/new_plugin.py foobar --extra
```

This creates `extra/<plugin>/` with:

- `<plugin>.mim`
- `CMakeLists.txt`
- `src/<plugin>.cpp`
- `src/normalizers.cpp`
- `include/mim/plug/<plugin>/<plugin>.h`
- `lit/const.mim`
- `.github/workflows/{linux,macos,windows}.yml` — GitHub Actions CI/CD workflows

In `--extra` mode, the script also:
- Initializes a new Git repository for the plugin
- Generates GitHub Actions workflows that automatically build and test the plugin against the main MimIR repository
- Patches workflow configurations to clone the main repository as the parent and the plugin as a submodule in `extra/<plugin>`

### Third-Party Plugin Discovery

If you clone a plugin repository into `extra/`, MimIR picks it up automatically during configuration when the repository contains a `CMakeLists.txt` as a direct child of `extra/`.

If the plugin repository also contains `lit/*.mim` tests, they are picked up automatically by the main `lit` target as well.

## Extract an Existing In-Tree Plugin

To move an existing in-tree plugin into `extra/foobar`, use:

```sh
./scripts/extract_plugin.py foobar
```

This moves:

- `src/mim/plug/<plugin>/` into `extra/<plugin>/`
- `include/mim/plug/<plugin>/` into `extra/<plugin>/include/...`
- `lit/<plugin>/` into `extra/<plugin>/lit/`

It also:

- Rewrites the extracted `CMakeLists.txt` for out-of-tree use
- Removes the plugin from the in-tree plugin list so it is picked up through `extra/` instead
- Generates GitHub Actions workflows that automatically build and test the extracted plugin against the main MimIR repository

The extracted plugin is staged with `git add` but not committed, allowing you to review the changes before committing.

## Standalone Third-Party Builds

After installing MimIR, a third-party plugin only needs to find the `mim` package.
For example, a plugin called `foo` can be set up like this:

```cmake
cmake_minimum_required(VERSION 3.25 FATAL_ERROR)
project(foo)

if(NOT COMMAND add_mim_plugin)
    find_package(mim REQUIRED)
endif()

add_mim_plugin(foo
    SOURCES
        src/foo.cpp
        src/normalizers.cpp
)
```

Configure the project standalone with:

```sh
cmake .. -Dmim_DIR=<MIM_INSTALL_PREFIX>/lib/cmake/mim
```

The authoritative reference for `add_mim_plugin` itself lives in [`cmake/Mim.cmake`](@ref add_mim_plugin_cmake).

## Normalizers

Normalizers usually obtain the owning [World](@ref mim::World) from one of their arguments, often `type->world()`, and then build the replacement directly in that world.
Small normalizers are expected to be direct and side-effect free.

That often leads to tiny functions of the form:

```cpp
const Def* normalize_const(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();
    return world.lit(world.type_idx(arg), 42);
}
```
