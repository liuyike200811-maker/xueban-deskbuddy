param([switch]$List)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Media.SpeechSynthesis.SpeechSynthesizer, Windows.Media.SpeechSynthesis, ContentType=WindowsRuntime]
$null = [Windows.Media.SpeechSynthesis.SpeechSynthesisStream, Windows.Media.SpeechSynthesis, ContentType=WindowsRuntime]
$voices = [Windows.Media.SpeechSynthesis.SpeechSynthesizer]::AllVoices
if ($List) {
  $voices | Where-Object Language -eq 'zh-CN' | Select-Object Id,DisplayName,Gender | ConvertTo-Json -Compress
  return
}
$utf8 = [Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = $utf8
$job = [Console]::In.ReadToEnd() | ConvertFrom-Json
$synth = [Windows.Media.SpeechSynthesis.SpeechSynthesizer]::new()
try {
  $synth.Voice = $voices | Where-Object Id -like ("*_" + $job.voice + "M") | Where-Object Language -eq 'zh-CN' | Select-Object -First 1
  $asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.IsGenericMethodDefinition -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
  } | Select-Object -First 1
  $operation = $synth.SynthesizeTextToStreamAsync([string]$job.text)
  $task = $asTask.MakeGenericMethod([Windows.Media.SpeechSynthesis.SpeechSynthesisStream]).Invoke($null, @($operation))
  $task.Wait()
  $stream = [System.IO.WindowsRuntimeStreamExtensions]::AsStreamForRead($task.Result)
  try { $stream.CopyTo([Console]::OpenStandardOutput()) } finally { $stream.Dispose() }
} finally { $synth.Dispose() }
