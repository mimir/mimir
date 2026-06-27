set print pretty on
set confirm off
set pagination off

define xdot
    if $argc == 0 || $argc >= 6
        help xdot
    end
    set $def = $arg0
    if $argc > 1
        set $max = $arg1
    else
        set $max = 0xFFFFFFFF
    end
    if $argc > 2
        set $types = $arg2
    else
        set $types = 0
    end
    if $argc > 3
        set $inline = $arg3
    else
        set $inline = 0
    end
    if $argc > 4
        set $hide = $arg4
    else
        set $hide = 0
    end
    # see https://stackoverflow.com/a/6889615
    shell echo set \$tmp=\"$(mktemp)\" >/tmp/tmp.gdb
    source /tmp/tmp.gdb
    # mim::DotConfig is {max, annexes, types, inline_consts, hide_default_filter}; annexes is ignored by Def::dot.
    call $def->dot($tmp, (mim::DotConfig){$max, 0, $types, $inline, $hide})
    eval "shell xdot %s 2&> /dev/null &", $tmp
end

document xdot
xdot
Generates DOT output for the given EXP and invokes xdot.

Usage: xdot EXP [MAX] [TYPES] [INLINE] [HIDE]
    EXP     Must provide $EXP->dot(file, mim::DotConfig).

    MAX     Maximum recursion depth while following a Def's ops.
            Default: 0xFFFFFFFF.

    TYPES   Follow type dependencies?
            Default: 0 (no)

    INLINE  Wire up literals, axioms, etc. instead of detaching them into a separate row?
            Default: 0 (no)

    HIDE    Hide a lambda's filter if it still carries its kind's default (ff for cn, tt for direct-style)?
            Default: 0 (no)

Examples:

xdot def        - Show full DOT graph of 'def' but ignore type dependencies.
xdot ref.def_   - As above but on a Ref.
xdot def 3      - As above but use recursion depth of 3.
xdot def 3 1    - As above but follow type dependencies.
xdot def 3 0 1 1 - Compact view: inline shared nodes and hide default filters.
end

define xdott
    if $argc == 0 || $argc >= 3
        help xdott
    end
    if $argc > 1
        set $max = $arg1
    else
        set $max = 0xFFFFFFFF
    end
    xdot $arg0 $max 1
end

document xdott
xdott
Generates DOT output for the given argument and invokes xdot while always
following type dependencies.

Usage: xdott EXP [MAX]

Same as: xdot EXP $MAX 1
end

define xdotw
    if $argc == 0 || $argc >= 6
        help xdotw
    end
    set $world = $arg0
    if $argc > 1
        set $annexes = $arg1
    else
        set $annexes = 0
    end
    if $argc > 2
        set $types = $arg2
    else
        set $types = 0
    end
    if $argc > 3
        set $inline = $arg3
    else
        set $inline = 0
    end
    if $argc > 4
        set $hide = $arg4
    else
        set $hide = 0
    end
    # see https://stackoverflow.com/a/6889615
    shell echo set \$tmp=\"$(mktemp)\" >/tmp/tmp.gdb
    source /tmp/tmp.gdb
    # mim::DotConfig is {max, annexes, types, inline_consts, hide_default_filter}; max is the recursion depth.
    call $world->dot($tmp, (mim::DotConfig){0xFFFFFFFF, $annexes, $types, $inline, $hide})
    eval "shell xdot %s 2&> /dev/null &", $tmp
end

document xdotw
xdotw
Generates DOT output for the given World and invokes xdot.

Usage: xdotw WORLD [ANNEXES] [TYPES] [INLINE] [HIDE]
    WORLD   Must provide $WORLD.dot(file, mim::DotConfig).

    ANNEXES Include all annexes - even if unused?
            Default: 0 (no)

    TYPES   Follow type dependencies?
            Default: 0 (no)

    INLINE  Wire up literals, axioms, etc. instead of detaching them into a separate row?
            Default: 0 (no)

    HIDE    Hide a lambda's filter if it still carries its kind's default (ff for cn, tt for direct-style)?
            Default: 0 (no)

Note:

xdotw expects the address of the World.

Examples:

Show DOT graph of 'world' - ignoring type dependencies and unused annexes.
xdotw &def->world()

Show full DOT graph of 'world' including types and all annexes.
xdotw &def->world() 1 1
end
