# Tensor benchmarks

`shapes.txt` lists the gemm and conv shapes and which networks of `nets.mim` to run: an MLP and a ResNet-18.
`run.py` turns them into one CPU Mim program, compiles it with
`mim -p opt … -p ll`, links it with `driver.cpp`, times every
extern and checks it against a scalar reference.
A network is checked against its PyTorch mirror in `nets.py` instead, relative to its largest output, and stays
unchecked without PyTorch.
`--torch` also times the same computation in single-threaded eager PyTorch on the CPU.
For gemm and conv, the same call on one-element operands is subtracted from PyTorch's time: it is Python's dispatch,
which the extern does not pay.

    cmake --build build --target bench-cpu           # -DMIM_BENCH_ARGS="--torch --filter sq1024" at configure time passes options
    python3 bench/run.py --mim build/bin/mim

Results land in `<out>/bench-cpu.{json,md}`, headed by the CPU they ran on; `--summary FILE` appends the table to
FILE.
Times are the best of `reps` calls of the extern.
`faults` is the number of minor page faults per call: memory the compiled code allocates afresh on every call.
The `perf` workflow runs the CPU matrix on every pull request whose title starts with `perf`.
