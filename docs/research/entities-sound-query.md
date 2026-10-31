# Owner state queries of dynamic entity sounds

Notes behind `src/entity_query.h` (RDR2 build 1491.50, offline dump): how a
dynamic entity sound asks its owner for a child, and why a replay puppet's
saddle and stirrup foley was cut short.

A dynamic entity sound (metadata type 0x10) picks its child when it plays, by
asking the owner's audio entity a chain of names (the dynamic sound calls the
entity's vtable slot +0x68 with the chain, its length and the top-level sound:
kAnswerUse). For every ped, horse and animal that slot is the resolver
(kResolver, in the three holder vtables). It switches on the head of the chain
and reads the owner's live relations: "mount" (0xA07A1994) asks the engine
mount link (kMountLink: component row +0x128, flags +0x10 bit 4, mount +0x178)
and hands the rest of the chain to the mount's own audio entity; the boat and
vehicle heads, the weapon, the clothing caches, the map material and two task
heads read their own state. A replay puppet has none of that state, so every
such chain answers the null sound, the sound has no child, and the engine ends
it 2 to 3 frames later (saddle and stirrup foley cut short).
