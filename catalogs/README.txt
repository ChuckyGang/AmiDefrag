================================================================================
                      AmiDefrag - Localization (Catalogs)
================================================================================

AmiDefrag uses the AmigaOS locale.library catalog system for translation.

  * All translatable strings live in  AmiDefrag.cd  (the catalog description -
    the single source of truth).
  * The C build generates  ../src/amidefrag_strings.h  from it (string ids +
    the built-in ENGLISH defaults compiled into the program).
  * Translators produce a  <language>.ct  file and compile it into a binary
    <language>/AmiDefrag.catalog.

locale.library only exists on AmigaOS 2.1 (Kickstart V38) and later.  On
Kickstart 2.04 (V37) AmiDefrag runs unchanged and simply shows the built-in
English strings - no catalog is loaded, nothing fails.  The Linux builds
always show the English strings.

What is translated: the window (buttons, status line, legend, requesters)
and the CLI's own output.  What stays English on purpose: the engines'
progress and journal lines ("Moved anode ...", "Journal: ..."), the DEBUG
output and the word YES the CLI asks for before a live run - so bug reports
read the same everywhere and the danger prompt never changes.


--------------------------------------------------------------------------------
 TOOLCHAIN
--------------------------------------------------------------------------------
The standard FlexCat / CatComp tools understand AmiDefrag.cd directly.  For a
self-contained build (no FlexCat installed) a small reproducible generator is
provided: ../support/gencat.py  (used automatically by the Makefile to produce
amidefrag_strings.h).  It also creates templates and compiles catalogs:

  Generate the C header (done automatically by `make`):
    python3 ../support/gencat.py header AmiDefrag.cd ../src/amidefrag_strings.h

  Create a fresh translation template (or `make catalog-template LANG=deutsch`):
    python3 ../support/gencat.py ct AmiDefrag.cd deutsch.ct deutsch

  Compile a finished translation into a binary catalog:
    python3 ../support/gencat.py catalog AmiDefrag.cd deutsch.ct deutsch/AmiDefrag.catalog

FlexCat equivalents (if you prefer the real tools):
    flexcat AmiDefrag.cd NEWCTFILE deutsch.ct
    flexcat AmiDefrag.cd deutsch.ct CATALOG deutsch/AmiDefrag.catalog


--------------------------------------------------------------------------------
 TRANSLATING
--------------------------------------------------------------------------------
1. Generate a template:        make catalog-template LANG=deutsch
   (writes catalogs/deutsch.ct, pre-filled with the English text)
2. Edit deutsch.ct: translate the line under each MSG_ identifier.
   * Keep every printf placeholder (%s %lu %c ...) - same count and order.
     %% is a literal percent sign.
   * Keep the "|" separators in button strings ("... - Defragment|Cancel").
   * Keep the "_" before the keyboard shortcut letter in the button labels
     (_Analyze, _Rescan, ...) and keep those labels short: five buttons
     share one row of a 640-pixel-wide window.
   * CLI table lines (the LIST header and rows, the class rows) use fixed
     column widths; keep the %-Ns widths if you want aligned columns.
   * Strings that end in "\n" are complete CLI lines; keep the newline.
   * The Amiga character set is ISO-8859-1 (Latin-1); save the file as Latin-1.
3. Compile:  make catalog LANG=deutsch   (or the gencat.py command above)


--------------------------------------------------------------------------------
 INSTALLING A CATALOG
--------------------------------------------------------------------------------
Copy the compiled catalog to either of locale.library's search paths:

    LOCALE:Catalogs/deutsch/AmiDefrag.catalog      (system-wide)
    PROGDIR:Catalogs/deutsch/AmiDefrag.catalog     (next to the AmiDefrag binary)

Set your preferred language in the Locale Preferences and relaunch AmiDefrag.


--------------------------------------------------------------------------------
 EDITING AmiDefrag.cd (developers)
--------------------------------------------------------------------------------
  * ALWAYS append new messages at the END of their section, never insert in the
    middle - string ids are positional and must stay stable so existing
    translated catalogs keep matching.
  * Encode line breaks as \n, tabs as \t, embedded quotes as \", backslash as \\,
    other Latin-1 characters as \xNN.
  * One logical string per message (use \n for internal newlines).
  * Run `make strings` (or just `make`) after editing to regenerate the header.
================================================================================
