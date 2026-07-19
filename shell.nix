{ pkgs ? import <nixpkgs> {} }:

let
  mim = pkgs.writeShellScriptBin "mim"
    ''
    MIM_BIN=build/bin/mim
    if [ -f "$MIM_BIN" ]; then
      DBG=""
      if [ "$1" = "dbg" ]; then
        shift
        DBG="cgdb --args"
      fi

      if [ "$1" = "dot" ]; then
        shift
        $DBG $MIM_BIN -P build/lib64/mim --output-dot out.dot "$@" && [ -z "$DBG" ] && xdot out.dot
      elif [ "$1" = "nest" ]; then
        shift
        $DBG $MIM_BIN -P build/lib64/mim --output-nest nest.dot "$@" && [ -z "$DBG" ] && xdot nest.dot
      elif [ "$1" = "cfg" ]; then
        shift
        $DBG $MIM_BIN -P build/lib64/mim --output-cfg cfg.dot "$@" && [ -z "$DBG" ] && xdot cfg.dot
      elif [ "$1" = "spirv" ]; then
        shift
        $DBG $MIM_BIN -P build/lib64/mim --output-spirv out.spvasm "$@"
      elif [ "$1" = "mim" ]; then
        shift
        $DBG $MIM_BIN -P build/lib64/mim --output-mim out.mim "$@"
      elif [ "$1" = "ll" ]; then
        shift
        $DBG $MIM_BIN -P build/lib64/mim --output-ll out.ll "$@"
      elif [ "$1" = "rr" ]; then
        shift
        rr record $MIM_BIN -P build/lib64/mim "$@"
        rr replay -d cgdb
      elif [ "$1" = "raw" ]; then
        shift
        $DBG $MIM_BIN "$@"
      else
        $DBG $MIM_BIN -P build/lib64/mim "$@"
      fi
    else
      echo "Mim binary not found, try building first."
    fi
    '';
in
pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
  nativeBuildInputs = with pkgs; [
    # build tools
    cmakeWithGui
    llvmPackages.clang-tools
    libllvm

    # testing
    # lldb
    gcc
    gdb
    cgdb
    rr
    (python3.withPackages (ppkgs: with ppkgs; [
      lit
      psutil
    ]))
    xdot
    graphviz
    valgrind

    spirv-tools

    # Vulkan: headers/loader for building+linking the vulkan_runner test
    # harness, validation-layers for debugging, mesa for its lavapipe
    # software ICD (headless rendering, no GPU required -- see shellHook).
    vulkan-headers
    vulkan-loader
    vulkan-validation-layers
    mesa

    # documentation
    doxygen
    texlive.combined.scheme-full
    ghostscriptX

    pre-commit
    mim
  ];

  shellHook = ''
    export VK_ICD_FILENAMES="${pkgs.mesa}/share/vulkan/icd.d/lvp_icd.x86_64.json"
    export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
    export VK_LAYER_PATH="${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d"
  '';
}
