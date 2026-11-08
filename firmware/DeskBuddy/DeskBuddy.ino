#include <Arduino.h>
#include <ESP_I2S.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <mp3dec.h>
#include "Control.h"
#include "LedController.h"
#include "AliRealtime.h"
#include "CloudPrompt.h"
#include "DeskDisplay.h"

constexpr unsigned MicRate = 16000, PcmRate = 24000, SpeakerRate = 48000, RecordSeconds = 5;
constexpr unsigned SampleCount = MicRate * RecordSeconds;
enum Job : uint8_t { FocusVoice, RestVoice, ChatVoice, FocusDone, RestDone };
struct Work { Job job; uint32_t revision; Mode mode; uint32_t seconds; };
struct Reply { uint32_t revision; char action[16]; int seconds; };
struct Applied { bool ok; Mode mode; uint32_t seconds; };

Timer timer;
WebServer web(80);
QueueHandle_t alarms, replies, applied;
QueueHandle_t musicCommands;
SemaphoreHandle_t audioLock;
std::atomic<bool> alarmActive{false};
std::atomic<bool> musicPlaying{false};
std::atomic<bool> voicePlayback{false};
std::atomic<PetMood> petMood{PetMood::Idle};
std::atomic<bool> dialogueEnabled{true};
std::atomic<bool> restOvertime{false};
std::atomic<int> speakerVolume{50};
I2SClass speaker(I2S_NUM_1);
bool speakerReady = false;
String backend, token, wifiSsid, wifiPassword;
String aliHost, aliKey, aliVoice;
uint32_t aliRevision = 0;
Preferences networkConfig;
bool networkStorageReady = false;
uint32_t revision = 0;
bool busy = false, fsReady = false;
LedController leds;
DeskDisplay display;
constexpr uint8_t TrigPin = 5, EchoPin = 6;
constexpr uint8_t DialogueButtonPin = 18;
// The generic ESP32-S3 board profile maps its onboard pixel to GPIO48.
constexpr uint8_t OnboardRgbPin = PIN_RGB_LED;
bool buttonRaw, buttonStable;
uint32_t buttonChangedAt = 0, rgbUpdatedAt = 0, dialogueChangedAt = 0;
uint32_t distanceCm = 0;
const char *modeName(Mode m) { return m == Mode::Focus ? "focus" : m == Mode::Rest ? "rest" : "idle"; }

// Existing local assets and cloud PCM are 24 kHz mono; the MAX98357A
// receives 48 kHz stereo with the same sample in both channels.
bool writePcm(I2SClass &speaker, const int16_t *samples, size_t n) {
  int16_t stereo[240 * 4];
  while (n) {
    size_t count = min(n, size_t(240));
    int percent = speakerVolume.load();
    for (size_t i = 0; i < count; ++i)
      stereo[4*i] = stereo[4*i+1] = stereo[4*i+2] = stereo[4*i+3] = int32_t(samples[i]) * percent / 100;
    if (speaker.write(stereo, count * 8) != count * 8) return false;
    samples += count;
    n -= count;
  }
  return true;
}

void playTone(I2SClass &speaker, unsigned hz) {
  int16_t chunk[240];
  for (unsigned i = 0; i < 16; ++i) {
    for (unsigned j = 0; j < 240; ++j)
      chunk[j] = ((i * 240 + j) * hz / (PcmRate / 2)) % 2 ? 550 : -550;
    if (!writePcm(speaker, chunk, 240)) return;
  }
}

bool playPcm(I2SClass &speaker, Stream &source, size_t bytes) {
  int16_t chunk[240];
  if (!bytes || bytes % 2) return false;
  while (bytes) {
    size_t n = min(bytes, sizeof(chunk));
    if (source.readBytes(reinterpret_cast<char *>(chunk), n) != n ||
        !writePcm(speaker, chunk, n / 2)) return false;
    bytes -= n;
  }
  return true;
}

bool playFile(I2SClass &speaker, const char *path) {
  if (!fsReady) return false;
  File file = LittleFS.open(path);
  return file && playPcm(speaker, file, file.size());
}

String musicCommand(int track) {
  if (track < 0 || track > 6) return "{\"ok\":false,\"error\":\"请选择第1到6首\"}";
  char path[24];
  snprintf(path, sizeof(path), "/track%d.mp3", track);
  if (track && (!speakerReady || !fsReady || !LittleFS.exists(path)))
    return "{\"ok\":false,\"error\":\"该曲目不可用\"}";
  xQueueOverwrite(musicCommands, &track);
  restOvertime = false;
  return String("{\"ok\":true,\"queued\":true,\"track\":") + track +
    ",\"category\":\"" + (track == 0 ? "none" : track == 1 || track == 6 ? "encouragement" : "motivation") + "\"}";
}

constexpr int FocusReward = 7, RestOvertime = 8;

String setSpeakerVolume(const char *level) {
  int percent = !strcmp(level, "low") ? 30 : !strcmp(level, "medium") ? 50 :
                !strcmp(level, "high") ? 100 : 0;
  if (!percent) return "{\"ok\":false,\"error\":\"请选择低、中、高档\"}";
  speakerVolume = percent;
  Serial.printf("SPEAKER_VOLUME %s %d%%\n", level, percent);
  return String("{\"ok\":true,\"level\":\"") + level + "\",\"percent\":" + percent + "}";
}

void musicWorker(void *) {
  uint8_t input[3072];
  int16_t decoded[2304], mono[1728];
  int sequence = 0, track = 0;
  for (;;) {
    int command;
    if (xQueueReceive(musicCommands, &command, track ? 0 : portMAX_DELAY) == pdTRUE) {
      sequence = command == FocusReward || command == RestOvertime ? command : 0;
      track = command == FocusReward ? 6 : command == RestOvertime ? 2 : command;
    }
    if (!track) { Serial.println("MUSIC_STOPPED"); continue; }
    char path[24];
    snprintf(path, sizeof(path), "/track%d.mp3", track);
    File file = LittleFS.open(path);
    HMP3Decoder decoder = file ? MP3InitDecoder() : nullptr;
    if (!decoder) {
      Serial.printf("MUSIC_ERROR track=%d open_or_decoder\n", track);
      if (sequence == RestOvertime) restOvertime = false;
      track = 0;
      continue;
    }
    int left = 0;
    unsigned phase = 0;
    bool started = false;
    while (!uxQueueMessagesWaiting(musicCommands)) {
      if (voicePlayback.load() || alarmActive.load()) { delay(10); continue; }
      left += file.read(input + left, sizeof(input) - left);
      if (left < 4) break;
      int sync = MP3FindSyncWord(input, left);
      if (sync < 0) {
        memmove(input, input + left - 3, 3);
        if (!file.available()) break;
        left = 3;
        continue;
      }
      unsigned char *cursor = input + sync;
      int available = left - sync;
      int result = MP3Decode(decoder, &cursor, &available, decoded, 0);
      if (result == ERR_MP3_INDATA_UNDERFLOW) {
        if (!file.available()) break;
        cursor = input + sync;
        available = left - sync;
        if (available == sizeof(input)) { ++cursor; --available; }
      } else if (cursor == input + sync) { ++cursor; --available; }
      memmove(input, cursor, available);
      left = available;
      if (result) continue;
      MP3FrameInfo frame{};
      MP3GetLastFrameInfo(decoder, &frame);
      size_t count = 0;
      for (int i = 0; i < frame.outputSamps; i += frame.nChans) {
        int sample = frame.nChans == 2 ? (int(decoded[i]) + decoded[i + 1]) / 2 : decoded[i];
        phase += PcmRate;
        while (phase >= unsigned(frame.samprate)) {
          phase -= frame.samprate;
          mono[count++] = sample;
        }
      }
      xSemaphoreTake(audioLock, portMAX_DELAY);
      bool ok = writePcm(speaker, mono, count);
      xSemaphoreGive(audioLock);
      if (!ok) { Serial.printf("MUSIC_ERROR track=%d i2s\n", track); break; }
      if (!started) {
        musicPlaying = true;
        Serial.printf("MUSIC_PLAYING track=%d rate=%d channels=%d\n", track, frame.samprate, frame.nChans);
        started = true;
      }
    }
    MP3FreeDecoder(decoder);
    musicPlaying = false;
    Serial.printf("MUSIC_END track=%d decoded=%d\n", track, started);
    if (started && sequence == FocusReward && track == 6) track = 1;
    else if (started && sequence == RestOvertime && restOvertime.load()) track = track == 5 ? 2 : track + 1;
    else {
      if (sequence == RestOvertime) restOvertime = false;
      track = 0;
    }
  }
}

void cue(I2SClass &speaker, Job job) {
  if (job == FocusDone) {
    if (!playFile(speaker, "/focus_done.pcm")) playTone(speaker, 784);
    playFile(speaker, "/focus_music.pcm");
  } else if (job == RestDone) {
    if (!playFile(speaker, "/rest_done.pcm")) playTone(speaker, 523);
  } else {
    const char *path = job == FocusVoice ? "/ask_focus.pcm" : job == RestVoice ? "/ask_rest.pcm" : "/ask_chat.pcm";
    if (!playFile(speaker, path)) playTone(speaker, 660);
  }
}

bool record(I2SClass &mic, int16_t *pcm) {
  int32_t raw[256];
  for (unsigned offset = 0; offset < SampleCount;) {
    unsigned n = min(256u, SampleCount - offset);
    if (mic.readBytes(reinterpret_cast<char *>(raw), n * sizeof(raw[0])) != n * sizeof(raw[0]))
      return false;
    for (unsigned j = 0; j < n; ++j) pcm[offset + j] = static_cast<int16_t>(raw[j] >> 16);
    offset += n;
  }
  return true;
}

void alarmWorker(void *) {
  for (;;) {
    Work work;
    xQueueReceive(alarms, &work, portMAX_DELAY);
    xSemaphoreTake(audioLock, portMAX_DELAY);
    alarmActive = true;
    if (speakerReady) cue(speaker, work.job);
    alarmActive = false;
    xSemaphoreGive(audioLock);
  }
}

bool usbVoice(const Work &work, const int16_t *pcm, Reply &reply, uint8_t *&audio, size_t &audioBytes) {
  uint32_t started = millis();
  Serial.printf("VOICE_BEGIN %s %s %lu\n",
                work.job == FocusVoice ? "focus" : work.job == RestVoice ? "rest" : "chat",
                modeName(work.mode), static_cast<unsigned long>(work.seconds));
  Serial.write(reinterpret_cast<const uint8_t *>(pcm), SampleCount * 2);
  while (millis() - started < 60000) {
    if (!Serial.available()) { delay(10); continue; }
    String line = Serial.readStringUntil('\n');
    if (!line.startsWith("RES\t")) continue;
    int sep = line.indexOf('\t', 4);
    if (sep < 0) break;
    String action = line.substring(4, sep);
    Serial.printf("UI usb %lu\n", static_cast<unsigned long>(millis() - started));
    if (action == "error") return false;
    action.toCharArray(reply.action, sizeof(reply.action));
    int audioSep = line.indexOf('\t', sep + 1);
    if (audioSep < 0) return false;
    reply.seconds = line.substring(sep + 1, audioSep).toInt();
    audioBytes = line.substring(audioSep + 1).toInt();
    if (!audioBytes) return true;
    if (audioBytes > 1800000 || audioBytes % 2) return false;
    audio = static_cast<uint8_t *>(heap_caps_malloc(audioBytes, MALLOC_CAP_SPIRAM));
    if (!audio) {
      Serial.println("USB audio allocation failed");
      return false;
    }
    if (Serial.readBytes(reinterpret_cast<char *>(audio), audioBytes) != audioBytes) {
      free(audio);
      audio = nullptr;
      Serial.println("USB reply audio incomplete");
      return false;
    }
    Serial.printf("UI usb_audio %lu\n", static_cast<unsigned long>(millis() - started));
    return true;
  }
  Serial.println("UI usb timeout");
  return false;
}

void voiceWorker(void *) {
  I2SClass mic(I2S_NUM_0);
  mic.setPins(10, 11, -1, 12);
  AliRealtime realtime;
  uint32_t loadedAliRevision = 0;
  bool waitingReported = false;
  for (;;) {
    if (!dialogueEnabled.load()) {
      delay(50);
      continue;
    }
    if (aliKey.isEmpty() || WiFi.status() != WL_CONNECTED) {
      if (!waitingReported) {
        Serial.println("ALI_WAITING_FOR_NETWORK");
        waitingReported = true;
      }
      delay(250);
      continue;
    }
    waitingReported = false;
    if (loadedAliRevision != aliRevision) {
      realtime.close();
      loadedAliRevision = aliRevision;
    }
    busy = true;
    realtime.listen(aliHost, aliKey, aliVoice, CloudPrompt, mic,
            [] { return alarmActive.load(); },
      [](const char *state) {
        voicePlayback = !strcmp(state, "thinking") || !strcmp(state, "speaking");
        petMood = !strcmp(state, "hearing") ? PetMood::Hearing :
          !strcmp(state, "thinking") ? PetMood::Thinking :
          !strcmp(state, "speaking") ? PetMood::Speaking :
          !strcmp(state, "error") ? PetMood::Error : PetMood::Idle;
        leds.signal(state);
      },
      [](const int16_t *samples, size_t n) {
        xSemaphoreTake(audioLock, portMAX_DELAY);
        bool ok = !dialogueEnabled.load() || (speakerReady && writePcm(speaker, samples, n));
        xSemaphoreGive(audioLock);
        return ok;
      },
      [](const char *name, const char *arguments) -> String {
        if (!dialogueEnabled.load()) return "{\"ok\":false,\"error\":\"对话已关闭\"}";
        if (!strcmp(name, "play_encouragement")) return musicCommand(random(2) ? 1 : 6);
        if (!strcmp(name, "set_speaker_volume")) {
          auto *args = cJSON_Parse(arguments);
          auto *level = cJSON_GetObjectItemCaseSensitive(args, "level");
          String result = setSpeakerVolume(cJSON_IsString(level) ? level->valuestring : "");
          cJSON_Delete(args);
          return result;
        }
        if (!strcmp(name, "stop_music")) return musicCommand(0);
        if (!strcmp(name, "play_music")) {
          auto *args = cJSON_Parse(arguments);
          auto *track = cJSON_GetObjectItemCaseSensitive(args, "track");
          int number = !track ? 1 : cJSON_IsNumber(track) && track->valuedouble == track->valueint &&
                       track->valueint >= 1 && track->valueint <= 6 ? track->valueint : -1;
          cJSON_Delete(args);
          return musicCommand(number);
        }
        Reply request{revision, "query", 0};
        if (!strcmp(name, "stop_timer")) strcpy(request.action, "stop");
        else if (!strcmp(name, "start_timer")) {
          cJSON *args = cJSON_Parse(arguments);
          auto *mode = cJSON_GetObjectItemCaseSensitive(args, "mode");
          auto *seconds = cJSON_GetObjectItemCaseSensitive(args, "seconds");
          bool valid = cJSON_IsString(mode) && (!strcmp(mode->valuestring, "focus") || !strcmp(mode->valuestring, "rest")) &&
                       cJSON_IsNumber(seconds) && seconds->valuedouble >= 1 && seconds->valuedouble <= 7200 &&
                       seconds->valuedouble == seconds->valueint;
          if (valid) {
            strcpy(request.action, mode->valuestring);
            request.seconds = seconds->valueint;
          }
          cJSON_Delete(args);
          if (!valid) return "{\"ok\":false,\"error\":\"时长或模式无效\"}";
        } else if (strcmp(name, "get_timer")) return "{\"ok\":false,\"error\":\"未知操作\"}";
        xQueueSend(replies, &request, portMAX_DELAY);
        Applied state{};
        xQueueReceive(applied, &state, portMAX_DELAY);
        return state.ok ? "{\"ok\":true,\"mode\":\"" + String(modeName(state.mode)) +
                           "\",\"seconds\":" + state.seconds + "}" : "{\"ok\":false,\"error\":\"计时状态已变化\"}";
      }, [] { return !dialogueEnabled.load() || WiFi.status() != WL_CONNECTED; });
    busy = false;
    if (dialogueEnabled.load()) delay(1500);
  }
}

const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>学伴桌搭</title><h1>学伴桌搭</h1><p id="state"></p>
<label>时长（分钟） <input id="minutes" type="number" min="1" max="120" value="25"></label>
<button onclick="send('focus')">开始专注</button><button onclick="send('rest')">开始休息</button>
<button onclick="send('stop')">结束计时</button>
<p id="message"></p>
<script>
const state=document.querySelector('#state'), minutes=document.querySelector('#minutes'), message=document.querySelector('#message');
async function refresh(){
  try {
    const s=await (await fetch('/status')).json();
    state.textContent=({idle:'待机',focus:'专注中',rest:'休息中'})[s.mode]+' '+Math.floor(s.seconds/60)+':'+String(s.seconds%60).padStart(2,'0')+(s.busy?' · 语音处理中':'');
  } catch { state.textContent='设备连接中断'; }
}
async function send(action){
  try {
    const r=await fetch('/timer',{method:'POST',body:new URLSearchParams({action,seconds:String(Number(minutes.value)*60)})});
    message.textContent=r.ok?'已更新':'请输入 1 到 120 分钟'; await refresh();
  } catch { message.textContent='无法连接设备'; }
}
refresh(); setInterval(refresh,1000);
</script></html>
)HTML";

bool applyAction(const char *action, int seconds) {
  if (!strcmp(action, "none")) return true;
  if (!strcmp(action, "stop")) {
    if (restOvertime.load()) musicCommand(0);
    timer.stop(); ++revision; return true;
  }
  Mode mode = !strcmp(action, "focus") ? Mode::Focus : !strcmp(action, "rest") ? Mode::Rest : Mode::Idle;
  if (!timer.start(mode, seconds, millis())) return false;
  if (restOvertime.load()) musicCommand(0);
  ++revision;
  return true;
}

bool timerAuthorized() {
  if (!token.isEmpty() && web.header("Authorization") == "Bearer " + token) return true;
  web.send(401, "application/json", "{\"ok\":false}");
  return false;
}

void sendTimerResult() {
  web.send(200, "application/json", "{\"ok\":true,\"mode\":\"" + String(modeName(timer.mode())) +
    "\",\"seconds\":" + timer.remaining(millis()) + ",\"revision\":" + revision + "}");
}

void timerApiWrite() {
  if (!timerAuthorized()) return;
  if (web.hasArg("revision") && web.arg("revision") != String(revision)) {
    web.send(409, "application/json", "{\"ok\":false}");
    return;
  }
  String action = web.arg("action"), seconds = web.arg("seconds");
  if (!((action == "stop" && seconds == "0") ||
        ((action == "focus" || action == "rest") && seconds == String(seconds.toInt()))) ||
      !applyAction(action.c_str(), seconds.toInt())) {
    web.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  sendTimerResult();
}

void setup() {
  Serial.setRxBufferSize(8192);
  Serial.begin(921600);
  pinMode(DialogueButtonPin, INPUT_PULLUP);
  buttonRaw = buttonStable = digitalRead(DialogueButtonPin);
  rgbLedWrite(OnboardRgbPin, 0, 28, 0);
  if (!leds.begin()) Serial.println("LED_RMT_INIT_FAILED");
  fsReady = LittleFS.begin(false, "/littlefs", 10, "spiffs");
  if (!fsReady) Serial.println("No LittleFS audio; using built-in tones");
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
      Serial.printf("DESKBUDDY_WIFI_DISCONNECTED %lu %u\n", static_cast<unsigned long>(millis()), info.wifi_sta_disconnected.reason);
  });
  Serial.setTimeout(1200);
  alarms = xQueueCreate(2, sizeof(Work));
  replies = xQueueCreate(2, sizeof(Reply));
  applied = xQueueCreate(1, sizeof(Applied));
  audioLock = xSemaphoreCreateMutex();
  musicCommands = xQueueCreate(1, sizeof(int));
  pinMode(TrigPin, OUTPUT);
  digitalWrite(TrigPin, LOW);
  pinMode(EchoPin, INPUT);
  if (!display.begin()) Serial.println("OLED_NOT_FOUND");
  speaker.setPins(15, 16, 17);
  speakerReady = speaker.begin(I2S_MODE_STD, SpeakerRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  if (!speakerReady) Serial.println("Speaker I2S init failed");
  if (!alarms || !replies || !applied || !audioLock || !musicCommands ||
      xTaskCreate(musicWorker, "music", 20480, nullptr, 1, nullptr) != pdPASS ||
      xTaskCreate(alarmWorker, "reminders", 4096, nullptr, 2, nullptr) != pdPASS ||
      xTaskCreate(voiceWorker, "audio", 20480, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("Cannot start audio worker");
    while (true) delay(1000);
  }
  const char *requestHeaders[] = {"Authorization"};
  web.collectHeaders(requestHeaders, 1);
  web.on("/api/timer", HTTP_GET, [] { if (timerAuthorized()) sendTimerResult(); });
  web.on("/api/timer", HTTP_POST, timerApiWrite);
  web.on("/api/volume", HTTP_POST, [] {
    if (!timerAuthorized()) return;
    web.send(200, "application/json", setSpeakerVolume(web.arg("level").c_str()));
  });
  web.on("/api/music", HTTP_POST, [] {
    if (!timerAuthorized()) return;
    if (web.arg("category") == "encouragement") {
      web.send(200, "application/json", musicCommand(random(2) ? 1 : 6));
      return;
    }
    String value = web.arg("track");
    int track = value.toInt();
    if (value != String(track) || track < 0 || track > 6) {
      web.send(400, "application/json", "{\"ok\":false}");
      return;
    }
    web.send(200, "application/json", musicCommand(track));
  });
  networkStorageReady = networkConfig.begin("deskbuddy-net", false);
  if (networkStorageReady) {
    aliHost = networkConfig.getString("ali-host", "");
    aliKey = networkConfig.getString("ali-key", "");
    aliVoice = networkConfig.getString("ali-voice", "longanqian_v3.1");
    wifiSsid = networkConfig.getString("ssid", "");
    wifiPassword = networkConfig.getString("password", "");
    if (!wifiSsid.isEmpty()) {
      backend = networkConfig.getString("backend", "");
      token = networkConfig.getString("token", "");
    }
  } else {
    Serial.println("DESKBUDDY_WIFI_STORAGE_UNAVAILABLE");
  }
  web.on("/", HTTP_GET, [] { web.send(200, "text/html; charset=utf-8", PAGE); });
  web.on("/status", HTTP_GET, [] {
    web.send(200, "application/json", "{\"mode\":\"" + String(modeName(timer.mode())) +
      "\",\"seconds\":" + timer.remaining(millis()) + ",\"busy\":" + (busy ? "true" : "false") +
      ",\"dialogue_enabled\":" + (dialogueEnabled.load() ? "true" : "false") +
      ",\"distance_cm\":" + (distanceCm ? String(distanceCm) : String("null")) + "}");
  });
  web.on("/timer", HTTP_POST, [] {
    String action = web.arg("action"), seconds = web.arg("seconds");
    bool known = action == "focus" || action == "rest" || action == "stop";
    bool valid = known && (action == "stop" || (seconds.length() && seconds.length() <= 4 && seconds == String(seconds.toInt())));
    bool ok = valid && applyAction(action.c_str(), seconds.toInt());
    web.send(ok ? 200 : 400, "text/plain", ok ? "OK" : "Invalid timer action");
  });
  Serial.println("DESKBUDDY_READY");
}

void loop() {
  if (!busy && Serial.available()) {
    String line = Serial.readStringUntil('\n');
    if (line.startsWith("ALI\t")) {
      int hostEnd = line.indexOf('\t', 4);
      int keyEnd = line.indexOf('\t', hostEnd + 1);
      if (hostEnd > 4 && keyEnd > hostEnd + 1 && line.length() > keyEnd + 1) {
        aliHost = line.substring(4, hostEnd);
        aliKey = line.substring(hostEnd + 1, keyEnd);
        aliVoice = line.substring(keyEnd + 1);
        ++aliRevision;
        if (networkStorageReady) {
          size_t n = networkConfig.putString("ali-host", aliHost) + networkConfig.putString("ali-key", aliKey) +
                     networkConfig.putString("ali-voice", aliVoice);
          if (n != aliHost.length() + aliKey.length() + aliVoice.length()) Serial.println("ALI_STORAGE_WRITE_FAILED");
        }
        Serial.println("ALI_DIRECT_CONFIGURED");
      }
    } else if (line.startsWith("CFG\t")) {
      int ssidEnd = line.indexOf('\t', 4);
      int passEnd = ssidEnd < 0 ? -1 : line.indexOf('\t', ssidEnd + 1);
      int hostEnd = passEnd < 0 ? -1 : line.indexOf('\t', passEnd + 1);
      if (ssidEnd >= 4 && passEnd > ssidEnd && hostEnd > passEnd) {
        wifiSsid = line.substring(4, ssidEnd);
        wifiPassword = line.substring(ssidEnd + 1, passEnd);
        backend = line.substring(passEnd + 1, hostEnd);
        token = line.substring(hostEnd + 1);
        Serial.println("DESKBUDDY_WIFI_CONFIG_RECEIVED");
        if (!wifiSsid.isEmpty() && networkStorageReady) {
          size_t stored = networkConfig.putString("ssid", wifiSsid);
          stored += networkConfig.putString("password", wifiPassword);
          stored += networkConfig.putString("backend", backend);
          stored += networkConfig.putString("token", token);
          if (stored != wifiSsid.length() + wifiPassword.length() + backend.length() + token.length())
            Serial.println("DESKBUDDY_WIFI_STORAGE_WRITE_FAILED");
        }
        if (!wifiSsid.isEmpty() && (WiFi.status() != WL_CONNECTED || WiFi.SSID() != wifiSsid)) {
          WiFi.disconnect();
          int16_t count = WiFi.scanNetworks(false, true);
          int chosen = -1;
          for (int i = 0; i < count; ++i) {
            if (WiFi.SSID(i) == wifiSsid && (chosen < 0 || WiFi.RSSI(i) > WiFi.RSSI(chosen))) chosen = i;
          }
          if (chosen >= 0) {
            Serial.printf("DESKBUDDY_WIFI_TARGET channel=%ld rssi=%ld\n", static_cast<long>(WiFi.channel(chosen)), static_cast<long>(WiFi.RSSI(chosen)));
            WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str(), WiFi.channel(chosen), WiFi.BSSID(chosen));
          } else {
            Serial.printf("DESKBUDDY_WIFI_TARGET_NOT_SEEN count=%d ssid_bytes=%u\n", count, static_cast<unsigned>(wifiSsid.length()));
            WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
          }
          WiFi.scanDelete();
        }
      }
    }
  }
  uint32_t now = millis();
  static bool wasConnected = false;
  static bool wifiRetrying = false;
  static uint32_t nextWifiRetry = 0;
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) {
    wifiRetrying = false;
  } else if (!wifiSsid.isEmpty() && (!wifiRetrying || static_cast<int32_t>(now - nextWifiRetry) >= 0)) {
    Serial.println("DESKBUDDY_WIFI_RECONNECTING");
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    wifiRetrying = true;
    nextWifiRetry = now + 2000;
  }
  if (connected && !wasConnected) {
    web.begin();
    configTime(0, 0, "ntp.aliyun.com", "pool.ntp.org");
    Serial.printf("DESKBUDDY_IP %s\n", WiFi.localIP().toString().c_str());
  }
  wasConnected = connected;
  if (connected) web.handleClient();
  Mode finished = timer.tick(now);
  if (finished != Mode::Idle) {
    ++revision;
    Work work{finished == Mode::Focus ? FocusDone : RestDone, revision, Mode::Idle, 0};
    int firstTrack = finished == Mode::Focus ? 6 : 2;
    char path[24];
    snprintf(path, sizeof(path), "/track%d.mp3", firstTrack);
    if (speakerReady && fsReady && LittleFS.exists(path)) {
      restOvertime = finished == Mode::Rest;
      int command = restOvertime.load() ? RestOvertime : FocusReward;
      xQueueOverwrite(musicCommands, &command);
    } else if (xQueueSend(alarms, &work, 0) != pdTRUE) Serial.println("Reminder queue full");
    leds.complete(now);
    Serial.printf("TIMER_FINISHED %s\n", modeName(finished));
  }
  Reply reply;
  if (xQueueReceive(replies, &reply, 0) == pdTRUE) {
    if (!strcmp(reply.action, "complete")) {
      busy = false;
    } else if (strcmp(reply.action, "error")) {
      bool accepted = !strcmp(reply.action, "none") || !strcmp(reply.action, "query") ||
                      (reply.revision == revision && applyAction(reply.action, reply.seconds));
      Applied state{accepted, timer.mode(), timer.remaining(now)};
      xQueueSend(applied, &state, 0);
      Serial.printf("UI action %s %d %s\n", reply.action, reply.seconds, accepted ? "accepted" : "rejected");
    }
  }
  bool raw = digitalRead(DialogueButtonPin);
  if (raw != buttonRaw) {
    buttonRaw = raw;
    buttonChangedAt = now;
  }
  if (buttonStable != raw && now - buttonChangedAt >= 25) {
    buttonStable = raw;
    if (raw == LOW) {
      dialogueEnabled = !dialogueEnabled.load();
      dialogueChangedAt = now;
      petMood = PetMood::Idle;
      if (!dialogueEnabled.load()) leds.signal("idle");
      Serial.println(dialogueEnabled.load() ? "VOICE_MODE_ON" : "VOICE_MODE_OFF");
    }
  }
  bool canTalk = dialogueEnabled.load();
  if (now - rgbUpdatedAt >= 33) {
    rgbUpdatedAt = now;
    uint32_t elapsed = now - dialogueChangedAt;
    if (canTalk) {
      uint32_t beat = elapsed % 1400;
      rgbLedWrite(OnboardRgbPin, 0, beat < 120 ? 38 : beat >= 220 && beat < 340 ? 23 : 3, 0);
    } else {
      uint32_t phase = elapsed % 4000;
      uint8_t yellow = 3 + 25 * (phase < 2000 ? phase : 4000 - phase) / 2000;
      rgbLedWrite(OnboardRgbPin, yellow, yellow * 4 / 5, 0);
    }
  }
  bool networkLost = !wifiSsid.isEmpty() && !connected;
  bool localAudio = musicPlaying.load() || alarmActive.load();
  leds.update(timer.mode() != Mode::Idle || localAudio, networkLost, now);
  PetMood mood = !canTalk ? PetMood::Sleeping : networkLost ? PetMood::Error : petMood.load();
  if (localAudio && mood == PetMood::Idle) mood = PetMood::Happy;
  display.update(timer.mode(), timer.remaining(millis()), timer.total(),
                 mood, restOvertime.load(), localAudio || mood == PetMood::Speaking);
  static uint32_t lastRange = 0;
  if (now - lastRange >= 500) {
    lastRange = now;
    digitalWrite(TrigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(TrigPin, LOW);
    uint32_t cm = pulseIn(EchoPin, HIGH, 24000) / 58;
    distanceCm = cm >= 2 && cm <= 400 ? cm : 0;
    static bool rangeReported = false;
    if (distanceCm && !rangeReported) {
      rangeReported = true;
      Serial.printf("SONAR_READY %u cm\n", unsigned(distanceCm));
    }
  }
  delay(5);
}

