# Hats: the loose hat look and the hat chain

Reverse engineering notes behind recording a loose hat's look and fixing the hat chain of story characters (RDR2 build 1491.50, offline dump plus recorded clips). The code keeps the contract and a link here.

## The look of a loose hat (src/hat_look.h)

A hat knocked off a ped is an object of a generic model (0x30762E1A,
0xE6856EC4 and 0x0B8A6250 are the only models seen with origin=hat) that
draws the metaped layer it carries: the drawable, albedo, normal,
material, palette and three tints, the eight values _SET_META_PED_TAG
takes. The bare model built by CREATE_OBJECT has no layer and draws as a
grey octagon. Engine layout:

```text
- _GET_SHOP_ITEM_HAT_COMPONENT (0x7E02E4218D916B94) takes the hat object
  (short_update.c calls it on the object of EVENT_PLAYER_HAT_KNOCKED_OFF
  and EVENT_PLAYER_HAT_EQUIPPED). Its core (kTagCore) accepts only an
  object (entity type byte [+0x30] == 5) with a tag count word [+0x418]
  above 0, takes the tag array [+0x410] and passes tag 0's +0xC, +0x10,
  +0x14, +0x18 and the qword +0x0 to the shop item lookup.
- The ped side calls the same lookup for a ped's layer with the drawable,
  albedo, normal and material (their getters read slots 0, 8, 0x10, 0x18
  of the ped's layer, the order GET_META_PED_ASSET_GUIDS's core returns
  them in) and the packed qword GET_META_PED_ASSET_TINT unpacks: so +0xC
  is the drawable, +0x10 the albedo, +0x14 the normal, +0x18 the material
  and +0x0 the packed palette and tints.
- The packing is _SET_META_PED_TAG's: palette in the high dword, tint0 in
  byte 2, tint1 in byte 1, tint2 in byte 0 (its unpacker reads it the same
  way).
- The object streaming walk steps through the tags by 0x38 (its loop end,
  kTagWalk) and requests each tag's assets from +0x20, +0x24, +0x28 (-1:
  none).
```

A replay editor usually records a loose ped prop the same way: the prop's
hash, texture and anchor in the object's create, whatever path made it.

## The hat chain of a human skeleton (src/hat_chain.h)

Floating hats are recorded that way: a story character's skeleton hangs the
hat from a short chain under the head (Micah: head 94 -> 121 -> 122 -> 123
-> 124, hat group 737..753 on 124; Dutch: head 88 -> 111 -> ... -> 114, hat
group 818 on 114). The engine's animation fits the hat to the head by moving
the chain's first bone (local x 0.16 for Micah, 0.17 for Dutch) only while
the ped is on screen; off screen the animation output leaves that track
unwritten and the pose finalizer takes the bind pose (0.26), 10 cm higher.
The recording copies what the finalizer wrote, so every sample of a ped the
game camera was not looking at carries the high hat.

A hat point is a bone at the RDR2 human skeleton's hat offset from its
parent, (0, +-0.11, -0.09) (seen in three skeletons of three clips: the
base skeleton's own points and the first bone of the hat group). The chain
runs from a hat point's parent up to the head, the first ancestor with
many children (the face bones); a hat point hung on the head itself (the
generic peds' 959-bone layout) has no chain. The chain's top bone (the
head's child) is the one the animation moves; the subtree under it (the
rest of the chain, the hat points and the hat group) follows it.

Measured on recorded clips with ReplayClipTool hats: Micah's hat high in 87% of one, Dutch 93%, Bill 99%; 28 hats never fitted in a whole clip (peds the camera never saw).

