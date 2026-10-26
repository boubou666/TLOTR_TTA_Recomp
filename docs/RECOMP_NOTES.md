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

## Runner build

- With full debug info the runner was ~3 GB (2.8 GB of DWARF for the generated code) and Windows refuses to load it
  ("Exec format error"). `CMakeLists.txt` moves the DWARF to `ps2EntryRunner.debug` after each link (136 MB exe left).
- Build time (16 threads, 34 GB): a full rebuild went from 55+ min to ~12 min, an edit to `runtime/*.cpp` takes ~80 s.
  - `register_functions.cpp` is one constructor of ~214,000 stores: 35 min at `-O2 -g`, alone at the end of every full
    build. It is built outside the unity batches at `-O0 -g0`.
  - The generated code gets line tables only (`-g1`, the runtime library keeps `-g`): 507 MB of DWARF instead of
    2.8 GB, faster batches, link and split.
  - `runtime/*.cpp` is outside the unity batches. Source properties need `TARGET_DIRECTORY ps2EntryRunner`: the target
    is created in the submodule, and properties set from this directory alone are silently ignored.
  - `rsync -rc` of the 48,800 generated files took 5-6 min on MSYS2; `tools/sync_tree.py` (content hashes in
    `build/generated.manifest`) does it in seconds.
  - Fork 281f7d9: generated sources declare only the functions they call by name (84 files of 48,810) instead of
    including the 4.2 MB `ps2_recompiled_functions.h`, so new entry points or stubs rebuild only the changed files.
    An edit to a runtime header still rebuilds all the generated code.

## Boot

1. All 8 IRX load. The EE then spins at 0x00226D60-0x00226D78 (`FUN_00226bd0`, EA's SIF handshake): it sends its
   buffer addresses to the IOP as SIF software registers 0x1F/0x1E (system command SET_SREG, cid 0x80000001),
   `AUDIOPF.IRX` polls `sceSifGetSreg` (sifcmd ordinal 6) and answers with SET_SREG towards the EE, and the EE polls
   its own SREGs through its sceSifGetSreg (0x0032A250). Fork fix: SET_SREG is handled on both sides without a
   registered handler, and sifcmd ordinals 6/7 read and write the IOP SREGs (test in `iop_emulator_tests`). The
   game's own `sceSifGetSreg@0x0032A250` is stubbed to the runtime's.
2. Past the handshake (GS setup, vsync callback, DMA/GIF/VIF traffic), then
   `[guest-branch:missing-target] JALR 0x198690 -> 0x1C45A0`: a C++ virtual method only reachable through the
   pointer at 0x37EDF4. `tools/find_code_pointers.py` scans `.data`/`.rodata`/`.sdata` for words pointing into
   `.text` at addresses the function table does not have, keeping those that open a stack frame or follow a
   `jr ra`: 497 targets, added as `ptr_*` entry points in the TOML.
3. Past the vtable call, then the indirect call at 0x26CBA4 jumps to 0xFFBF00D0 (the encoding of
   `sd ra, 0xd0(sp)`). 0x26CB80 calls `g_allocator->vtable[0x1C]` with `g_allocator` at 0x378888, set once by
   0x26BA50 to `new Allocator` (8 bytes from EA's own allocator 0x1B8538, vtable 0x37ECD0, slot 0x1C = 0x26B978).
   The first call goes to 0x26B978; a later one finds the object overwritten. `TTA_TRACE_ALLOC=1` logs the object,
   its vtable and the slot whenever they change.
4. The allocator global is 0: `operator new` (0x1B83A8 -> EA's 0x1B8538 -> 0x1BA838 -> 0x2500F8) returned NULL, the
   constructor wrote the vtable to guest address 0, the first call went through `*(0)`, then address 0 was reused.
5. EA's heap (0x1B8478, run by state 1 of the main state machine 0x1C29C8 before state 3 creates the allocator):
   `0x1BD698` gives `0x2000000 - end of BSS`, the heap is one newlib `malloc` of that minus 16 KB, retried 16 KB
   smaller until it fits. The game's `sbrk` (0x328C30) stops at the runtime's `EndOfHeap` (0x1F00000, the runtime
   keeps the top 1 MB): the heap gets 0x1AB31F0 at 0x448E20, and every allocation from it fails, 24 bytes included.
   The allocator is dlmalloc 2.6 (arena 0x3A9790: create 0x24E788, add region 0x250978, memalign 0x2500F8, malloc
   0x24F780; no MORECORE, so requests come from the top chunk, `top->size & ~5` against the request + 16).
6. `TTA_MEMTOP=0x1F00000` (fits EA's first malloc under `EndOfHeap`) gives the same heap size and the same failures:
   not the size. The arena dump looks sane (bins self-linked, top = region base + 0x20, region node linked).

## Open

- Why EA's dlmalloc fails every request: dump the top chunk header (boot 7), then step through 0x24F780.
