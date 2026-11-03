# Peggle Nights — reverse-engineering workspace

Everything here is derived from the installed Steam build in the parent folder. The installed game files are untouched.
Findings and the multiplayer plan are in [NOTES.md](NOTES.md).

The repository holds only this README, `NOTES.md`, `names.tsv` and `scripts/`. Everything else in the table below is
made from a local copy of the game or downloaded, and stays out of it.

## Layout

| Path | What it is |
|---|---|
| `bin/PeggleNights_inner.exe` | An analysis copy of the game executable (the `popcapgame1.exe` the launcher runs). |
| `decompiled/` | Ghidra pseudo-C of the real game: 12,419 functions. |
| `decompiled/classes/*.c` | Functions grouped by C++ class (RTTI-recovered), e.g. `Sexy_Board.c`, `Sexy_LogicMgr.c`. |
| `decompiled/global/*.c` | Functions not yet attributed to a class, grouped by 64 KB address range (`00460000.c` = 0x460000–0x46FFFF). |
| `decompiled/index.tsv` | Every function: address, size, namespace, name, file, prototype. |
| `decompiled/strings.tsv` | Every string with the functions that reference it. |
| `decompiled/vftables.tsv` | Every vtable, slot by slot. |
| `decompiled/types.h` | Class layouts recovered from RTTI (4,971 types; member names are offsets). |
| `assets/main/`, `assets/z_spring/` | Unpacked `main.pak` and `levelpacks/z_spring.pak`. |
| `ghidra/PeggleNights.gpr` | Analyzed Ghidra project (open it in the Ghidra GUI). |
| `names.tsv` | Hand-identified function/data names. Applied to the project and the export by `scripts/reexport.ps1`. |
| `tools/` | Ghidra 12.2, Temurin JDK 25, a Python venv with Capstone (~1.2 GB; safe to delete and re-download). |

The pseudo-C is for reading and for writing hooks. It is not recompilable source: native C++ cannot be turned back into the original code. The mod is a DLL that hooks the original game.

## Tools

```powershell
# Open the project in the GUI
re\tools\ghidra_12.2_PUBLIC\ghidraRun.bat          # then File > Open Project > re\ghidra\PeggleNights.gpr

# After adding names to re\names.tsv: apply them and regenerate re\decompiled (~30 s, no re-analysis)
powershell -File re\scripts\reexport.ps1

# Callers of functions, following jump stubs (writes re\ghidra\xrefs.txt)
powershell -File re\scripts\xrefs.ps1 56b510 46b6f0
```

```bash
python re/scripts/show.py 0040e460            # print a decompiled function by address
python re/scripts/show.py -n 60 LogicMgr      # ...or by (partial) name, first 60 lines
re/tools/pyenv/Scripts/python re/scripts/disasm.py 0040ae60 20   # raw disassembly at an address
python re/scripts/unpak.py <file.pak> <out_dir>                  # extract a PopCap .pak
```

`scripts/ghidra/` holds the Ghidra scripts: `ExportDecompiled.java` (full export), `ApplyNames.java`, `Xrefs.java`.
The initial analysis was `analyzeHeadless ... -import bin\PeggleNights_inner.exe -postScript RecoverClassesFromRTTIScript.java -postScript ExportDecompiled.java <out>`.
