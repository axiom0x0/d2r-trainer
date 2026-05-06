# d2r-trainer

![D2R Cover](d2r-cover.jpg)

Linux memory trainer for Diablo II: Resurrected running under Wine/Proton.

The full writeup covering how this was built (memory scanning, pattern matching, failed approaches, and god mode): https://axiom0x0.sh/posts/d2r-memory-trainer-part1/

Reads and writes player stats directly in process memory using `process_vm_readv`/`process_vm_writev` calls.

## Build

Requires libreadline-dev, libncurses-dev, and cmake.

```
mkdir build && cd build
cmake ..
make
```

Produces two binaries:
- `d2r-trainer` - readline CLI
- `d2r-trainer-tui` - ncurses interface with live vitals bars

## Usage

Needs permission to read/write another process's memory. Depending on your system's `ptrace_scope`, this may or may not require root:

```
./d2r-trainer-tui
# or if permission denied:
sudo ./d2r-trainer
```

Connect after you're in-game (stat arrays don't exist at character select):

```
> c        # connect to D2R.exe
> s        # show stats
> g        # toggle god mode (HP/Mana guardian)
> R        # rescan if stats go stale
```

## Stats

The trainer finds and tracks the in-memory stat array:

| # | Stat | Notes |
|---|------|-------|
| 1 | Strength | |
| 2 | Energy | |
| 3 | Dexterity | |
| 4 | Vitality | |
| 5 | Stat Points | |
| 6 | Skill Points | |
| 7 | HP | stored shifted <<8 |
| 8 | Max HP | stored shifted <<8 |
| 9 | Mana | stored shifted <<8 |
| 10 | Max Mana | stored shifted <<8 |
| 11 | Stamina | stored shifted <<8 |
| 12 | Max Stamina | stored shifted <<8 |
| 13 | Level | |
| 14 | Experience | |
| 15 | Gold | also written to .d2s save |
| 16 | Gold Stash | also written to .d2s save |

## Commands

```
c          connect/reconnect to D2R
s          print current stat values
g          toggle god mode
R          rescan memory for stat arrays
f<N> <val> freeze stat N at value (e.g. f7 99999)
u<N>       unfreeze stat N
w<N> <val> write stat N in memory
ws<N> <v>  write stat to .d2s save (by display index)
wc<N> <v>  write stat to .d2s save (by raw code)
d          dump raw stat region
q          quit
```

## God Mode

Runs a 1ms guardian thread that monitors all player stat arrays and writes MaxHP back whenever HP drops below max. Also guards Mana. Uses base stat fingerprinting to identify player arrays vs. monsters/mercs.

## Notes

- Stat arrays move in memory periodically. The trainer rescans every 5 seconds when god mode is active and detects stale addresses on the freeze thread.
- Works with Lutris, Steam/Proton, or plain Wine. The process finder looks for `D2R.exe` in /proc.
- Gold writes also update the .d2s save file (with backup) so the value persists across sessions.
- `tools/find-offsets.cpp` is a standalone research tool for tracing pointer chains from a known address back to the PE image. Not needed at runtime.
