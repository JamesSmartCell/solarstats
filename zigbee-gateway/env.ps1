# Dot-source this in PowerShell before idf.py:
#   . .\env.ps1
#   idf.py menuconfig

# Drop any previously activated venv (e.g. idf5.5_py3.11_env)
if (Get-Command deactivate -ErrorAction SilentlyContinue) {
    deactivate
}
if ($env:VIRTUAL_ENV) {
    $venvScripts = Join-Path $env:VIRTUAL_ENV "Scripts"
    $paths = $env:PATH -split ';' | Where-Object { $_ -and ($_ -ne $venvScripts) }
    $env:PATH = ($paths -join ';')
    Remove-Item Env:VIRTUAL_ENV -ErrorAction SilentlyContinue
    Remove-Item Env:IDF_PYTHON_ENV_PATH -ErrorAction SilentlyContinue
}

$env:IDF_PATH = "D:\Espressif\frameworks\esp-idf-v5.5.4"
$env:IDF_TOOLS_PATH = "D:\Espressif"

if (-not (Test-Path "$env:IDF_PATH\export.ps1")) {
    Write-Error "ESP-IDF not found at $env:IDF_PATH"
    return
}

. "$env:IDF_PATH\export.ps1"

$py = (Get-Command python -ErrorAction SilentlyContinue).Source
Write-Host "Ready."
Write-Host "  python = $py"
Write-Host "  IDF_PYTHON_ENV_PATH = $env:IDF_PYTHON_ENV_PATH"
Write-Host "  cmake = $((Get-Command cmake -ErrorAction SilentlyContinue).Source)"

if ($py -notmatch 'idf5\.5_py3\.12_env') {
    Write-Warning "Expected idf5.5_py3.12_env but got another Python. Close this terminal and run: . .\env.ps1"
}
