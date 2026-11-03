# Lists callers (following thunks/tail jumps) of the given hex addresses using the saved Ghidra project.
# usage: powershell -File re/scripts/xrefs.ps1 566c50 566be0
$root = Split-Path -Parent $PSScriptRoot
$out = "$root\ghidra\xrefs.txt"
$env:GHIDRA_HEADLESS_MAXMEM = "8G"
& "$root\tools\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat" "$root\ghidra" PeggleNights `
    -process PeggleNights_inner.exe -noanalysis -readOnly `
    -scriptPath "$root\scripts\ghidra" -postScript Xrefs.java $out @args *> "$root\ghidra\xrefs_console.txt"
Get-Content $out
