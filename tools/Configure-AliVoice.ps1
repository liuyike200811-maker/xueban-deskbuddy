param([string]$Port='COM18',[string]$Voice='longanqian_v3.1')
$ErrorActionPreference='Stop'
$workspace=Read-Host 'Alibaba Model Studio Workspace ID (ws-...)'
if($workspace -notmatch '^ws-[A-Za-z0-9-]+$'){throw 'Invalid workspace ID'}
if($Voice -notmatch '^[A-Za-z0-9_.-]+$'){throw 'Invalid voice'}
$secret=Read-Host 'DashScope API Key' -AsSecureString
$key=[Net.NetworkCredential]::new('', $secret).Password
if(!$key -or $key -match '\s'){throw 'Invalid key'}
$p=[System.IO.Ports.SerialPort]::new($Port,921600)
try {
  $p.Open()
  Start-Sleep -Milliseconds 700
  $p.Write("ALI`t$workspace.cn-beijing.maas.aliyuncs.com`t$key`t$Voice`n")
  $p.ReadTimeout=5000
  do { $line=$p.ReadLine().Trim() } until ($line -like 'ALI_*')
  $line
} finally { $p.Dispose() }
