param([string]$Port,[switch]$Prepare,[switch]$ComputerAudio)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$build=Join-Path $root '.build'
New-Item -ItemType Directory -Force $build | Out-Null
$speechDeps=Join-Path $build 'python'
$mcpDeps=Join-Path $build 'mcp-python'
$env:HF_HOME=Join-Path $build 'hf'
$env:PYTHONPATH="$speechDeps;$mcpDeps;$mcpDeps\win32;$mcpDeps\win32\lib"
if ($Prepare) {
  python -m pip install --disable-pip-version-check --target $speechDeps 'faster-whisper==1.2.1' 'pyserial==3.5'
  if ($LASTEXITCODE -ne 0) { throw 'Local speech dependencies could not be installed' }
  python -m pip install --disable-pip-version-check --target $mcpDeps 'mcp==1.30.0'
  if ($LASTEXITCODE -ne 0) { throw 'MCP SDK could not be installed' }
  python -c "from faster_whisper import WhisperModel; WhisperModel('base', device='cpu', compute_type='int8')"
  if ($LASTEXITCODE -ne 0) { throw 'Local Whisper model could not be downloaded' }
}
if (!$env:DESKBUDDY_DEVICE_TOKEN) {
  $tokenFile=Join-Path $build 'device-token'
  if (!(Test-Path -LiteralPath $tokenFile)) {
    [Guid]::NewGuid().ToString('N') | Set-Content -Encoding ascii -LiteralPath $tokenFile
  }
  $env:DESKBUDDY_DEVICE_TOKEN=(Get-Content -Raw -LiteralPath $tokenFile).Trim()
}
$env:HF_HUB_OFFLINE='1'
$env:DESKBUDDY_STT='local'
$env:DESKBUDDY_TTS='windows'
$env:DESKBUDDY_MODEL_PROVIDER='deepseek'
$env:DESKBUDDY_SERIAL=$Port
$env:DESKBUDDY_PC_AUDIO=if($ComputerAudio){'1'}else{'0'}
python -B (Join-Path $root 'backend\server.py')
exit $LASTEXITCODE

