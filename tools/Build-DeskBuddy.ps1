param([string]$Port='COM18',[switch]$Flash,[switch]$FlashAudio)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$build=Join-Path $project '.build'
$src=Join-Path $build 'esp-sr-model'
$modelDir=Join-Path $src 'mn7_cn'
New-Item -ItemType Directory -Force $modelDir | Out-Null
$url='https://raw.githubusercontent.com/espressif/esp-sr/master/model'
$files=[ordered]@{
  '_MODEL_INFO_'='a50062a50b3f9eec0bdd583645372c8f34c4b089'
  mn7_data='1a1878645bce5210e179a2e23ae48f1b233a8544'
  mn7_index='923223099556bc40b5b1fbd34624fa97326f67fb'
  vocab='70484041f33289a259e2940576b54f91143e9ed1'
}
foreach ($name in $files.Keys) {
  $path=Join-Path $modelDir $name
  if (!(Test-Path $path)) {
    curl.exe -fsSL --max-time 25 "$url/multinet_model/mn7_cn/$name" -o $path
    if ($LASTEXITCODE -ne 0) { throw "Model download failed: $name" }
  }
  if ((git hash-object -- $path) -ne $files[$name]) { throw "Model hash mismatch: $name" }
}
$packer=Join-Path $build 'pack_model.py'
if (!(Test-Path $packer)) {
  curl.exe -fsSL --max-time 20 "$url/pack_model.py" -o $packer
  if ($LASTEXITCODE -ne 0) { throw 'Could not download official ESP-SR packer' }
}
if ((Get-FileHash $packer).Hash -ne 'FE753AA8E64F6442459362BB71D29EC714CB271E4AA476B5B3807864EA318346') { throw 'ESP-SR packer hash mismatch' }
$bin=Join-Path $build 'model-cn.bin'
python $packer -m $src -o $bin
if ($LASTEXITCODE -ne 0 -or (Get-Item $bin).Length -gt 0x3E0000) { throw 'Model does not fit the model partition' }

$base=Join-Path $env:USERPROFILE 'AppData\Local\Arduino15'
$env:ARDUINO_DIRECTORIES_DATA=$base
$cli=Join-Path $env:USERPROFILE 'AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$sketch=Join-Path $project 'firmware\DeskBuddy'
$out=Join-Path $build 'firmware'
$fqbn='esp32:esp32:esp32s3:CDCOnBoot=default,FlashSize=16M,FlashMode=qio,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB'
& (Join-Path $PSScriptRoot 'Enable-WsDiagnostics.ps1') (Join-Path $build 'arduino-libs/arduinoWebSockets-2.7.2/src/WebSockets.cpp')
& $cli compile --libraries (Join-Path $build 'arduino-libs') --fqbn $fqbn --build-path $out $sketch
if ($LASTEXITCODE -ne 0) { throw 'Firmware build failed' }
if ($FlashAudio -and !$Flash) { throw 'Use -Flash with -FlashAudio' }
if ($Flash) {
  $extra='0xC10000 "'+$bin+'"'
  if ($FlashAudio) {
    $audio=Join-Path $project 'audio'
    if (!(Get-ChildItem $audio -File | Where-Object Extension -in '.pcm','.mp3')) { throw 'Add audio files before using -FlashAudio' }
    $audioOut=Join-Path $build 'audio.bin'
    $imageTool=Join-Path $base 'packages\esp32\tools\mklittlefs\4.0.2-db0513a\mklittlefs.exe'
    & $imageTool -c $audio -p 256 -b 4096 -s 6291456 $audioOut
    if ($LASTEXITCODE -ne 0) { throw 'Audio image build failed' }
    $extra+=' 0x610000 "'+$audioOut+'"'
  }
  & $cli upload -p $Port --fqbn $fqbn --input-dir $out --upload-property "upload.extra_flags=$extra" $sketch
  if ($LASTEXITCODE -ne 0) { throw 'Upload failed' }
}

