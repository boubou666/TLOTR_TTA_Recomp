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

7. Boot 9 (a copy of the generated 0x24F780 with prints, `runtime/tta_probe_dlmalloc.cpp`): at the first malloc the top
   chunk's size is 0, though it was 0x1AB2FD1 right after setup. The runtime's own guest heap (`guestMalloc`, for HLE
   stubs) starts at the SetupHeap base 0x448E10, inside the span TTA's sbrk hands to EA: the HLE `sceGsResetGraph`
   builds its 128-byte GIF packet there and the high word of DISPFB lands on the top chunk's size field. Fork 2d2ae00
   (local): `PS2Runtime::pinGuestHeap`, used by `applyTta` to put the runtime's heap in the 1 MB under the reserved
   area (EndOfHeap then returns 0x1E00000), and SIF command packets from a reserved pool.
8. Boot 11 (pinned heap): EA's heap holds 0x19B31F0 bytes (about 25.7 MB) and its allocations succeed. The boot
   stops at `JALR 0x119DEC -> 0x12B8D0`, a leaf behind alignment nops that the `jr ra` rule missed: the script now
   steps over them (246 more entry points).
9. Boot 12 stops at `JALR 0x1C75CC -> 0x1C62C0`, in code Ghidra never disassembled (317 such gaps, about 39 KB). The
   script now also takes, given `tta_functions.csv`, every pointer target outside all Ghidra functions; the
   recompiler runs such an entry up to the next known function (98 more, 841 in all).
10. Boot 13: first pictures. A ring of particles turns with a garbled line of text (the loading screen), double
   buffered, for the whole run. Once, in a handler on the invocation stack (sp 0x1FFFFF0), `JALR 0x126434 -> 0`:
   0x125FA8 calls a method of the object at 0x3A1728, which 0x1552C0 (state machine, 0x1C2EA8) only sets later.
   The game's VSync handler 0x212638 runs with its hook (0x3F6E54) still 0, so it is not that one.
   `TTA_TRACE_CALLS=1` logs the first calls of the functions around it.
11. Boot 15 (missing-target log with the thread and invocation): `thread=1 invocation=HleCall`. The fork's HLE
   `sceIpuInit` ran guest code at a fixed 0x126428, another game's copy of libipu's static `SetD4_CHCR(1)`; in TTA
   that address is inside 0x125FA8. The helper only writes D4_CHCR = 1 with DMA suspended, so the stub now does that
   (fork ebc558a, with the pinned heap and the log). Boot 16: no missing target left; the loading screen still turns.
   The main state machine (0x1C29C8, byte state at 0x3BA700, table at 0x35FC98 of {enter, update, exit, next})
   sits in state 3 (enter 0x1C3DD8, update 0x1C41F0, which runs the per-frame updates of 0x1C4018), and no file
   is read: the game's libcdvd (0x27C808-0x27EA18) is mostly unnamed recompiled code.
12. Boot 17 (fork ebc558a, `PS2X_STATE_DUMP=15 PS2X_CALL_TRACE=1 PS2X_WATCH=0x3BA700`): same picture. The state
   stays 3; the EE thread waits on vsync in the frame swap (0x2124B0); the only SIF RPC is the pad library's
   (sid 0x80001300, call 0x8000131A, every frame); `sceSifSendCmd` runs about 2900 times a second (EA's sound
   commands to AUDIOPF); no libcdvd or `sceOpen` call at all, so the game has not started loading anything.

## Open

- Why the loading screen never ends: what state 3 waits for, and whether the game's disc reads reach the runtime.
- The text on the loading screen is garbled (font texture or CLUT upload).
- HLE `sceGsExecLoadImage` copies the whole image into a guest packet: images over ~1 MB would not fit the pinned heap.
