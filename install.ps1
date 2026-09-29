# Desktop Companion - installazione su Windows (il comando da usare e' nel README).
# Installa Python se manca, scarica l'app da GitHub in %LOCALAPPDATA%\DesktopCompanion,
# installa le librerie, la fa partire con Windows e la avvia.
$ErrorActionPreference = "Stop"
$repo = "AndreaDp271/Companion-Robot"
$dest = Join-Path $env:LOCALAPPDATA "DesktopCompanion"

Write-Host ""
Write-Host "  Desktop Companion - installazione" -ForegroundColor Cyan
Write-Host "  cartella: $dest"
Write-Host ""

function Find-Python {
    foreach ($cmd in @("py", "python")) {
        $c = Get-Command $cmd -ErrorAction SilentlyContinue
        if ($c) {
            try {
                $exe = & $c.Source -c "import sys; print(sys.executable)" 2>$null
                if ($LASTEXITCODE -eq 0 -and $exe -and (Test-Path $exe) -and $exe -notmatch "WindowsApps") { return $exe }
            } catch {}
        }
    }
    return $null
}

# 1) Python
$python = Find-Python
if (-not $python) {
    Write-Host "[1/4] Python non trovato: lo installo con winget..." -ForegroundColor Yellow
    winget install -e --id Python.Python.3.12 --scope user --accept-package-agreements --accept-source-agreements
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "User") + ";" + [Environment]::GetEnvironmentVariable("Path", "Machine")
    $python = Find-Python
    if (-not $python) { throw "Python non installato: installalo da https://www.python.org e rilancia questo comando." }
} else {
    Write-Host "[1/4] Python trovato: $python"
}
$pythonw = Join-Path (Split-Path $python) "pythonw.exe"

# 2) scarica l'app
Write-Host "[2/4] Scarico l'app da GitHub..."
$tmp = Join-Path $env:TEMP ("companion_install_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $tmp | Out-Null
$zip = Join-Path $tmp "companion.zip"
Invoke-WebRequest "https://github.com/$repo/archive/refs/heads/main.zip" -OutFile $zip -UseBasicParsing
Expand-Archive $zip $tmp -Force
$src = Get-ChildItem $tmp -Directory | Select-Object -First 1
# se c'era gia' un'installazione avviata, la chiudo prima di sovrascrivere
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match "Desktop Companion.pyw" } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $dest | Out-Null
Get-ChildItem $src.FullName -Force | ForEach-Object {
    if ($_.Name -ne "config.json") { Copy-Item $_.FullName $dest -Recurse -Force }
}
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue

# 3) librerie
Write-Host "[3/4] Installo le librerie Python (qualche minuto)..."
& $python -m pip install --user --upgrade -q -r (Join-Path $dest "pc\requirements.txt")

# 4) avvio con Windows e avvio
Write-Host "[4/4] Avvio con Windows e avvio dell'app..."
$launcher = Join-Path $dest "Desktop Companion.pyw"
Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "DesktopCompanion" -Value "`"$pythonw`" `"$launcher`""
Start-Process $pythonw "`"$launcher`""

Write-Host ""
Write-Host "  Fatto! Cerca l'icona del robottino vicino all'orologio." -ForegroundColor Green
Write-Host "  Collega il robot o il Nesso N1 via USB: si accendono da soli."
Write-Host ""
