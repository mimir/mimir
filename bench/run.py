#!/usr/bin/env python3
"""Compiles bench/shapes.txt into one CPU Mim program, times its externs with driver.cpp and reports a table.

    run.py --mim build/bin/mim [--torch] [--filter sq256 ...] [--summary FILE]

Writes <out>/bench-cpu.json and .md; --summary appends the table to FILE (e.g. $GITHUB_STEP_SUMMARY)."""

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

import nets

HERE = Path(__file__).resolve().parent

PRELUDE = """plugin tensor;
plugin core;
plugin math;

let F32 = math.F32;
let R: tensor.Ring = (F32, 0:F32, math.arith.add math.mode.contract, math.arith.mul math.mode.contract);
"""


def read_shapes(path):
    shapes = []
    for line in path.read_text().splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        op, name, *rest = line.split()
        dims, reps = [int(x) for x in rest[:-1]], int(rest[-1])
        shapes.append((op, name, dims, reps))
    return shapes


def conv_out(h, k, s, p):
    return (h + 2 * p - k) // s + 1


def gen_mim(shapes):
    # nets.mim declares the networks' externs itself; see the -I passed to mim.
    lines = [PRELUDE] + (['import nets;'] if any(op == 'net' for op, *_ in shapes) else [])
    for op, name, d, _ in shapes:
        if op == 'gemm':
            m, k, n = d
            lines.append(f'extern fun gemm_{name} (a: «{m}, {k}; F32», b: «{k}, {n}; F32»): «{m}, {n}; F32» = '
                         f'return (tensor.product_2d R (a, b));')
        elif op == 'conv':
            b, c, h, w, o, kh, kw, s, p = d
            oh, ow = conv_out(h, kh, s, p), conv_out(w, kw, s, p)
            lines.append(f'extern fun conv_{name} (x: «{b}, {c}, {h}, {w}; F32», w: «{o}, {c}, {kh}, {kw}; F32»): '
                         f'«{b}, {o}, {oh}, {ow}; F32» = '
                         f'return (tensor.conv R (({s}, {s}), (1, 1), ({p}, {p})) (x, w));')
        elif op == 'net':
            if name not in nets.NETS:
                sys.exit(f'unknown network {name}; nets.mim has {", ".join(nets.NETS)}')
        else:
            sys.exit(f'unknown op {op}')
    return '\n'.join(lines) + '\n'


def gen_inc(shapes):
    out = []
    for op, name, d, reps in shapes:
        if op == 'net':
            continue
        args = ', '.join(str(x) for x in d)
        out.append(f'{op.upper()}({op}_{name}, {args}, {reps})')
    return '\n'.join(out) + '\n'


def gen_nets_inc(shapes):
    rows = [nets.NETS[name].cxx(reps) for op, name, _, reps in shapes if op == 'net']
    return '\n'.join([d for d, _, _ in rows] + [c for _, c, _ in rows] + ['static std::vector<Net> nets = {']
                     + [e for _, _, e in rows] + ['};']) + '\n'


def torch_fn(op, d):
    """The same computation in eager PyTorch, on random inputs of the same shapes."""
    import torch
    import torch.nn.functional as F

    if op == 'gemm':
        m, k, n = d
        a, b = torch.randn(m, k), torch.randn(k, n)
        return lambda: torch.mm(a, b)
    if op == 'conv':
        b, c, h, w, o, kh, kw, s, p = d
        x, wt = torch.randn(b, c, h, w), torch.randn(o, c, kh, kw)
        return lambda: F.conv2d(x, wt, stride=s, padding=p)
    sys.exit(f'unknown op {op}')


def best_ms(fn, reps):
    fn()
    best = float('inf')
    for _ in range(reps):
        t0 = time.perf_counter()
        fn()
        best = min(best, time.perf_counter() - t0)
    return best * 1e3


def torch_ms(shapes):
    import torch

    torch.set_num_threads(1)
    res = {}
    with torch.inference_mode():
        for op, name, d, reps in shapes:
            if op == 'net':
                res[name] = best_ms(nets.NETS[name].torch(), reps)
            else:
                # Python's dispatch costs microseconds the extern does not pay, so the op on one element is taken off.
                one = [1] * (len(d) - 2) + ([1, 0] if op == 'conv' else [1, 1])
                res[name] = best_ms(torch_fn(op, d), reps) - best_ms(torch_fn(op, one), max(reps, 100))
    return res


def check_nets(rows, out):
    """The networks have no scalar reference: their output is compared with PyTorch's, relative to its magnitude."""
    try:
        import numpy as np
        import torch
    except ImportError:
        print('PyTorch and numpy are missing, so the networks stay unchecked', file=sys.stderr)
        return
    with torch.inference_mode():
        for r in rows:
            if r['op'] == 'net':
                want = nets.NETS[r['name']].torch()().numpy().ravel()
                got  = np.fromfile(out / f"{r['name']}.out", dtype=np.float32)
                r['maxerr'] = float(np.abs(got - want).max() / np.abs(want).max())


def cpu_name():
    try:
        for line in Path('/proc/cpuinfo').read_text().splitlines():
            if line.startswith('model name'):
                return line.split(':', 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine()


def run(cmd, **kw):
    print('+', ' '.join(str(c) for c in cmd), file=sys.stderr)
    subprocess.run(cmd, check=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mim', required=True)
    ap.add_argument('--clangxx', default=shutil.which('clang++') or 'clang++')
    ap.add_argument('--shapes', type=Path, default=HERE / 'shapes.txt')
    ap.add_argument('--out', type=Path, default=Path('bench-out'))
    ap.add_argument('--torch', action='store_true', help='time single-threaded eager PyTorch as the reference')
    ap.add_argument('--filter', nargs='*', default=[], help='shape names to run; default all')
    ap.add_argument('--summary', type=Path, help='append the markdown table to this file')
    ap.add_argument('--emit-only', action='store_true', help='stop after mim emitted the LLVM IR')
    args = ap.parse_args()
    args.mim = str(Path(args.mim).resolve()) # mim and clang++ run inside the output directory

    shapes = read_shapes(args.shapes)
    if args.filter:
        shapes = [s for s in shapes if s[1] in args.filter]
    out = args.out / 'cpu'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'bench.mim').write_text(gen_mim(shapes))
    (out / 'shapes.inc').write_text(gen_inc(shapes))
    (out / 'nets.inc').write_text(gen_nets_inc(shapes))

    (out / 'bench.ll').unlink(missing_ok=True)
    mim = [args.mim, '-I', str(HERE), '-p', 'opt', 'bench.mim', '-p', 'll']
    run(mim, cwd=out)
    if args.emit_only:
        return

    # The emitted vector width needs the host's ISA; without it a 16-lane loop falls back to SSE.
    cxx = [args.clangxx, '-std=c++23', '-O2', '-march=native', '-Wno-override-module', '-I.', str(HERE / 'driver.cpp'),
           'bench.ll', '-o', 'bench']
    run(cxx, cwd=out)

    env = dict(os.environ, OMP_NUM_THREADS='1')
    proc = subprocess.run(['./bench'], cwd=out, env=env, check=True, capture_output=True, text=True)
    rows = [json.loads(l) for l in proc.stdout.splitlines() if l.startswith('{')]
    for r in rows:
        r['device'] = 'cpu'
        r['name']   = r['name'].removeprefix(r['op'] + '_')
        if r['maxerr'] < 0:
            r['maxerr'] = None
    if any(r['op'] == 'net' for r in rows):
        check_nets(rows, out)

    if args.torch:
        ref = torch_ms(shapes)
        for r in rows:
            flop = r['gflops'] * r['ms'] * 1e6
            r['ref_ms']     = ref[r['name']]
            r['ref_gflops'] = flop / r['ref_ms'] * 1e-6

    has_ref = any('ref_ms' in r for r in rows)
    head = ['device', 'op', 'shape', 'dims', 'ms', 'GF/s', 'faults']
    head += (['torch ms', 'torch GF/s', 'vs torch'] if has_ref else []) + ['max err']
    table = ['| ' + ' | '.join(head) + ' |', '|' + '---|' * len(head)]
    for r in rows:
        cells = [r['device'], r['op'], r['name'], r['dims'], f"{r['ms']:.3f}", f"{r['gflops']:.1f}",
                 f"{r['faults']:.0f}"]
        if has_ref:
            if 'ref_ms' in r:
                cells += [f"{r['ref_ms']:.3f}", f"{r['ref_gflops']:.1f}", f"{100 * r['ref_ms'] / r['ms']:.0f}%"]
            else:
                cells += ['', '', '']
        cells.append('-' if r['maxerr'] is None else f"{r['maxerr']:.1e}")
        table.append('| ' + ' | '.join(cells) + ' |')
    cpu = cpu_name()
    for r in rows:
        r['cpu'] = cpu
    md = f'CPU: {cpu}\n\n' + '\n'.join(table) + '\n'

    (out.parent / 'bench-cpu.json').write_text(json.dumps(rows, indent=1) + '\n')
    (out.parent / 'bench-cpu.md').write_text(md)
    print(md)
    if args.summary:
        with args.summary.open('a') as f:
            f.write(f'### tensor benchmark (cpu)\n\n{md}\n')
    bad = [r for r in rows if r['maxerr'] is not None and r['maxerr'] > 1e-2]
    if bad:
        sys.exit('mismatch against the reference: ' + ', '.join(r['name'] for r in bad))


if __name__ == '__main__':
    main()
