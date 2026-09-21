set print pretty on
set confirm off
set pagination off

# Internal helper: dump DOT for a Def*/World* and open it in xdot.
#   $arg0   pointer providing ->dot(file, mim::DotConfig)
#   $arg1   follow_types flag
define _xdot_run
    # see https://stackoverflow.com/a/6889615
    shell echo set \$tmp=\"$(mktemp)\" >/tmp/tmp.gdb
    source /tmp/tmp.gdb
    # mim::DotConfig is {max, all_annexes, follow_types, inline_consts, default_filter, show_hidden};
    call $arg0->dot($tmp, (mim::DotConfig){0xFFFFFFFF, 0, $arg1, 0, 0, 0})
    eval "shell xdot %s 2&> /dev/null &", $tmp
end

define xdot
    if $argc == 0 || $argc >= 3
        help xdot
    end
    set $types = 0
    if $argc > 1
        set $types = $arg1
    end
    _xdot_run $arg0 $types
end

document xdot
xdot
Generates DOT output for the given EXP and invokes xdot.

Usage: xdot EXP [TYPES]
    EXP     Must provide $EXP->dot(file, mim::DotConfig).

    TYPES   Follow type dependencies?
            Default: 0 (no)

Examples:

xdot def        - Show full DOT graph of 'def' but ignore type dependencies.
xdot ref.def_   - As above but on a Ref.
xdot def 1      - As above but follow type dependencies.
end

define xdott
    if $argc == 0 || $argc >= 2
        help xdott
    end
    xdot $arg0 1
end

document xdott
xdott
Generates DOT output for the given argument and invokes xdot while always
following type dependencies.

Usage: xdott EXP

Same as: xdot EXP 1
end

define xdotw
    if $argc == 0 || $argc >= 3
        help xdotw
    end
    set $types = 0
    if $argc > 1
        set $types = $arg1
    end
    _xdot_run $arg0 $types
end

document xdotw
xdotw
Generates DOT output for the given World and invokes xdot.

Usage: xdotw WORLD [TYPES]
    WORLD   Must provide $WORLD.dot(file, mim::DotConfig).

    TYPES   Follow type dependencies?
            Default: 0 (no)

Note:

xdotw expects the address of the World.

Examples:

Show DOT graph of 'world' - ignoring type dependencies.
xdotw &def->world()

Show DOT graph of 'world' including types.
xdotw &def->world() 1
end
