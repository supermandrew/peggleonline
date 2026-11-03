# Applies re/names.tsv to the saved Ghidra project and regenerates re/decompiled (no re-analysis).
$root = Split-Path -Parent $PSScriptRoot
$env:GHIDRA_HEADLESS_MAXMEM = "12G"
& "$root\tools\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat" "$root\ghidra" PeggleNights `
    -process PeggleNights_inner.exe -noanalysis `
    -scriptPath "$root\scripts\ghidra" `
    -postScript ApplyNames.java "$root\names.tsv" `
    -postScript ExportDecompiled.java "$root\decompiled" `
    -log "$root\ghidra\reexport.log" -scriptlog "$root\ghidra\reexport_script.log" *> "$root\ghidra\reexport_console.txt"
Select-String -Path "$root\ghidra\reexport_script.log" -Pattern "Applied|Wrote|ERROR|Exception" | ForEach-Object { $_.Line }
