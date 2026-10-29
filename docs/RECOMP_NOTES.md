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
  - Fork 373bc18 (VP2S, on main as 3c04393): generated code includes only `ps2_runtime_macros.h` and
    `ps2_recomp_calls.h` (over `ps2_recomp_abi.h`), not `ps2_runtime.h`. The fork's `ps2EntryRunner` PCH still listed
    `ps2_runtime.h`, `ps2_syscalls.h` and `ps2_stubs.h`, so on TTA (and JC2) a runtime header edit still rebuilt every
    generated file; fork branch `tlotr/runner-pch` makes the runner PCH those two light headers.
- What VP2S does for its build (VP2S_Recomp `port/CMakeLists.txt`, 2026-10-28, a runtime header edit ~20 s instead of
  13-15 min), for TTA to take up:
  - PCH = the two light headers above.
  - `UNITY_BUILD_MODE GROUP`, groups by guest address (`VP2S_UNITY_RANGE_SHIFT` 13: 8 KiB of guest code per group,
    ~470 groups): one changed function rebuilds one group, a new file shifts no other batch. TTA still uses the fork's
    BATCH mode (32 files in name order).
  - Generated code at `/O2 /Ob1` (MSVC); runtime `/GL`, link `/LTCG:INCREMENTAL /CGTHREADS:16 /OPT:REF /OPT:ICF
    /INCREMENTAL:NO` (relink ~2.5 min to seconds). TTA builds with MinGW GCC, which has no incremental LTO link.
  - VU1 AOT fragments in their own target and job pool, unity off (TTA does this too). No ccache.
  - Pitfalls: never switch branches in the build tree (a checkout rewrites header mtimes: full rebuild; build from a
    worktree and check out commits by hash), a PUBLIC compile definition on `ps2_runtime` forces a full rebuild, and a
    `file(GLOB)` of module directories needs `CONFIGURE_DEPENDS`.

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
13. The loading screen is EA's UStream (`SPECIF/UStream.c`, request queue 0x3F2A48, wait loop 0x1D6880 drawing
   frames through its callback). Its requests go to EA's AUDIOPF.IRX by SIF commands (IOP handlers 0x627FC/0x6282C
   wake a worker that answers with SIF command 0 to the EE handler 0x226B60). AUDIOPF builds each disc path with
   sysclib `sprintf("\%s\%s.%s;1", dir, name, ext)`; the IOP's sprintf copied the format literally, so every
   `sceCdSearchFile` failed (256 tries per file, 144 files). Fork ef79b6a formats the varargs (and logs a missed
   search once per name). Boot 25: the files are found and read, the loading screen ends.
14. Boots 26-32: a dark screen follows, the same in every frame, with the CPU and the paraLLEl-GS renderer alike.
   The game is not stuck: the main loop runs every frame (frame limiter 0x1C36C8 spins on the vsync counter
   0x3F65B4), VU1 runs 16k microprograms per dump, the pad works (SIO2 trace: DS2U_D sets the DualShock 2 in
   pressure mode and polls it) and scripted START/CROSS presses change nothing. A GS dump (`PS2X_GS_DUMP_AT`)
   shows full-screen 8x8 sprite passes into the off-screen buffers 256 and 384, and the 3D strips drawn to the
   display buffer have every Y at 0x7FFF and X at the guard band (1535.9/2559.9): their vertices are projected
   off screen. They are built on the EE (0x218FB8 and 0x218910: FPU and VU0 macro code through the scratchpad).
   `PS2X_VU0_CHECK=1` finds no compiled/interpreted VU0 difference.
15. Boots 33-41: the strips came from VU1 (XGKICK of an AOT-compiled microprogram), not from the EE. The EE side
   checks out: the mesh renderer 0x2C9A08 multiplies its matrices with the VU0 microprogram at 0xA80 (probe: the
   product is exact) and 0x218FB8 only draws a full-screen quad. With `PS2X_VU_AOT=0` (VU1 interpreter) the loading
   screen shows the One Ring with its inscription and a readable line of text, then the memory card check box;
   `PS2X_VU1_EXACT_FLAGS=1` with AOT on does the same, so the AOT observer analysis misses one of TTA's MAC/status
   flag readers and skips flags it needs (reported to sc3-port). `applyTta` sets PS2X_VU1_EXACT_FLAGS=1 until then.
   After the memory card check the screen goes black (no movie call, no further disc read).
16. Boots 42-52: after the memory card check the save manager waited forever in `kState_STALL_FOR_UI`: its timer
   (0x1BE428) reads COP0 Count, which the runtime never advanced. Fork branch `tlotr/cop0-count`: Count and the
   performance counters (TTA reads `$25` 432 times) run with the EE cycle clock. Then the legal screen shows, and
   the boot movies start: TTA feeds `sceMpegAddBs` only from its `sceMpegCbNodata` callback, which the HLE never
   called, and real `sceMpegAddBs` sends `((size + 19) >> 4) << 4` bytes (TTA passes 0xFB0 for 0xFC0-byte blocks).
   Fork branch `tlotr/mpeg-nodata` fixes both. The CPU GS rasterizer draws the movie frames as solid magenta;
   paraLLEl-GS shows them (EA GAMES logo, New Line), so `applyTta` requests paraLLEl-GS. After about 30 movie frames
   the game calls `sceMpegInit` (0x27EA40) from the Nodata callback when its packet queue runs dry, to reset the IPU;
   the HLE stub wipes every stream and callback, so the TOML now leaves `sceMpegInit` to the game's code.
17. Boots 53-54 (2026-10-28, fork main with both branches merged, submodule on `tlotr/native`): EA GAMES and New Line
   play to the end, then a black screen. The Nodata callback 0x1BE698 takes the next packet from its queue
   (0x1BE648); when the queue is empty it calls `sceMpegInit` and returns 0, which is how TTA ends a movie. The HLE
   ignored the result and GetPicture waited forever. Fork `tlotr/mpeg-nodata-end`: a callback that fed nothing and
   returned 0 ends the input (`inputFinished`, `streamEnded`), the pictures left are shown and `sceMpegIsEnd` reports
   the end. Boot 54 reaches the title screen (New Game / Load Game / More); the attract movie plays and returns to it.
   Mind the MPEG HLE's logs: GetPicture's lines stop after 32 on purpose, frame 31 is not where decoding stops.

18. Boots 70-78 (2026-10-29): a pad script through New Game (`PS2X_PAD_SCRIPT`, Cross every 6 s from 55 s) reaches
   in-engine 3D (forest, a Black Rider) at ~15-20 game frames/s on the native defaults, then the game dies on the
   next scene load. The old configuration (paraLLEl-GS, interpreted IOP, EE clock) dies the same way, so it predates
   the native work. Seen: dlmalloc's `malloc_consolidate` (0x2506C0) looping on a fastbin cycle, a malloc bin
   loop (0x24F980), and a virtual call at 0x2B61CC (0x2B6180 walks an array of objects at [this+0xC0], count
   [this+0xC4]) through an object whose vtable word points into code: an object freed and reused while still listed.
   Tools in `runtime/tta_overrides.cpp`: `TTA_HEAP_CHECK=1` (a free of a chunk already on a fastbin: EA's dlmalloc
   marks those with size bit 2), `TTA_HEAP_WATCH=<addr>` (every malloc/free of one block, with the callers),
   `TTA_HEAP_WALK=1` (the whole heap walked around every malloc/free: no failure in boot 78, so the corruption is
   not inside the allocator), `TTA_TRACE_MPEG=1` (GetPicture's buffer is 1024 macroblocks, enough for 512 x 448).
   Fork `tlotr/mpeg-nodata-caller`: the Nodata callback runs on GetPicture's thread (it frees game memory; as a
   queued invocation it could cut into another thread's malloc). Right on its own, not the cause here.
   Boots 79-87: runs are not reproducible even with the EE clock, `PS2X_PAD_CLOCK=ee` and `PS2X_GS_SYNC=1`, but the
   crash always comes ~110-120 s in, during the in-engine intro cinematic (letterboxed forest), not on a load. The
   fastbin check caught `free(0x48)`: a child vector at +0xC0 of an object of the 0x2B6178 / 0x2B65A8 family held
   data 0x38 (the object was already destroyed; 0x2BA268 destroys them, 0x2C123C adds children to them later).
   Pressing nothing after the menus changes nothing. A 10 ms IOP CD completion (the HLE completes in 128 cycles)
   moved the crash to a JAL into code with no function: the scan of every JAL in the executable found 10 targets
   Ghidra and the recompiler missed (leaves after a function's padding), now `jal_*` entries in the TOML. Still open.

## Native PC port (2026-10-28 / 29)

The goal, shared with TMB, VP2S and JC2: no PS2 emulation left. Fork `docs/native.md` (on `native/renderer`) has the
layers. What TTA runs by default now (submodule on `tlotr/native`):

- **GS: GSNativeBackend** (OpenGL, VP2S's `native/gs-backend`), chosen by `applyTta` through
  `PS2Runtime::requestNativeGsRenderer` (fork `tlotr/native-gs-request`; `PS2X_GS_RENDERER` still overrides). No
  paraLLEl-GS. It draws the movies, the title screen and its off-screen passes (a 512 x 448 picture composed from
  targets at FBP 0x1C0 and 0x100). Three fixes were needed: a first draw that is native recorded no GS thread (no GL
  context, null targets), frames narrower than 640 failed the host copy (magenta), both now on `native/gs-backend`;
  and TTA draws its movie pictures with the undefined PSM 0x20, which the front end now reads as PSMCT32 (fork
  `tlotr/gs-unknown-psm`; the CPU renderer showed them magenta).
- **IOP: the eight IRX recompiled to C++** (`scripts/iop_recomp.sh`, the fork's `tools/iop_recomp.py` with pointer
  seeds: `jc2/iop-recomp-scan`, written with JC2): AUDIOPF 394 functions, LIBSD 59, SIO2MAN 49, SIO2D 26, DS2U_D 45,
  MCMAN 199, MCSERV 24, DBCMAN 73, none interpreted. AUDIOPF's SIF handlers (0x627FC, 0x6282C) and worker (0x62858)
  are only passed as pointers, so the tool's entry/export/JAL walk had missed them. `PS2X_IOP_NATIVE` is read per
  module load (fork `tlotr/iop-native-late`) and `applyTta` sets it (=0 for an interpreted A/B).
- **Time: COP0 Count on the host clock** (fork `tlotr/cop0-wallclock`, `PS2Runtime::setCop0WallClock`). TTA does
  not count vblanks: its main loop 0x1C3588 takes the frame step in ms from MFC0 Count (factor 1000 / 294912000) and
  then spins on Count until 1/60 s has passed (0x1C37C0). With Count on the host clock its engine and its limiter run
  on real time with no override of its loop. `TTA_WALLCLOCK=0` gives the emulated EE clock back.
- **Sound and IOP timers on the host clock**: `scripts/run.sh` exports `PS2X_SPU2_WALLCLOCK=1` and
  `PS2X_IOP_WALLCLOCK=1` (the SPU2 reads its switch before the override runs).
- **VU1**: the microprograms run AOT-compiled (C++). `TTA_NATIVE=1` instead runs them on the fork's generic
  `Vu1StreamAdapter` (`native/vu1-stream`, written for TTA; TMB and JC2 adopt it), whose sandbox and GifToNative ignore
  FRAME/ZBUF set inside XGKICK packets: TTA renders off-screen passes that way, so its frame comes out black on the
  GL backend. TTA uses six VU1 programs (0x0, 0x620, 0x1968, 0x1B80, 0x1E68, 0x1EA8); next is translating them.

Boots 55-69 on the way there; boot 69 reaches the title screen with every default and no environment.

## Open

- The CPU GS rasterizer's magenta movie frames (unsupported texture path).
- Fork branches not on main yet: `native/vu1-stream`, `tlotr/mpeg-nodata-end`, `tlotr/runner-pch`; the submodule
  is on `tlotr/native` (native/renderer + main + those three).
- VU1 AOT flag observer: which TTA microprogram reads a flag the analysis treats as unread (sc3-port).
- The text on the loading screen is garbled (font texture or CLUT upload).
- HLE `sceGsExecLoadImage` copies the whole image into a guest packet: images over ~1 MB would not fit the pinned heap.
