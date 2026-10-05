"""The PyTorch mirror of nets.mim: each network's parameters in the order of its extern, and its forward pass."""

import math


class Net:
    def __init__(self, name, params, out, flop, forward):
        self.name    = name
        self.params  = params # (shape, scale, bias) per parameter of the extern
        self.out     = out
        self.flop    = flop
        self.forward = forward

    def cxx(self, reps):
        """The extern's declaration, a call through an array of its parameters, and its row in the driver's table."""
        n    = len(self.params)
        decl = f'extern "C" float* {self.name}({", ".join(["float*"] * n)});'
        call = (f'static float* call_{self.name}(float** p) {{ return {self.name}('
                + ', '.join(f'p[{i}]' for i in range(n)) + '); }')
        ps   = ', '.join(f'{{{math.prod(s)}, {i + 1}, {float(sc)!r}f, {float(b)!r}f}}'
                         for i, (s, sc, b) in enumerate(self.params))
        dims = 'x'.join(map(str, self.params[0][0]))
        row  = (f'    {{"net", "{self.name}", "{dims}", call_{self.name}, {{{ps}}}, {math.prod(self.out)}, '
                f'{float(self.flop)!r}, {reps}}},')
        return decl, call, row

    def inputs(self):
        """The parameters as the driver's fill_net fills them."""
        import numpy as np

        res = []
        for i, (s, sc, b) in enumerate(self.params):
            h = (np.arange(math.prod(s), dtype=np.uint64) * 2654435761 + (i + 1) * 40503) & 0xffffffff
            v = ((h >> 16) % 1001).astype(np.float32) / np.float32(1000) - np.float32(0.5)
            res.append((v * np.float32(sc) + np.float32(b)).reshape(s))
        return res

    def torch(self):
        """The forward pass over the parameters the driver passes."""
        import torch
        import torch.nn.functional as F

        ps = [torch.from_numpy(v) for v in self.inputs()]
        return lambda: self.forward(F, iter(ps))


def mlp():
    n, ds = 64, (4096, 2048, 2048, 1000)
    params, flop = [((n, ds[0]), 1.0, 0.0)], 0
    for din, dout in zip(ds, ds[1:]):
        params += [((dout, din), 3 / math.sqrt(din), 0.0), ((dout,), 0.1, 0.0)]
        flop   += 2 * n * din * dout

    def forward(F, p):
        y = next(p)
        for i in range(len(ds) - 1):
            y = F.linear(y, next(p), next(p))
            y = F.relu(y) if i + 2 < len(ds) else y
        return y

    return Net('mlp', params, (n, ds[-1]), flop, forward)


def resnet18():
    n, r = 2, 224
    params, flop = [((n, 3, r, r), 1.0, 0.0)], 0

    def conv(cin, cout, h, k, s):
        nonlocal flop
        params.extend([((cout, cin, k, k), 3 / math.sqrt(cin * k * k), 0.0), ((cout,), 0.2, 1.0), ((cout,), 0.1, 0.0)])
        oh = (h + 2 * (k // 2) - k) // s + 1
        flop += 2 * n * cout * oh * oh * cin * k * k
        return oh

    h, c = conv(3, 64, r, 7, 2) // 2, 64
    for stage, cout in enumerate((64, 128, 256, 512)):
        for b in range(2):
            s  = 2 if stage > 0 and b == 0 else 1
            oh = conv(c, cout, h, 3, s)
            conv(cout, cout, oh, 3, 1)
            if s == 2:
                conv(c, cout, h, 1, 2)
            h, c = oh, cout
    params += [((1000, c), 3 / math.sqrt(c) / (h * h), 0.0), ((1000,), 0.1, 0.0)]
    flop   += 2 * n * c * 1000

    def forward(F, p):
        def conv_bn(y, s, pad):
            w, sc, sh = next(p), next(p), next(p)
            return F.conv2d(y, w, stride=s, padding=pad) * sc.view(1, -1, 1, 1) + sh.view(1, -1, 1, 1)

        y = F.max_pool2d(F.relu(conv_bn(next(p), 2, 3)), 3, 2, 1)
        for stage in range(4):
            for b in range(2):
                s = 2 if stage > 0 and b == 0 else 1
                z = conv_bn(F.relu(conv_bn(y, s, 1)), 1, 1)
                y = F.relu(z + (conv_bn(y, 2, 0) if s == 2 else y))
        return F.linear(y.sum((2, 3)), next(p), next(p))

    return Net('resnet18', params, (n, 1000), flop, forward)


NETS = {net.name: net for net in (mlp(), resnet18())}
