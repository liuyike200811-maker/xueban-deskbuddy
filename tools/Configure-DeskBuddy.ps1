param([string]$Port='COM18')
$ErrorActionPreference='Stop'
$ssid=Read-Host 'Wi-Fi name (blank for offline-only)'
if (!$ssid) { return }
$password=Read-Host 'Wi-Fi password' -AsSecureString
$backend=Read-Host 'Backend URL, e.g. http://192.168.3.10:8765 (blank to use local voice only)'
$token=Read-Host 'Device token (not the model API key)' -AsSecureString
$portHandle=[System.IO.Ports.SerialPort]::new($Port,921600)
$portHandle.Encoding=[Text.UTF8Encoding]::new($false)
try {
  $portHandle.Open()
  $values=@($ssid,[Net.NetworkCredential]::new('', $password).Password,$backend.TrimEnd('/'),[Net.NetworkCredential]::new('', $token).Password)
  if ($values -match "[`r`n`t]") { throw 'Configuration cannot contain tabs or newlines' }
  $portHandle.Write("CFG`t"+($values -join "`t")+"`n")
  $portHandle.ReadTimeout=15000
  $result=$portHandle.ReadLine()
  while ($result -notmatch '^DESKBUDDY_IP ') { $result=$portHandle.ReadLine() }
  $result.Trim()
} finally { $portHandle.Dispose() }
