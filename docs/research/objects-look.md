# Object look: document text and tint

Notes behind `src/object_look_core.h` and `src/object_look.h`: where the engine
keeps the custom texture dictionary and the tint of an object (RDR2 build
1491.50, offline dump).

Document text (letters, notes, telegrams, newspapers, posters, photos)
is a texture dictionary the script puts on the object with
SET_CUSTOM_TEXTURES_ON_OBJECT(object, txd, p2, p3) after
REQUEST_STREAMED_TXD(txd). Build 1491.50:
  native 0xE124889AE0521FCF handler -> its body: the
  handle resolved as an object, then the setter (object, &txd,
  &p2, &p3). The setter releases the old references and stores each hash
  at object +0x420 (txd), +0x424 (p2), +0x428 (p3) only when the TXD
  store has a slot for it, adding a reference (the store's
  entry +8); hash 0 leaves 0.
  SET_OBJECT_TINT_INDEX (0x971DA0055324D033) body calls
  the tint writer (entity, tint): `and [rcx+0xB8], 0xFFFFFE07; and edx, 0x3F;
  shl edx, 3; or [rcx+0xB8], edx`: the tint is bits 3..8 of the dword at
  entity +0xB8 (six readers do `shr 3; and 0x3F` on it), then a redraw.
