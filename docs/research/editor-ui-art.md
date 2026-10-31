# Editor UI art files

The painted art the editor loads from `RedemptionEditor\ui` next to RDR2.exe
(`editor/ui_art.h`). The code keeps the loading contract; this is the file
list with the size and slicing of each piece.

```text
  edge_ink.png    the panels' worn letterpress edge: white, alpha the edge
                  profile, the inside at the top; 1024x48, tiled along each
                  side, every other tile mirrored (no seam)
  edge_torn.png   torn paper edge: shipped, not drawn
  bar_torn.png    the timeline track, torn on both long edges (tinted)
  bar_red.png     the played part of the timeline (coloured)
  brush_red.png   the red brush stroke behind a title or a selected item:
                  white, tinted; 1024x128, the start (columns 0-64), the
                  body (to 920, stretched) and the frayed tail
  icon_*.png      the timeline's marks: a camera key, the selected key, a
                  key inside a directed shot, a cut (sheriff star), a
                  shake event (spur rowel), the playhead (watch crown);
                  the selected cut and shake (white on a red halo), the
                  clip's start and end (bent nail brackets), the seam
                  between two clips of a project
  menu_*.png, tool_*.png  the menu items' and the editor tools' icons,
                  gold, and *_white on the red brush or a selected row;
                  128x128 (menu_new_project: the projects book with a "+"
                  badge; menu_save_buffer: a film reel with a down-arrow
                  badge; menu_edit_clip and menu_edit_project: the clips
                  film strip and the projects book with a pencil badge;
                  menu_close_project: the book with an "x" badge)
  row_box.png     a list row's box, as the pause menu's settings list: a
                  grainy body with rough edges, white, tinted dark; 1024x64,
                  three slices (32-column caps at their aspect, the body
                  stretched)
  row_ink.png     the hand-drawn ink outline of the selected row (tinted
                  red) or the row under the mouse (tinted grey); 1024x64,
                  three slices (48-column caps), drawn over the row's box
                  grown by kRowInkOut units
                  (tools/ui_art/make_menu_art.py makes the row art,
                  menu_new_project*.png and menu_save_buffer*.png)
  loading_cylinder.png  the busy cue: the cylinder turning one chamber,
                  12 frames of 128x128 side by side (60 degrees, a loop)
  reel_mark.png   the Cylinder Reel at the start of a title banner (white,
                  tinted)
  divider.png     the menus' divider: a worn gold hairline with a diamond
  corner_star.png a gold corner piece for the large panels
  panel_grain.png a grey grain for panel bodies: shipped, not drawn
  cursor_ink.png  the mouse cursor when the editor draws it: a white
                  dry-ink arrow with a dark outline, 128x128, the tip at
                  texel (5, 0) (src/editor_cursor.h InkRect)
```
