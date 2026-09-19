Fork of doldecomp/melee, branch reporter, for the tournament menu. Design: ../tournament-reporter/docs/design.md §6.1.
New code lives only in melee/mn/mntourney.c, melee/lb/lbtourney.c, melee/lb/lbrelayexi.c.
Never modify a matched function. The DOL is shifted; matching is not required for new files.
Include relay_proto.h copied from ../tournament-reporter/generated/ into include/; never redefine structs.
No malloc, no string parsing, all buffers static. Follow existing menu file patterns.
Build with ninja. If the build breaks, fix the cause, don't work around it.
