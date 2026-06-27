# CLAUDE.md — CoCo3 / NitrOS-9 / Multi-Vue / MVKit notes (mvedit)

Non-obvious lessons for this app. Most of the platform notes are shared with
[mvdraw](https://github.com/jamieleecho/mvdraw)'s CLAUDE.md; this file keeps the
shared toolchain rules short and focuses on what is specific to a **text editor**
in a scrolling window.

## Toolchain (cmoc + coco-dev Docker)

- Builds run inside `jamieleecho/coco-dev`. The vendored `./coco-dev` wrapper
  uses `docker run -it`, which **fails in a non-TTY/agent shell**. Invoke docker
  directly without `-t`:
  ```
  docker run --rm -v /Users:/home -e HOME=/home/<user> \
    -w /home/<user>/src/mvedit jamieleecho/coco-dev:<ver> make
  ```
- **Every `docker run` is a fresh container.** The bootstrap (clone `cmoc_os9`,
  `make -C mvkit install`) runs on every build. `mvkit/mv_defs.h: No such file`
  means the install step didn't run.
- **cmoc language limits:** 16-bit `int`; 32-bit `long`; **no floating point**;
  **no `<stddef.h>`** (use `0`/`(char *)0`, not `NULL`); strict `?:` pointer
  typing. `struct sgbuf`, `_gs_opt`, `_ss_opt` come from `<fcntl.h>`.
- Verify the module CRC: `os9 ident <disk>,CMDS/mvedit` → **Good**. `make clean`
  before a disk rebuild if you hit "pathname not found" / permission.

## The window: WT_FSWIN (framed, with scroll bars)

- `mv_app_run_with_scrollbars(...)` opens a `WT_FSWIN`. The scroll **arrows** are
  delivered as ordinary menu selections with the window manager's reserved ids
  **`MN_USCRL` / `MN_DSCRL` / `MN_LSCRL` / `MN_RSCRL`** (cgfx.h). They never
  match an app menu row, so they fall through to the **catch-all** entry of the
  `MVMenuItemAction` table (the `{-1,-1,...}` sentinel). mvedit replaces the
  usual no-op sentinel with `unhandled_menu`, which switches on the id and
  scrolls. (`MN_MOVE` / `MN_GROW` also arrive there and are ignored.)
- Set the scroll **thumb** with `_cgfx_ss_sbar(path, horbar, verbar)`. The
  header doesn't say, but the OS-9 Windowing System manual (p.10-64, "Set Scroll
  Marker Positioning") is explicit: the args are **absolute character
  coordinates** of the markers within the scroll regions — rows down from the
  top of the vertical track, columns right from the left of the horizontal one —
  **not** a 0..255 proportional value. Sending 0..255 was the bug: large values
  land far off the track, wrap/clamp, and **paint the marker over the up arrow**
  (the "scroll down blackens the up button" symptom) or hide the thumb. mvedit
  now maps to `0..EDITOR_ROWS-1` / `0..EDITOR_COLS-1` (`SBAR_V_MAX`/`SBAR_H_MAX`
  in `update_scrollbars()`); verified in MAME (thumb at the bottom when scrolled
  to the end, up arrow clean). Tune the maxes if the thumb doesn't reach the
  track ends on hardware.
- The window is 80×25; chrome leaves a **78×23** working area. `mv_set_menus_sized
  (..., 80, 25)` pins the minimum size so the working area never changes.

## Text rendering in the content area

- Text is drawn with `_cgfx_curxy(path, col, row)` + `cwrite(path, s, n)` in 8×8
  **character cells**, relative to the content-area origin. The mouse's
  `pt_wrx`/`pt_wry` are window pixels in the **same** origin, so `col = pt_wrx/8`,
  `row = pt_wry/8` line up with `curxy` (this is the assumption the click→cursor
  mapping rests on — confirmed against MVKit's file dialog, which divides the
  mouse by 8 and uses `curxy` in the same space). Verify on a real run.
- **Auto-scroll on a full write is the trap.** An OS-9 SCF window scrolls when
  the cursor advances past the bottom-right cell. So mvedit:
  - uses the bottom row as a **status line** and writes it only to column
    `EDITOR_COLS-2`, leaving the corner cell `(77, 22)` untouched;
  - writes full-width text rows above it freely — wrapping from the end of row
    *r* to the start of row *r+1* is harmless because an explicit `curxy` at the
    next row cancels the pending wrap before anything scrolls.
  If the window ever scrolls by itself during a repaint, something wrote that
  corner cell.
  - **The same trap applies to a `cwarea`-clipped working area.** The
    `_cgfx_insline`/`_cgfx_delline` paths clip to just the text rows
    (`cwarea(1,1,78,22)`) so the hardware shift won't drag the status line — but
    that makes row `EDITOR_ROWS-1` the *last* row of the active working area, so
    a full-width `cwrite` there scrolls the window just like the real corner
    does. Fix: do the shift while clipped, then **un-clip (`clip_full`) before
    redrawing the changed rows**, so the bottom row is drawn in the full area
    where the wrap off its end is harmless. This was the "Enter at the bottom of
    the screen scrolls" bug; verified fixed in MAME (Enter on the second-to-last
    row no longer scrolls the top line off).
  - **Vertical scrolling uses the same `delline`/`insline` ops, not a full
    redraw.** `vscroll_repaint()` hardware-shifts the overlapping rows and
    redraws only the newly exposed band (~1 row vs all 22), so scroll-bar
    arrows, arrow-key scrolls, and Enter-at-the-bottom are all cheap. Edge
    cases worth remembering: an edit that *also* scrolls needs the dirtied rows
    redrawn on top of the shift (Enter-at-bottom = `delline` + redraw the split
    head & tail); a **backspace-join at the top** needs *no* shift at all —
    removing a line and scrolling up one cancel for every row below the merge,
    so only the merged row 0 is redrawn. Horizontal scrolls and whole-screen
    jumps still fall back to a full redraw (line ops can't shift sideways).
- Reverse-video selection: bracket the selected run with `_cgfx_revon` /
  `_cgfx_revoff` around its `cwrite`, exactly as the file dialog highlights the
  current row. The caret is the hardware text cursor (`_cgfx_curxy` + `_cgfx_curon`).

## Keyboard

- CoCo/NitrOS-9 control codes used here: the **arrow keys move the cursor** —
  **Up `$0C`, Down `$0A`** (confirmed by the file dialog's scroll keys),
  **Right `$09`**, **Left `$08`** (the left/erase key) — **Enter `$0D`**, and
  **Backspace = ESC/BREAK `$05`** (the code the file dialog reads for Escape).
  All key codes are `#define`d at the top of `text_view.c`; **verify against your
  keymap** and adjust. (Verified in MAME: `$08` moves left; the BREAK key,
  injected via the `:row6` "BREAK" matrix field, deletes.)
- The main run loop does **not** disable the keyboard interrupt/abort chars, so
  `BREAK`/`$05` (abort) and `Ctrl-C` (`$03`, interrupt) would raise a signal
  instead of being read. `mvedit_init()` clears `sg_kbich`/`sg_kbach` on
  `MV_INPATH` (as the file dialog does) so `$05` (Backspace here) and the `Ctrl-`
  Edit shortcuts arrive as data.

## App architecture (mirrors mvdraw)

- `textdoc.*` is the framework-agnostic model: a fixed array of NUL-terminated
  lines plus editing primitives (insert/delete char, split/join line, range
  delete, multi-line insert). Because it is a plain fixed-size value, a snapshot
  for **single-level undo** is one struct assignment.
- `text_view.*` owns rendering, the cursor, mouse selection (press =
  `select_press`-style anchor; a drag loop reads `_cgfx_gs_mouse` until
  `pt_cbsa` clears, à la mvdraw's `track_drag`), scrolling, and an internal
  clipboard. It reports edits via `will_change` (app snapshots for undo) and
  `did_change` (app marks dirty + `mv_app_refresh_menubar()`), so the view never
  touches files or the undo stack.
- Dirty tracking is a plain `editor.doc_modified` flag (not MVKit's undo marker),
  set on any change and cleared on save; the New/Open/close save-prompt is
  `confirm_discard()`. This avoids the single-level-undo-vs-undo-marker mismatch
  you would get from `MVDocument`'s multi-level dirty model.

## Memory: the 64 KB process ceiling (fork error 207)

- A NitrOS-9 Level 2 process is mapped into **64 KB total** — the code module
  plus its data area must fit. If `module size + data area > 64 KB`, gshell's
  launch fails with **"Fork error <app> - 207"** (`E$MemFul`, `$CF`). This is
  silent at build time; it only shows when you run the app under Multi-Vue.
- `MEM_SIZE` (AIF, in 256-byte pages) is the **data area** (BSS + stack + heap),
  not "extra" memory. `os9 ident build/<app>,CMDS/<app>` prints **Module size**
  (code) and **Data size** (BSS+globals). Keep `Module size + MEM_SIZE*256` a few
  KB under 65536. mvedit: module ~26 KB, so `MEM_SIZE := 120` (30 KB) fits with
  ~10 KB of stack; `MEM_SIZE := 200` overflowed and 207'd.
- Three whole-document buffers (model + undo snapshot + clipboard) dominate the
  BSS, so `ED_MAX_LINES` × `ED_LINE_SZ` × 3 is the knob to watch.
- **`make` does not track header deps.** Editing `textdoc.h` (e.g. `ED_MAX_LINES`)
  does *not* recompile the `.c` files (app.mk lists only `$(SRCS)` as
  prerequisites). Force it: `rm -f build/<app> && make`. The CRC changing in
  `os9 ident` confirms a real rebuild.

## Headless verification with MAME (this works — use it)

The coco-dev image ships a headless MAME 0.287; you *can* drive the GUI and
screenshot it. ROMs live on the host at `~/Applications/mame/roms` (coco3 needs
`coco3.zip`; the FDC `disk11.rom` it contains is enough — ignore the
`hdbdw3bc3.rom` verifyroms warning and the `jvc_format: track count of 160
unsupported` line, both harmless).

Run pattern (no display; snapshots come from a Lua script):
```
mame coco3 -rompath ~/Applications/mame/roms \
  -video none -sound none -skip_gameinfo -nothrottle -seconds_to_run <N> \
  -ext:fdc:wd17xx:0 525qd -flop1 build/<app>.os9 \
  -snapshot_directory snap -autoboot_script drive.lua
```
- `-video none` still lets `manager.machine.video:snapshot()` write PNGs (to
  `snap/coco3/NNNN.png` by default). `-seconds_to_run` is the hard stop —
  **always set it**, or a hung Lua leaves MAME running forever (then
  `docker kill` the container).
- **Boot is not automatic.** `-autoboot_command 'dos\n'` did *not* boot here;
  post the boot command from Lua instead: at ~3 emulated seconds,
  `mac.natkeyboard:post("DOS\n")`. NitrOS-9 + Multi-Vue take ~**80 s of emulated
  time** to reach the gshell desktop (cheap: MAME runs ~50× real speed with
  `-nothrottle`).
- **Lua gotcha 1:** `emu.add_machine_frame_notifier(fn)` returns a subscription
  you must store in a **global** (`SUB = ...`) or it is GC'd and stops firing
  after the first call.
- **Lua gotcha 2 (the big one):** `mac.time.seconds` is the **integer** seconds
  only — it reads the *same* value for every tick within a second. Schedule
  events with **`mac.time:as_double()`** (fractional). With `.seconds`, every
  sub-second event (button down at X.0, up at X.2) collapses into one tick, so
  the button goes 1→0 before `set_value` runs once and **the click is silently
  dropped**. This is what makes double-clicks flaky and menu-item clicks never
  register — fix the time source and they work.
- Apply input (`JX:set_value`/`BTN:set_value`) **after** the per-tick event loop,
  not before, so an event's update takes effect the same frame.

### Driving the mouse (the CoCo joystick)

Multi-Vue's pointer is the right joystick, read as absolute position:
- `:joystick_rx` / `:joystick_ry` fields "AD Stick X" / "AD Stick Y" (0–1023),
  `:joystick_buttons` field "Right Button 1". **Hold analog values every frame**
  in the notifier (`JX:set_value(tx)`), or the OSD recenters them.
- Calibrated map (this disk, 640×192 desktop): arrow-tip
  `screenX ≈ 102 + (jx-150)*0.57`, `screenY ≈ 52 + (jy-150)*0.15`. At `jy=0` the
  tip sits at ~y8 — just *below* the menu-bar title row, so the joystick can
  **not** reliably click the top menu bar. Drive the app via its icons/content,
  not the gshell menus.
- A drive icon (`/DD`, top-left) **opens on a double-click**; the app icon in the
  opened window **selects on one click, launches on a double-click**. Once the
  Lua clock uses `as_double()`, clicks land reliably.
- **Deterministic launch via the menu** (preferred over the icon double-click):
  open `/DD`, click the app icon to select it, then drive **Files ▸ Open**.
  Menus **open and items activate on mouse-up**; a clean down/up (~0.1 s with
  fractional time) on the **Files** title drops the menu, and the menu stays open
  only while the pointer is **inside the dropdown** (move onto an item — e.g.
  jx≈200 jy≈106 for "Open" on this disk — then click). The desktop's joystick
  map differs from inside the menu, so settle after each move and confirm the
  item is highlighted before clicking. After clicking Open the fork takes several
  emulated seconds; wait before snapshotting.

### What's verified vs not

Confirmed on real (emulated) hardware via the MAME harness above: clean build +
Good CRC, the icon appears in Multi-Vue, the app launches (Files ▸ Open) and
renders its `File/Edit/Help` menu bar, the `WT_FSWIN` scroll-bar frame, the black
paper, and the reverse-video status line; **keyboard text entry inserts** (typing
"Hello W" showed the text and the status updated to "untitled `*` Ln 1 Col 8");
and the **partial-row repaint optimization renders cleanly** (no artifacts). The
fork-error-207 memory bug was found and fixed this way. Note: the run loop's
`sleep(1)` makes key processing slow (~1 char/s under `natkeyboard:post`), which
is a responsiveness limit, not a rendering one. Still unverified: mouse
click-to-position and drag-select inside a launched instance.
