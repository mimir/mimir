set print pretty on
set confirm off
set pagination off

define xdot
    if $argc == 0 || $argc >= 4
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
    # see https://stackoverflow.com/a/6889615
    shell echo set \$tmp=\"$(mktemp)\" >/tmp/tmp.gdb
    source /tmp/tmp.gdb
    call $def->dot($tmp, $max, $types)
    eval "shell xdot %s >/dev/null 2>&1 &", $tmp
end

document xdot
xdot
Generates DOT output for the given EXP and invokes xdot.

Usage: xdot EXP [MAX] [TYPES]
    EXP     Must provide $EXP->dot(file, $MAX, $TYPES).

    MAX     Maximum recursion depth while following a Def's ops.
            Default: 0xFFFFFFFF.

    TYPES   Follow type dependencies?
            Default: 0 (no)

Examples:

xdot def      - Show full DOT graph of 'def' but ignore type dependencies.
xdot ref.def_ - As above but on a Ref.
xdot def 3    - As above but use recursion depth of 3.
xdot def 3 1  - As above but follow type dependencies.
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
    if $argc == 0 || $argc >= 4
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
    # see https://stackoverflow.com/a/6889615
    shell echo set \$tmp=\"$(mktemp)\" >/tmp/tmp.gdb
    source /tmp/tmp.gdb
    call $world->dot($tmp, $annexes, $types)
    eval "shell xdot %s 2&> /dev/null &", $tmp
end

document xdotw
xdotw
Generates DOT output for the given World and invokes xdot.

Usage: xdotw WORLD [ANNEXES] [TYPES]
    WORLD   Must provide $WORLD.dot(file, $ANNEXES, $TYPES).

    ANNEXES Include all annexes - even if unused?
            Default: 0 (no)

    TYPES   Follow type dependencies?
            Default: 0 (no)

Note:

xdotw expects the address of the World.

Examples:

Show DOT graph of 'world' - ignoring type dependencies and unused annexes.
xdotw &def->world()

Show full DOT graph of 'world' including types and all annexes.
xdotw &def->world() 1 1
end
