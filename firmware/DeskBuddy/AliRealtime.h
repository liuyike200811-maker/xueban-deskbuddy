#pragma once
#include <Arduino.h>
#include <ESP_I2S.h>
#include <esp_ns.h>
#include <WebSocketsClient.h>
#include <DeskBuddyTls.h>
#include <cJSON.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <functional>

class AliRealtime {
  WebSocketsClient ws;
  String host, authorization;
  bool online = false, configured = false, failed = false;
  bool running = false, responding = false, toolPending = false;
  uint32_t stoppedAt = 0;
  bool heard = false, audioStarted = false, toolUsed = false;
  String transcript;
  String directMusicResult;
  String directTimerResult;
  const char *awaitingTimerMode = nullptr;
  uint32_t awaitingTimerAt = 0;
  String deferredTimerId, deferredTimerArgs;
  uint32_t deferredTimerAt = 0;
  bool deferredResponseDone = false;
  std::function<bool(const int16_t *, size_t)> play;
  std::function<String(const char *, const char *)> tool;
  std::function<void(const char *)> light;

  static const char *field(const cJSON *j, const char *key) {
    const auto *value = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(value) ? value->valuestring : "";
  }

  // Chinese ASR normalizes spoken numbers to digits. Accept one explicit duration.
  static int transcriptSeconds(const char *text) {
    int duration = 0;
    for (const char *p = text; *p;) {
      if (*p < '0' || *p > '9') { ++p; continue; }
      bool fractional = p > text && p[-1] == '.';
      unsigned value = 0;
      do { if (value <= 7200) value = value * 10 + (*p - '0'); ++p; }
      while (*p >= '0' && *p <= '9');
      while (*p == ' ') ++p;
      if (!strncmp(p, "个", strlen("个"))) p += strlen("个");
      bool shortMinute = false;
      if (!strncmp(p, "分", strlen("分"))) {
        const char *tail = p + strlen("分");
        while (*tail == ' ') ++tail;
        shortMinute = !*tail || !strcmp(tail, "。") || !strcmp(tail, "！") ||
          !strcmp(tail, "？") || !strcmp(tail, "吧") || !strcmp(tail, "吧。") ||
          !strcmp(tail, ".") || !strcmp(tail, "!") || !strcmp(tail, "?");
      }
      unsigned multiplier = !strncmp(p, "秒", strlen("秒")) ? 1 :
        (!strncmp(p, "分钟", strlen("分钟")) || shortMinute) ? 60 :
        !strncmp(p, "小时", strlen("小时")) ? 3600 : 0;
      if (!multiplier) continue;
      if (fractional || duration || !value || value > 7200 / multiplier) return 0;
      duration = value * multiplier;
    }
    return duration;
  }

  void dispatchTimer() {
    String phrase = transcript;
    phrase.trim();
    for (const char *prefix : {"请", "能不能", "可以", "帮我", "给我"}) {
      if (phrase.startsWith(prefix)) {
        phrase.remove(0, strlen(prefix));
        phrase.trim();
      }
    }
    const char *mode = nullptr;
    if (phrase.startsWith("休息") || phrase.startsWith("开始休息")) mode = "rest";
    else if (phrase.startsWith("计时") || phrase.startsWith("定时") || phrase.startsWith("倒计时") ||
             phrase.startsWith("专注") || phrase.startsWith("开始计时") || phrase.startsWith("开始专注")) mode = "focus";
    if (phrase.indexOf("不要") >= 0 || phrase.indexOf("别") >= 0 ||
        phrase.indexOf("还是") >= 0 || phrase.indexOf("之后") >= 0 || phrase.indexOf("以后") >= 0) return;
    int seconds = transcriptSeconds(phrase.c_str());
    if (mode && !seconds) {
      if (phrase.endsWith("。") || phrase.endsWith("？")) phrase.remove(phrase.length() - strlen("。"));
      if (phrase == "计时" || phrase == "定时" || phrase == "倒计时" || phrase == "专注" ||
          phrase == "休息" || phrase == "开始计时" || phrase == "开始专注" || phrase == "开始休息") {
        awaitingTimerMode = mode;
        awaitingTimerAt = millis();
      }
      return;
    }
    if (!mode && awaitingTimerMode && millis() - awaitingTimerAt <= 30000 &&
        phrase.length() && phrase[0] >= '0' && phrase[0] <= '9' &&
        phrase.indexOf("吗") < 0 && phrase.indexOf("够") < 0) mode = awaitingTimerMode;
    awaitingTimerMode = nullptr;
    if (!mode || !seconds) return;
    String args = String("{\"mode\":\"") + mode + "\",\"seconds\":" + seconds + "}";
    directTimerResult = tool("start_timer", args.c_str());
    Serial.printf("ALI_TIMER_DIRECT %s %d %s\n", mode, seconds, directTimerResult.c_str());
  }

  String verifiedTimer(const char *arguments) {
    if (!directTimerResult.isEmpty()) return directTimerResult;
    int seconds = transcriptSeconds(transcript.c_str());
    auto *args = cJSON_Parse(arguments);
    auto *mode = cJSON_GetObjectItemCaseSensitive(args, "mode");
    if (seconds && cJSON_IsString(mode)) {
      String verified = String("{\"mode\":") + (!strcmp(mode->valuestring, "focus") ? "\"focus\"" :
        !strcmp(mode->valuestring, "rest") ? "\"rest\"" : "null") + ",\"seconds\":" + seconds + "}";
      Serial.printf("ALI_TIMER_VERIFIED %d\n", seconds);
      directTimerResult = tool("start_timer", verified.c_str());
      awaitingTimerMode = nullptr;
    }
    cJSON_Delete(args);
    if (!directTimerResult.isEmpty()) return directTimerResult;
    Serial.println("ALI_TIMER_DURATION_UNCONFIRMED");
    return "{\"ok\":false,\"error\":\"没听到明确时长，请问用户要几分钟或几秒\"}";
  }

  void sendToolResult(const char *id, const String &result) {
    auto *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "conversation.item.create");
    auto *item = cJSON_AddObjectToObject(msg, "item");
    cJSON_AddStringToObject(item, "type", "function_call_output");
    cJSON_AddStringToObject(item, "call_id", id);
    cJSON_AddStringToObject(item, "output", result.c_str());
    if (send(msg)) toolPending = true;
  }

  void requestToolReply() {
    toolPending = false;
    if (!ws.sendTXT("{\"type\":\"response.create\",\"response\":{\"modalities\":[\"audio\",\"text\"]}}")) failed = true;
  }

  void resolveDeferredTimer() {
    sendToolResult(deferredTimerId.c_str(), verifiedTimer(deferredTimerArgs.c_str()));
    deferredTimerId = "";
    deferredTimerArgs = "";
    if (deferredResponseDone && toolPending) requestToolReply();
    deferredResponseDone = false;
  }

  void dispatchMusic() {
    String phrase = transcript;
    phrase.trim();
    if (phrase.endsWith("。")) phrase.remove(phrase.length() - strlen("。"));
    if (phrase.startsWith("请")) phrase.remove(0, strlen("请"));
    if (phrase.startsWith("帮我")) phrase.remove(0, strlen("帮我"));
    phrase.trim();
    String volume = phrase;
    if (volume.startsWith("把")) volume.remove(0, strlen("把"));
    if (volume.startsWith("扬声器")) volume.remove(0, strlen("扬声器"));
    if (volume.startsWith("喇叭")) volume.remove(0, strlen("喇叭"));
    if (volume.startsWith("音量")) volume.remove(0, strlen("音量"));
    if (volume.startsWith("调到")) volume.remove(0, strlen("调到"));
    if (volume == "低档" || volume == "中档" || volume == "高档") {
      const char *args = volume == "低档" ? "{\"level\":\"low\"}" :
                         volume == "中档" ? "{\"level\":\"medium\"}" : "{\"level\":\"high\"}";
      directMusicResult = tool("set_speaker_volume", args);
    } else if (phrase == "鼓励我" || phrase == "鼓励我一下" || phrase == "放首鼓励音乐" ||
               phrase == "放一首鼓励音乐" || phrase == "播放鼓励音乐" || phrase == "放歌鼓励我") {
      directMusicResult = tool("play_encouragement", "{}");
    } else if (phrase == "停歌" || phrase == "停止播放") {
      directMusicResult = tool("stop_music", "{}");
    } else if (phrase == "放首歌" || phrase == "放一首歌" || phrase == "放歌") {
      directMusicResult = tool("play_music", "{\"track\":1}");
    } else {
      constexpr const char *digits[] = {"一","二","三","四","五","六"};
      for (unsigned i = 1; i <= 6; ++i) {
        if (phrase == String("播放第") + i + "首" ||
            phrase == String("播放第") + digits[i - 1] + "首") {
          String arguments = String("{\"track\":") + i + "}";
          directMusicResult = tool("play_music", arguments.c_str());
          break;
        }
      }
    }
    if (!directMusicResult.isEmpty()) Serial.printf("MUSIC_COMMAND %s\n", directMusicResult.c_str());
  }

  void lightState(const char *state) {
    if (light) light(state);
  }

  bool send(cJSON *message) {
    char *text = cJSON_PrintUnformatted(message);
    bool sent = text && ws.sendTXT(text);
    cJSON_free(text);
    cJSON_Delete(message);
    if (!sent) failed = true;
    return sent;
  }

  void event(WStype_t kind, uint8_t *data, size_t length) {
    if (kind == WStype_CONNECTED) {
      online = true;
      return;
    }
    if (kind == WStype_DISCONNECTED || kind == WStype_ERROR) {
      if (running) Serial.printf("ALI_WS_EVENT %s wifi=%d detail=%.*s\n",
        kind == WStype_DISCONNECTED ? "disconnected" : "error", int(WiFi.status()),
        int(min(length, size_t(120))), data ? reinterpret_cast<const char *>(data) : "");
      online = configured = false;
      if (running) failed = true;
      lightState("error");
      return;
    }
    if (kind != WStype_TEXT) return;
    cJSON *j = cJSON_ParseWithLength(reinterpret_cast<const char *>(data), length);
    if (!j) {
      Serial.printf("ALI_INVALID_JSON bytes=%u\n", unsigned(length));
      failed = true;
      lightState("error");
      return;
    }
    const char *type = field(j, "type");
    if (!strcmp(type, "session.updated")) {
      const auto *session = cJSON_GetObjectItemCaseSensitive(j, "session");
      const auto *modalities = cJSON_GetObjectItemCaseSensitive(session, "modalities");
      Serial.printf("ALI_SESSION model=%s voice=%s audio_modal=%u\n",
                    field(session, "model"), field(session, "voice"),
                    cJSON_IsArray(modalities) && cJSON_GetArraySize(modalities) == 2);
      configured = true;
    } else if (!strcmp(type, "error")) {
      const auto *error = cJSON_GetObjectItemCaseSensitive(j, "error");
      Serial.printf("ALI_ERROR %.80s %.120s\n", field(error, "code"), field(error, "message"));
      failed = true;
      lightState("error");
    } else if (!strcmp(type, "conversation.item.input_audio_transcription.completed")) {
      transcript = field(j, "transcript");
      Serial.printf("ALI_HEARD %.120s\n", transcript.c_str());
      if (running && directTimerResult.isEmpty()) dispatchTimer();
      if (!deferredTimerId.isEmpty()) resolveDeferredTimer();
      if (running && !toolUsed && directMusicResult.isEmpty()) dispatchMusic();
    } else if (!strcmp(type, "response.audio_transcript.done")) {
      Serial.printf("ALI_REPLY %.160s\n", field(j, "transcript"));
    } else if (running) {
      if (!strcmp(type, "input_audio_buffer.speech_started")) {
        transcript = "";
        directMusicResult = "";
        directTimerResult = "";
        heard = true;
        lightState("hearing");
        Serial.println("ALI_SPEECH_START");
      } else if (!strcmp(type, "input_audio_buffer.speech_stopped")) {
        const char *reason = field(j, "reason");
        if (!strcmp(reason, "turn_invalid")) {
          lightState("listening");
          Serial.println("ALI_SPEECH_IGNORED");
        } else {
          responding = true;
          stoppedAt = millis();
          lightState("thinking");
          Serial.println("ALI_SPEECH_STOP");
        }
      } else if (!strcmp(type, "response.audio.delta")) {
        const char *encoded = field(j, "delta");
        size_t length = strlen(encoded);
        size_t n = 0;
        size_t capacity = length * 3 / 4 + 3;
        auto *pcm = static_cast<unsigned char *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM));
        if (pcm && mbedtls_base64_decode(pcm, capacity, &n, reinterpret_cast<const unsigned char *>(encoded), length) == 0 && n && !(n % 2)) {
          if (!audioStarted) {
            audioStarted = true;
            lightState("speaking");
            Serial.printf("ALI_FIRST_AUDIO %lu ms\n", static_cast<unsigned long>(stoppedAt ? millis() - stoppedAt : 0));
          }
          responding = true;
          if (!play(reinterpret_cast<const int16_t *>(pcm), n / 2)) failed = true;
        } else {
          failed = true;
        }
        free(pcm);
      } else if (!strcmp(type, "response.function_call_arguments.done")) {
        const char *id = field(j, "call_id");
        if (!*id) {
          failed = true;
          lightState("error");
        } else {
          const char *name = field(j, "name");
          if (!toolUsed && !strcmp(name, "start_timer") && transcript.isEmpty()) {
            deferredTimerId = id;
            deferredTimerArgs = field(j, "arguments");
            deferredTimerAt = millis();
            deferredResponseDone = false;
            toolUsed = true;
            cJSON_Delete(j);
            return;
          }
          String result;
          if (!directTimerResult.isEmpty() && !strcmp(name, "start_timer")) {
            result = directTimerResult;
          } else if (!directMusicResult.isEmpty() && (!strcmp(name, "play_music") || !strcmp(name, "stop_music") ||
              !strcmp(name, "play_encouragement") || !strcmp(name, "set_speaker_volume"))) {
            result = directMusicResult;
          } else if (toolUsed) {
            result = "{\"ok\":false,\"error\":\"每轮只允许一次计时操作\"}";
          } else if (!strcmp(name, "start_timer")) {
            result = verifiedTimer(field(j, "arguments"));
          } else {
            result = tool(name, field(j, "arguments"));
          }
          toolUsed = true;
          sendToolResult(id, result);
        }
      } else if (!strcmp(type, "response.done")) {
        if (!deferredTimerId.isEmpty()) {
          deferredResponseDone = true;
        } else if (toolPending) {
          requestToolReply();
        } else {
          const auto *response = cJSON_GetObjectItemCaseSensitive(j, "response");
          const char *status = field(response, "status");
          const auto *output = cJSON_GetObjectItemCaseSensitive(response, "output");
          const char *outputType = "";
          const char *outputText = "";
          if (cJSON_IsArray(output) && cJSON_GetArraySize(output)) {
            const auto *item = cJSON_GetArrayItem(output, 0);
            outputType = field(item, "type");
            const auto *content = cJSON_GetObjectItemCaseSensitive(item, "content");
            if (cJSON_IsArray(content) && cJSON_GetArraySize(content)) {
              const auto *part = cJSON_GetArrayItem(content, 0);
              outputText = field(part, "transcript");
              if (!*outputText) outputText = field(part, "text");
            }
          }
          Serial.printf("ALI_DONE status=%s output=%s text=%.120s\n", status, outputType, outputText);
          const auto *details = cJSON_GetObjectItemCaseSensitive(response, "status_details");
          const char *reason = field(details, "reason");
          if (!strcmp(status, "cancelled") && !strcmp(reason, "turn_detected")) {
            Serial.println("ALI_TURN_INTERRUPTED");
            lightState("hearing");
          } else if (!*status || !strcmp(status, "completed")) {
            lightState("listening");
            Serial.println("ALI_TURN_DONE");
          } else {
            Serial.printf("ALI_RESPONSE %s %.80s\n", status, reason);
            failed = true;
            lightState("error");
          }
          responding = false;
          heard = false;
          audioStarted = false;
          toolUsed = false;
        }
      }
    }
    cJSON_Delete(j);
  }

  bool session(const String &endpoint, const String &key, const String &voice, const char *prompt) {
    if (configured) return true;
    if (time(nullptr) < 1700000000) {
      uint32_t start = millis();
      while (time(nullptr) < 1700000000 && millis() - start < 6000) delay(50);
      if (time(nullptr) < 1700000000) {
        Serial.println("ALI_CLOCK_NOT_SYNCED");
        return false;
      }
    }
    failed = false;
    host = endpoint;
    authorization = "Authorization: Bearer " + key + "\r\n";
    ws.beginSslWithBundle(host.c_str(), 443,
      "/api-ws/v1/realtime?model=qwen-audio-3.1-realtime-flash",
      DeskBuddyCaBundle, sizeof(DeskBuddyCaBundle), "");
    ws.setExtraHeaders(authorization.c_str());
    ws.onEvent([this](WStype_t k, uint8_t *p, size_t n) { event(k, p, n); });
    ws.setReconnectInterval(0);
    uint32_t start = millis();
    while (!online && millis() - start < 7500) { ws.loop(); delay(2); }
    if (!online) return false;
    auto *msg = cJSON_Parse(R"JSON({"type":"session.update","session":{
      "modalities":["audio","text"],"turn_detection":{"type":"server_vad","threshold":0.35,"silence_duration_ms":2000},
      "input_audio_format":"pcm","output_audio_format":"pcm",
      "input_audio_transcription":{"language":"zh"},"output_audio":{"language":"zh"},
      "tools":[
        {"type":"function","function":{"name":"start_timer","description":"开始或修改倒计时；没有时长就先问。","parameters":{"type":"object","properties":{"mode":{"type":"string","enum":["focus","rest"]},"seconds":{"type":"integer","minimum":1,"maximum":7200}},"required":["mode","seconds"]}}},
        {"type":"function","function":{"name":"stop_timer","description":"停止当前倒计时。","parameters":{"type":"object","properties":{}}}},
        {"type":"function","function":{"name":"get_timer","description":"查询当前倒计时和剩余秒数。","parameters":{"type":"object","properties":{}}}},
        {"type":"function","function":{"name":"play_music","description":"播放板上第1到6首曲目。只说放首歌时选第1首。","parameters":{"type":"object","properties":{"track":{"type":"integer","minimum":1,"maximum":6}}}}},
        {"type":"function","function":{"name":"stop_music","description":"停止歌曲播放，不影响倒计时。","parameters":{"type":"object","properties":{}}}},
        {"type":"function","function":{"name":"play_encouragement","description":"从两段鼓励音频中选一段播放。用户要用音频鼓励时调用。","parameters":{"type":"object","properties":{}}}},
        {"type":"function","function":{"name":"set_speaker_volume","description":"设置扬声器总音量低、中、高档，对音乐、语音回复和提示音都生效。","parameters":{"type":"object","properties":{"level":{"type":"string","enum":["low","medium","high"]}},"required":["level"]}}}
      ]}})JSON");
    if (!msg) return false;
    auto *cfg = cJSON_GetObjectItemCaseSensitive(msg, "session");
    cJSON_AddStringToObject(cfg, "voice", voice.c_str());
    cJSON_AddStringToObject(cfg, "instructions", prompt);
    if (!send(msg)) return false;
    start = millis();
    while (!configured && !failed && millis() - start < 5000) { ws.loop(); delay(2); }
    return configured;
  }

public:
  void close() { ws.disconnect(); online = configured = false; }

  bool listen(const String &endpoint, const String &key, const String &voice, const char *prompt,
              I2SClass &mic, std::function<bool()> muted,
              std::function<void(const char *)> lightState,
              std::function<bool(const int16_t *, size_t)> sink,
              std::function<String(const char *, const char *)> call,
              std::function<bool()> stopRequested) {
    light = lightState;
    if (!session(endpoint, key, voice, prompt)) {
      lightState("error");
      close();
      return false;
    }
    play = sink;
    tool = call;
    failed = toolPending = responding = heard = audioStarted = toolUsed = false;
    stoppedAt = 0;
    transcript = "";
    directMusicResult = "";
    directTimerResult = "";
    awaitingTimerMode = nullptr;
    deferredTimerId = "";
    deferredTimerArgs = "";
    deferredResponseDone = false;
    running = true;
    bool micReady = mic.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
    if (!micReady) {
      failed = true;
      Serial.println("ALI_MIC_INIT_FAILED");
      lightState("error");
    } else {
      lightState("listening");
      Serial.println("ALI_LISTENING");
    }
    int32_t raw[640 * 2];
    int16_t pcm[640];
    int16_t frame[160];
    ns_handle_t noise = micReady ? ns_pro_create(10, 1, 16000) : nullptr;
    Serial.println(noise ? "ALI_NS_READY 10ms medium" : "ALI_NS_UNAVAILABLE");
    unsigned char encoded[1710];
    uint32_t reportAt = millis();
    unsigned inputSpan = 0, outputSpan = 0, sentBlocks = 0, mutedBlocks = 0;
    unsigned leftSpan = 0, rightSpan = 0, leftChanged = 0, rightChanged = 0;
    while (online && !failed && !stopRequested()) {
      ws.loop();
      if (failed) break;
      if (!deferredTimerId.isEmpty() && millis() - deferredTimerAt >= 1500) resolveDeferredTimer();
      size_t bytes = mic.readBytes(reinterpret_cast<char *>(raw), sizeof(raw));
      if (bytes != sizeof(raw)) {
        Serial.printf("ALI_MIC_READ_FAILED bytes=%u expected=%u\n", unsigned(bytes), unsigned(sizeof(raw)));
        failed = true;
        break;
      }
      if (stopRequested()) break;
      bool silence = responding || muted();
      int low[2] = {INT32_MAX, INT32_MAX}, high[2] = {INT32_MIN, INT32_MIN};
      for (unsigned i = 0; i < 640; ++i) {
        for (unsigned ch = 0; ch < 2; ++ch) {
          int sample = raw[i * 2 + ch] >> 8;
          low[ch] = min(low[ch], sample);
          high[ch] = max(high[ch], sample);
        }
      }
      unsigned l = high[0] - low[0], r = high[1] - low[1];
      leftSpan = max(leftSpan, l);
      rightSpan = max(rightSpan, r);
      leftChanged += l != 0;
      rightChanged += r != 0;
      unsigned slot = l == 0 && r != 0 ? 1 : 0;
      int rawMin = INT16_MAX, rawMax = INT16_MIN;
      for (unsigned offset = 0; offset < 640; offset += 160) {
        for (unsigned i = 0; i < 160; ++i) {
          int sample = raw[2 * (offset + i) + slot] >> 16;
          rawMin = min(rawMin, sample);
          rawMax = max(rawMax, sample);
          frame[i] = silence ? 0 : sample;
        }
        if (noise) ns_process(noise, frame, pcm + offset);
        else memcpy(pcm + offset, frame, sizeof(frame));
      }
      if (silence) memset(pcm, 0, sizeof(pcm));
      int16_t outMin = INT16_MAX, outMax = INT16_MIN;
      for (int16_t sample : pcm) {
        outMin = min(outMin, sample);
        outMax = max(outMax, sample);
      }
      inputSpan = max(inputSpan, unsigned(rawMax - rawMin));
      outputSpan = max(outputSpan, unsigned(int(outMax) - int(outMin)));
      size_t n = 0;
      if (mbedtls_base64_encode(encoded, sizeof(encoded), &n, reinterpret_cast<const unsigned char *>(pcm), sizeof(pcm))) {
        failed = true;
        break;
      }
      auto *msg = cJSON_CreateObject();
      cJSON_AddStringToObject(msg, "type", "input_audio_buffer.append");
      cJSON_AddStringToObject(msg, "audio", reinterpret_cast<char *>(encoded));
      if (!send(msg)) {
        Serial.printf("ALI_INPUT_SEND_FAILED wifi=%d sent_blocks=%u\n", int(WiFi.status()), sentBlocks);
      } else {
        ++sentBlocks;
        if (silence) ++mutedBlocks;
      }
      uint32_t now = millis();
      if (now - reportAt >= 2000) {
        Serial.printf("ALI_MIC_LEVEL raw_pp=%u sent_pp=%u blocks=%u muted=%u\n",
                      inputSpan, outputSpan, sentBlocks, mutedBlocks);
        Serial.printf("ALI_MIC_STEREO left_pp24=%u right_pp24=%u left_active_blocks=%u right_active_blocks=%u\n",
                      leftSpan, rightSpan, leftChanged, rightChanged);
        reportAt = now;
        inputSpan = outputSpan = sentBlocks = mutedBlocks = 0;
        leftSpan = rightSpan = leftChanged = rightChanged = 0;
      }
    }
    if (noise) ns_destroy(noise);
    if (micReady) mic.end();
    running = false;
    Serial.printf("ALI_LISTEN_END online=%u failed=%u speech=%u audio=%u\n", online, failed, heard, audioStarted);
    close();
    lightState(failed ? "error" : "idle");
    return false;
  }
};
