# Starts the game from this install folder the way Steam would (the launcher stub loads steam.dll by name,
# so Steam's folder must be on PATH). Prints the game process id once its window is up.
param([int]$TimeoutSeconds = 40)
$game = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$env:PATH = "C:\Program Files (x86)\Steam;" + $env:PATH
$before = @(Get-Process popcapgame* -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
Start-Process -FilePath "$game\PeggleNights.exe" -WorkingDirectory $game
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $new = Get-Process popcapgame* -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id -and $_.MainWindowHandle -ne 0 }
    if ($new) { $new | ForEach-Object { "pid=$($_.Id) hwnd=$($_.MainWindowHandle) name=$($_.ProcessName)" }; exit 0 }
}
"timeout waiting for the game window"; exit 1
