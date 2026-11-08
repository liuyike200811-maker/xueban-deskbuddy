param([string]$Port='COM18',[switch]$Flash)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$build=Join-Path $project '.build\led-marquee'
$base=Join-Path $env:USERPROFILE 'AppData\Local\Arduino15'
$env:ARDUINO_DIRECTORIES_DATA=$base
$cli=Join-Path $env:USERPROFILE 'AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$sketch=Join-Path $project 'tools\LedMarquee'
$fqbn='esp32:esp32:esp32s3:CDCOnBoot=default,FlashSize=16M,FlashMode=qio,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB'
& $cli compile --fqbn $fqbn --build-path $build $sketch
if($LASTEXITCODE -ne 0){throw 'LED marquee build failed'}
if($Flash){
  & $cli upload -p $Port --fqbn $fqbn --input-dir $build $sketch
  if($LASTEXITCODE -ne 0){throw 'LED marquee upload failed'}
}