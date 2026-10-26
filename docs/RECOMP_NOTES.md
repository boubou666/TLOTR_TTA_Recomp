# Recompilation notes: The Lord of the Rings: The Third Age (NTSC-U, SLUS-21027)

How the port is brought up, every fix and how it was found, and what remains open.

## The disc

- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SLUS_210.27;1`, `VMODE = NTSC`.
- `SLUS_210.27` (SHA-1 `d88ded370592114169fa18f0ff73b9f610a94bd0`): ELF32 R5900, entry `0x00110008`, **stripped** (no
  symbol table, no `.mdebug`). `.text` 0x00110000-0x00348D18, `.vutext` 0x00348D80 (66 KB of VU microcode), 39
  `.DVP.overlay` sections (VU1 microcode overlays), `.data`, `.rodata`, `.bss` up to ~0x449000.
- `MODULES/`: `IOPRP280.IMG`, `LIBSD`, `SIO2MAN`, `SIO2D`, `DBCMAN`, `DS2U_D`, `MCMAN`, `MCSERV`, `AUDIOPF.IRX` (the
  game's sound driver). `ps2net iop-compat` (PS2_Network_Recomp): every import of these modules is provided by the
  fork's IOP kernel.
- `DATA/GAME` (2.3 GB, per-area folders and `.SCP`/`.SAS` scene packs), `DATA/MOVIES` (1.8 GB).

## Function map

- `ps2_analyzer` (fork 9020eda) recognises 102 SDK functions with runtime handlers and 268 library functions without
  handlers from the embedded signature database.
- Ghidra 12.1.4 headless with the Emotion Engine extension and `ExportPS2Functions.java` produces
  `recomp/ghidra/tta_functions.csv` (`scripts/ghidra.sh`, 81 s of auto-analysis): 9,207 functions and 39,138
  executable labels.
- Every analyzer stub lands on a Ghidra function start except `mceIntrReadFixAlign@0x0027CD30` and
  `mceGetInfoApdx@0x0027D120` (memory card library). Ghidra only has labels there, and neither address is inside
  another function: functions Ghidra missed, not mid-function names.
- No `sceVu0*` HLE stubs: the game's own libvu0 runs (Drakengard found HLE libvu0/libm names landing mid-function).
  libm (`sinf`, `cosf`, `sqrtf`, `__ieee754_*`) is stubbed; check it if math goes wrong.

## Recompile

- `ps2_recomp`: 48,345 entries, 48,235 recompiled, 110 stubs, 0 decode failures, 0 errors, 4,892 warnings (almost
  all unresolved `JR`/`JALR` promoted to fallback entries: switch tables and function pointers, which dispatch through
  the function table). 381 MB of C++.
- `ps2_vu_aot`: 38 microcode fragments, 7,921 instruction pairs -> 41 files in `generated/vu1/`.

## To check (from the other ports, boubou666/PS2Recomp unless noted)

- `ps2xIOP/src/modules/dbcman.cpp` only answers the version-check RPC (0x80001363); the game reads the pad through
  libdbc + `DBCMAN.IRX`/`DS2U_D.IRX`, so pad input likely needs the rest of DBCMAN's RPCs.
- Already on main, nothing to do but watch for the symptoms: SQRT.S reads ft (58b5c8b), VU0 macro FMAC flags (9fa7e4e),
  masked DMAC completions stay pending until CIM is re-enabled (02398e0), no thread switch inside interrupt handlers or
  DI sections (b1870f5), MPEG HLE demux lead per game (bea000e, `ps2_stubs::setMpegMaxPicturesAhead`).
- VU1 microcode lives in 2 KB `.DVP.overlay` chunks: VP2S merges chained MPG uploads into one `ps2_vu_aot` fragment
  (not on main yet). Never build the fragments with MSVC `/fp:fast` (-0 becomes +0).
- C++ vtables built at run time show up as missing call targets; VP2S has a fix for `jr ra` empty methods (not on main).

## Open

- First runner build and boot.
