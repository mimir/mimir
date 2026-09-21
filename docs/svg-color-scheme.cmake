# Doxygen embeds an SVG via <object>, and Chrome paints an opaque canvas behind one that doesn't opt into both schemes.
file(READ "${SVG}" CONTENTS)
string(REPLACE "<svg " "<svg style=\"color-scheme: light dark\" " CONTENTS "${CONTENTS}")
file(WRITE "${SVG}" "${CONTENTS}")
