# Dot-source this in PowerShell before idf.py:
#   . .\env.ps1
#   idf.py menuconfig

$idf312 = "D:\Espressif\python_env\idf5.5_py3.12_env\Scripts"
$idf311 = "D:\Espressif\python_env\idf5.5_py3.11_env\Scripts"

if (Get-Command deactivate -ErrorAction SilentlyContinue) {
    deactivate
}

$paths = $env:PATH -split ';' | Where-Object {
    $_ -and ($_ -ne $idf311) -and
    (-not $env:VIRTUAL_ENV -or $_ -ne (Join-Path $env:VIRTUAL_ENV "Scripts"))
}
$env:PATH = ($paths -join ';')
Remove-Item Env:VIRTUAL_ENV -ErrorAction SilentlyContinue
Remove-Item Env:IDF_PYTHON_ENV_PATH -ErrorAction SilentlyContinue

if (Test-Path $idf312) {
    $env:PATH = "$idf312;$env:PATH"
    $env:IDF_PYTHON_ENV_PATH = "D:\Espressif\python_env\idf5.5_py3.12_env"
}

$env:IDF_PATH = "D:\Espressif\frameworks\esp-idf-v5.5.4"
$env:IDF_TOOLS_PATH = "D:\Espressif"

if (-not (Test-Path "$env:IDF_PATH\export.ps1")) {
    Write-Error "ESP-IDF not found at $env:IDF_PATH"
    return
}

. "$env:IDF_PATH\export.ps1"

if (Test-Path $idf312) {
    $env:PATH = "$idf312;$env:PATH"
    $env:IDF_PYTHON_ENV_PATH = "D:\Espressif\python_env\idf5.5_py3.12_env"
}

# Espressif's Initialize-Idf.ps1 defines idf.py as a function pinned to 3.11.
# Drop it so PATH's 3.12 python wins.
Remove-Item Function:idf.py -ErrorAction SilentlyContinue
Remove-Item Function:esptool.py -ErrorAction SilentlyContinue

$py = (Get-Command python -ErrorAction SilentlyContinue).Source
Write-Host "Ready."
Write-Host "  python = $py"
Write-Host "  IDF_PYTHON_ENV_PATH = $env:IDF_PYTHON_ENV_PATH"
Write-Host "  cmake = $((Get-Command cmake -ErrorAction SilentlyContinue).Source)"

if ($py -notmatch 'idf5\.5_py3\.12_env') {
    Write-Warning "Expected idf5.5_py3.12_env but got another Python. Close this terminal and run: . .\env.ps1"
}
