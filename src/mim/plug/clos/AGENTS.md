# clos

- Verify with `cd lit && ./lit ../build/lit -s --filter 'clos/|matrix/|autodiff/'`: matrix and autodiff depend on clos too.
- `ClosConv` and `LowerTypedClos` are `RWPhase`s; state compared against old-world Defs (`LowerTypedClos::lvm_`) must be built with `old_world()`, or hash-consing a new-world extract over a cyclic old-world operand recurses in `Checker::alpha` until the stack overflows.
- A Cn whose domain is a telescope is left unconverted, like a returning continuation: such a Lam must not capture free variables or be used first-class.
- Forcing `-p clos` on all of lit shows about 24 pre-existing crashes; compare against `HEAD` before blaming a change.
