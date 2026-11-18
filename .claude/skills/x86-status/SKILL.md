---
name: x86-status
description: Bring src/mim/plug/x86/AGENTS.md (the native backend's design and living status) in line with the code after a backend change - Status, Rules, Tests, Known gaps, Pitfalls, Next Steps. Use after changing anything under src/mim/plug/x86/, include/mim/plug/x86/, lit/x86/, or the x86 RUN lines, and when asked to update the x86 plan or status.
---

# x86-status

`src/mim/plug/x86/AGENTS.md` is both the design and the status log of the backend; it is only useful while it matches the code.

## Procedure

1. Read the whole file and the backend diff: `git diff HEAD -- src/mim/plug/x86 include/mim/plug/x86 lit/x86 src/pbqp` plus untracked files there, and any lit file whose x86 RUN lines changed.
2. For each change, update the section that owns it; don't append a changelog:
   - *Status*: one bullet per phase or component (`x86.isel`, `x86.schedule`, `Liveness`, `x86.spilling`, `x86.constrain`, `x86.color`, `x86.emit`); describe what it does now, in present tense.
   - Rules and covered operations: the `Rules:` bullet and the `x86.isel` coverage list.
   - `x86.mim` axioms: the list of instructions and types.
   - *Tests*: the `lit/x86/*.mim` list with a parenthetical when the name does not say what it pins; check with `ls lit/x86`.
   - *Known gaps and bugs*: delete what got fixed, add what you found; bump the "as of" date to today.
   - *Pitfalls already hit*: a fixed bug that is a rule for any future backend phase goes here, in one bullet.
   - *Next Steps*: drop what is done.
   - *Verification*: the allowlist count (`grep -rl --include='*.mim' -e '-X x86:o=' lit | grep -v '^lit/x86/' | wc -l`).
3. Keep the decisions (*Decisions*, *Instruction Selection with PBQP*, *Register Allocation*) unless the change overturned one; then rewrite the decision, don't annotate it.
4. Style: one sentence per line, no hard wraps, no "now"/"new"/"renamed" narration, names in backticks, citations as in the existing text.
5. Verify every claim against the code (clangd or grep), not against the diff summary.
6. Show the diff of the file to the user; don't commit.
