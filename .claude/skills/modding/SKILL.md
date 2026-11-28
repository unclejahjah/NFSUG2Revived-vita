---
name: modding
description: Port an option of a PC mod for Need for Speed Underground 2 (an ASI such as NFSU2 Extra Options) into the Vita port as a built-in feature, switched by the mod's own PC ini and the start-up screen's Mods tab. Use when asked to add, port, replicate or fix a PC mod feature or mod setting.
---

# Porting a PC mod option to the Vita

The port has no ASI loader. A supported mod option is rebuilt natively: same patches, same conditions, same
default, switched by the same key in the mod's own settings file. Follow the steps in order. The worked example
is EU Car Content, ExtraOptions 5.1.0.1340 `[Gameplay] UnlockRegionalCars`.

Work in a worktree on `development` (never `main`), so the host tests are in reach. Product files and
development-only files (`tests/`, `docs/`, `.claude/`, `AGENTS.md`) go in separate commits. Push only to `dev`.

## 1. Identify the ini

- Find the mod's own settings file and its path relative to the game folder, as the PC mod reads it. An ASI reads
  its ini next to itself, so for Extra Options that's `scripts/NFSU2ExtraOptionsSettings.ini`.
- Pin the exact mod release the port follows: its version resource or release tag. Get the ini shipped with that
  release; don't use a user's edited copy.
- List every key in the ini's order, with the mod's default for each. A mod new to the port gets all its keys in
  `src/launcher/mods_registry.cpp` (`Source`), each `supported = false` until ported.
- One header per mod holds its name, version, author, ini path and the sections and keys the port reads. For
  Extra Options this is `src/ug2/exopts.h`.

## 2. Scope the work: which mod and which options

- Name the mod, its version and the exact keys this piece of work introduces. Everything else in that mod stays
  `supported = false` and is never read or changed.
- Example: EU Car Content brought in only `[Gameplay] UnlockRegionalCars`. The other 63 Extra Options keys
  stayed unsupported.
- If the request doesn't say which keys, or one key's behaviour depends on another, ask before going on.

## 3. Find what the mod changes, and where

- Get the mod's source at the tag of that release. Versions can disagree: Extra Options' tag `v5.1.0.1340` has
  `FILEVERSION 5,1,0,1338` in its `.rc`. Note which one you used.
- Find where the key is read, for example `iniReader.ReadInteger("Gameplay", "UnlockRegionalCars", 1) != 0`.
  - The third argument is the default for a missing section or key, so a missing key means **on** here. The port
    keeps that default only for a value that does not parse: a missing key is vanilla (section 7).
  - `!= 0` is how the value is parsed.
  - Then grep the variable the key lands in (`UnlockRegionals`), not just the key name. An option can act in
    several places: start-up patches, code caves, hotkey or per-frame threads, other options' code.
- Collect **every** change in each guarded block. Look for:
  - `WriteMemory` (both data and code), `MakeNOP`, `MakeCALL`, `MakeJMP` and `MakeRangedNOP`;
  - code caves and naked functions, and the functions they call;
  - the guard conditions, such as file checks and other options;
  - when the change happens: in `DllMain` (before the game's entry point), or later.
- Check the source against the released DLL, because source and binary can differ. If a capture of that release
  exists, every in-scope address must appear in its `patches` or `data_runs`, and nothing else in scope may. For
  Extra Options the capture is `~/Projects/UG2VITA-notes/2026-11-17/extra-options/exopts-5.1.0.1340-analysis.json`.
- Write down a change table. For each change, give:
  - the address;
  - its section (`.text`, `.data` or `.rdata`);
  - the function that contains it;
  - the original bytes and the mod's bytes;
  - what it does.

### Worked example: `UnlockRegionalCars` (Extra Options 5.1.0.1340, `ExtraOptionsStuff.h`)

Guard: the key is on, and `..\CARS\CORSA\` and `..\CARS\PEUGOT106\` each have `TEXTURES.BIN` and
`GEOMETRY.BIN`. The check runs at `DllMain` time.

| Address | Section | Change | What it does | Port |
|---|---|---|---|---|
| 0x7F7C48, 0x7F7C4C | data | byte ← 0xFF | EU-only list ids (CORSA, PEUGOT106) match no car | `modrt::applyPatches` |
| 0x7F7C50, 0x7F7C54 | data | byte ← 0xFF | US-only list ids (CIVIC, RSX) match no car | `modrt::applyPatches` |
| 0x4DFA83 | .text | `call RegionLockHook` (returns 1) | `UICareerCarLot::BuildCarList` region check always passes | none needed (below) |
| 0x4E2374 | .text | `call RegionLockHook` | `UIQRCarSelect::BuildCarList` region check always passes | none needed (below) |
| 0x4FEB9D | .text | `mov ebp, 0x1a` → `0x1c` | `PickRandomCarsOfSomeSort` (0x4FEAF0): AI car list length 26 → 28 | `MOD_SWITCHES` (operand), flag `modrt::euCarsAiList` |
| 0x7F6DBE, 0x7F6DBF | data | 0x00 → 0x07, 0x06 | AI car lists' entries 26 and 27 become CORSA and PEUGOT106 | `modrt::applyPatches` |

- **The two hooks need no code.** 0x5022C0 returns 1 unless the car id is in a region list. With the list ids
  set to 0xFF it always returns 1, and both callers read only `al` before overwriting the other registers.
  Checks like this one must be argued from the disassembly and written into the tag comment.
- **The requirement check is a deviation.** The port checks four files per car, where the mod checks two
  (`TEXTURES.BIN` and `GEOMETRY.BIN`). This is recorded as a deviation (step 6).

## 4. Map each change onto the port

The game's code is translated to C++ ahead of time. **A write into guest `.text` changes nothing at run time.**
Each kind of change has its own home:

- **Data writes** (`.data` or `.rdata`) go in `modrt::applyPatches` (`src/ug2/mods_runtime.cpp`). It runs once,
  after the guest image is mapped and before any game code runs, which is the same point as the mod's `DllMain`.
- **Code edits** go in `MOD_SWITCHES` in `tools/recompiler/games/ug2.py` (`tools/recompiler/disasm/modswitch.py`):
  address, original bytes, the mod's bytes, a C++ flag, and an option. The generator emits the site both ways, as
  `if (flag) { mod } else { original }`, and refuses any site where the original analysis would not stay valid. Define
  the flag in `src/ug2/mods_runtime.cpp` and set it in `applyPatches`. Kinds:
  - *operand*: a same-length edit of an immediate, displacement or scale of one plain instruction, with the same
    mnemonic and the same registers and flags read and written.
  - *value* (option `'value'`): an operand edit whose value comes from the ini. The generated code reads the immediate
    (or a `lea` displacement) from the guest bytes at the address, which `applyPatches` writes as the mod does.
  - *flow*: the mod's bytes decode to other instructions, each inside one original instruction's bytes: a `jcc`
    turned `jmp` or another `jcc` with the same target and flags, NOP runs, `setcc` turned `mov al, 1`, a compare and
    its branch changed together. The generator refuses jumps to new targets, x87, calls, interior jump targets,
    reads of registers or flags the original does not read, flags set here and read after, and dropped register
    writes unless the value is already in the register's local on every path.
  - *call* (option `'call native::name REG... [reads REG...]'`, no mod bytes): a direct call becomes a call of a
    native with the callee's convention, writing the registers listed. The translated caller keeps registers in
    locals and stores to `cpu` only what the original callee reads, so a native reads only its arguments and the
    registers listed after `reads`, which the switch passes in `cpu` for the call. Use it for a code cave that sits next to a call, or for a
    function the mod jumps away from at its entry, when nothing else points at it: switch each caller. The generator
    refuses it when the caller keeps a listed register across the call, reads after the call a register the callee
    always writes and the native does not, or reads flags after it.
  - Regenerate (`tools/build.py` does it when the recompiler changes) and diff the generated code against the
    generation before the change: dropping each switch back to its original lines must give the old code exactly.
    Natives in `NATIVE_OVERRIDES` change their callers' code too (cpu.terminate checks), which is why call switches
    are preferred for caves.
- **Other code changes** (code caves away from a call, rewritten functions) go in a native override of the function
  that contains the address:
  1. Add the function to `NATIVE_OVERRIDES` in `tools/recompiler/games/ug2.py`, with a comment naming its file.
  2. Write `src/ug2/native/sub_<hex>.cpp`. When the feature is off it calls `sub_<hex>_guest` unchanged. When on,
     it does exactly what the patched x86 does.
  3. Follow `docs/native-workflow.md`: scaffold, host spec, shadow check.
  4. Regenerate the code (`dev-loop.sh gen`).
- If the function is **already** a native (it's in `NATIVE_OVERRIDES` or `src/ug2/native/`), the mod change goes
  into that native, behind the feature, and its host spec gains cases with the feature on.
- **Code that runs later** (hotkeys, per-frame threads) becomes native code at the matching point in the game loop.
  The read of the mod's setting stays at start-up.
- **Files the mod checks**: use the paths relative to the game folder (`..\CARS\X` from `scripts/` becomes
  `CARS/X`), with the case the Vita's file system needs.

Write the mapping as the change table plus a "port file and function" column, before writing any code.

## 5. Look for conflicts, and stop if any are found

Check each changed address, function and data item against:

- **The cfg.** Keys in `src/platform/settings.cpp` that control the same feature. A feature is either a cfg key
  (Vita only) or its mod's ini key, never both. List any such cfg key in the registry entry's `overlaps`
  (`cfgOverlap` must return null).
- **Port changes to the game.** These are:
  - natives in `NATIVE_OVERRIDES` and `src/ug2/native/`;
  - widescreen (`src/ug2/widescreen.cpp`);
  - pad tables (`src/ug2/padbind.cpp`, `src/ug2/pad_defaults.cpp`);
  - registry presets (`src/lib/registry.cpp`);
  - the garage stick, and the offload renderer's flush path;
  - anything else that writes the same guest addresses (`git grep` the address in `src/` and `tools/recompiler/`).
- **Other supported mods.** Their change tables, and their keys in `mods_registry.cpp`.
- **The mod's own other options** that touch the same addresses or functions. A ported option must still be right
  when the options it interacts with stay vanilla.

If anything conflicts, **don't pick a resolution yourself**. Tell the user what conflicts with what (addresses,
functions, settings) and what each choice would do, then ask with AskUserQuestion. Options can be: the mod wins
while it's on, the port's change wins, combine both, or leave this option out.

## 6. Replicate the mod exactly

- Use the same guard conditions, the same default for a missing key, the same value parsing, the same addresses
  and values, and the same timing. Don't improve, extend or "fix" the mod's behaviour.
- Deviate only when:
  - the user said that overriding is fine;
  - the mod has a definite bug (show the evidence);
  - or the Vita can't do it the same way (paths, Win32-only calls).
- Record every deviation:
  - in the commit message;
  - in the tag comment (step 8), as `deviation: <why>`;
  - and in the README Mods tab table if a player would notice it.
- Port the whole option. If part of it is left out (as AI opponents were for EU Car Content), say so to the user.
  Don't ship it as complete.

## 7. Tie it to the ini key and the Mods tab

- **One switch.** The feature is switched only by the mod's ini key: no cfg key, and no other ini.
  - No ini means off: the game stays vanilla.
  - An existing ini is honoured as the PC mod reads it, except that a missing key or section leaves the game vanilla
    for that key (user rule, 23 Nov 2026): none of its changes apply, not even the ones the mod makes whatever the
    key says. Values that are present are read as the mod reads them.
  - The game reads the setting at start-up (`readModSettings`). The feature is active only when the ini says on
    and `requirementsMet()` is true; otherwise it stays off and logs `requirementsString()`.
- **Shared setting**, `src/ug2/mods_require.{h,cpp}`: add a `SettingId`, plus a `Setting` that has the mod, name,
  ini path, section and key, the value kind (Bool, Int, Float or Enum with labels), the mod's code default (for a
  value that does not parse), its shipped value, its vanilla value (the ini value whose patches give the game's own
  bytes, found in the executable; empty when there is none), the Mods tab's range and step, and the mod's own
  requirements. Both the game and the launcher read it, through `modreq::parse`, which reads a value as
  the mod's ini reader does (Extra Options: `std::stoi`, base 16 after `0x`, the code default when that fails;
  `atof` for decimals).
- **Changes the mod makes whatever the keys say** (Extra Options' lap limits, drift tables, region, starting cash,
  neon) belong to a key and apply only when the file has that key (`modrt::present()`).
- **Game side**, `src/ug2/mods_runtime.{h,cpp}`: `active()`, the data patches, and any state the natives read.
- **Launcher side**, `src/launcher/mods_registry.{h,cpp}`:
  - add an `Id` and a `Mod` entry;
  - the `description` is quoted from the mod's readme;
  - `comment` is our wording for a line we write;
  - list `overlaps`, and flip the key to `supported = true`.
  - The Mods tab only edits the file. It is typed (bool, int, float or enum, with the mod's range and default),
    stays editable while requirements are missing, and shows them in orange. A row's default and reset value is the
    key's vanilla value; a key without one gets a first choice Vanilla, which leaves the key out (removing its line).
    Without the file Save creates it with every key at its vanilla value, the changed ones at theirs.
- **Docs**: a row in the README's Mods tab table (settings file, key, what it needs), and credit in
  `THIRD_PARTY.md` (mod, authors, licence; behaviour reimplemented, none of its code).
- **Host tests**, run with `bash tests/run_host_tests.sh <dir>`. Use `tests/eucars.cpp` as the pattern:
  - ini on, off, missing, unreadable, and missing the key or section;
  - requirements met and not met;
  - the patch writes exactly the mod's bytes and nothing else.
  - Also extend `tests/mods_cross.cpp` (the registry against the shared settings) and `tests/mods_edit.cpp` (the
    Mods tab edits).

## 8. Tag every change

Every place code changes for the mod carries one tag comment, on one line, in this form:

```
// {Mod name} {mod version} [{Section}] {Key}: {what this line replicates}
// ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars: 0x7F7C48 CORSA list id <- 0xFF
```

- Put it on each patch, each native branch that exists for the mod, each requirement check and each registry
  entry. Inside a function body this is the one comment allowed.
- Log lines use the mod header's constants (`exopts::Name`, `exopts::Version`).

## 9. Verify, then report

- Host tests pass, and a no-mod build behaves as before (the feature is off without the ini).
- Do a short Vita3K run with the ini on and off. Then check the feature on the Vita, everything in the change
  table (for EU Car Content: car lot, quick race car select, AI opponents). Follow the Vita lock rules.
- Report to the user:
  - the change table with the port's status for each row;
  - any deviations;
  - any conflicts and what was decided;
  - what wasn't tested.
