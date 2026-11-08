param([Parameter(Mandatory)][string]$Path)
$source = [IO.File]::ReadAllText($Path)
$headerPath = Join-Path (Split-Path -Parent $Path) 'WebSockets.h'
$header = [IO.File]::ReadAllText($headerPath)
$old = '#define WEBSOCKETS_MAX_DATA_SIZE (15 * 1024)'
$index = $header.IndexOf($old)
if (!$header.Contains('#define WEBSOCKETS_MAX_DATA_SIZE (64 * 1024)')) {
    if ($index -lt 0) { throw 'ESP32 WebSocket frame limit not found' }
    $header = $header.Remove($index, $old.Length).Insert($index, '#define WEBSOCKETS_MAX_DATA_SIZE (64 * 1024)')
    [IO.File]::WriteAllText($headerPath, $header)
}
$alloc = 'payload = (uint8_t *)malloc(header->payloadLen + 1);'
if ($source.Contains($alloc)) {
    $source = "#include <esp_heap_caps.h>`n" + $source.Replace($alloc,
        'payload = (uint8_t *)(header->payloadLen > 15 * 1024 ? heap_caps_malloc(header->payloadLen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : malloc(header->payloadLen + 1));')
    [IO.File]::WriteAllText($Path, $source)
}
if ($source.Contains('ALI_WS_LOCAL_CLOSE')) { return }
$points = [ordered]@{
  '    if(client->status == WSC_CONNECTED && code) {' = '    Serial.printf("ALI_WS_LOCAL_CLOSE code=%u\n", unsigned(code));'
  '    if(header->payloadLen > WEBSOCKETS_MAX_DATA_SIZE) {' = '        Serial.printf("ALI_WS_FRAME_REJECT bytes=%lu limit=%u\n", (unsigned long)header->payloadLen, unsigned(WEBSOCKETS_MAX_DATA_SIZE));'
  '            case WSop_close: {' = '                Serial.printf("ALI_WS_PEER_CLOSE code=%u bytes=%lu\n", header->payloadLen >= 2 ? unsigned(payload[0] << 8 | payload[1]) : 0, (unsigned long)header->payloadLen);'
  '        if((millis() - t) > WEBSOCKETS_TCP_TIMEOUT) {' = '            Serial.println("ALI_WS_IO_TIMEOUT");'
}
foreach ($point in $points.GetEnumerator()) {
    if (!$source.Contains($point.Key)) { throw "WebSocket diagnostic insertion point not found: $($point.Key)" }
    $source = $source.Replace($point.Key, $point.Key + "`n" + $point.Value)
}
[IO.File]::WriteAllText($Path, $source)
