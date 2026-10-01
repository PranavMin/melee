Fork of doldecomp/melee, branch vanilla-module, for the LazyTO kiosk module.
Venue Wiis run stock Melee 1.02. Our code is tournament.bin, loaded by the LazyTO Nintendont fork.
How it works: docs/tournament-module.md and ../tournament-reporter/docs/architecture.md; decisions (R-numbers): ../tournament-reporter/docs/decisions.md.
The module is the TUs listed in TUS in tools/build_module.py: mntourney.c, lbtourney.c, lbrelayexi.c, lbbuttonglyph.c, lbmodule_glue.c, lbwordmark.c, lbcrash.c.
Build: `python configure.py --non-matching` once, then `python tools/build_module.py`.
Hooks into the game live in tools/module_hooks.txt. Vanilla addresses come from config/GALE01/symbols.txt.
include/relay_proto.h is a generated copy from the relay repo (generated/); never hand-edit it, never redefine its structs.
Never modify a matched function. Hook where the behaviour happens.
No malloc, no string parsing, all buffers static. Follow existing menu file patterns.
If the build breaks, fix the cause, don't work around it.
