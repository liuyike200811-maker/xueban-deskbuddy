#pragma once
#include <Arduino.h>
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

struct OfflineCommand {
  const char *phrase, *action;
  int seconds;
  const char *clip;
};

// MultiNet 7 Chinese uses pinyin tokens. Add or remove supported phrases here.
constexpr OfflineCommand Commands[] = {
  {"kai shi zhuan zhu", "focus", 1500, "/focus_25.pcm"},
  {"zhuan zhu wu fen zhong", "focus", 300, "/focus_5.pcm"},
  {"zhuan zhu shi fen zhong", "focus", 600, "/focus_10.pcm"},
  {"zhuan zhu er shi wu fen zhong", "focus", 1500, "/focus_25.pcm"},
  {"kai shi xiu xi", "rest", 600, "/rest_10.pcm"},
  {"xiu xi wu fen zhong", "rest", 300, "/rest_5.pcm"},
  {"xiu xi shi fen zhong", "rest", 600, "/rest_10.pcm"},
  {"ting zhi ji shi", "stop", 0, "/timer_stopped.pcm"},
};

class OfflineSpeech {
  const esp_mn_iface_t *iface_ = nullptr;
  model_iface_data_t *model_ = nullptr;
  bool attempted_ = false;

  bool init() {
    if (attempted_) return model_ != nullptr;
    attempted_ = true;
    srmodel_list_t *models = esp_srmodel_init("model");
    char *name = models ? esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE) : nullptr;
    if (!name) { Serial.println("Chinese MultiNet model missing"); return false; }
    iface_ = esp_mn_handle_from_name(name);
    if (!iface_) return false;
    model_ = iface_->create(name, 6000);
    if (!model_) return false;
    bool ok = esp_mn_commands_alloc(iface_, model_) == ESP_OK;
    for (unsigned i = 0; ok && i < sizeof(Commands) / sizeof(*Commands); ++i) {
      ok = esp_mn_commands_add(i + 1, Commands[i].phrase) == ESP_OK;
    }
    esp_mn_error_t *errors = ok ? esp_mn_commands_update() : nullptr;
    if (!ok || (errors && errors->num)) {
      esp_mn_commands_free();
      iface_->destroy(model_);
      model_ = nullptr;
      Serial.println("Could not register Chinese offline commands");
    }
    return model_ != nullptr;
  }
public:
  const OfflineCommand *recognize(int16_t *pcm, unsigned count) {
    if (!init()) return nullptr;
    iface_->clean(model_);
    unsigned frame = iface_->get_samp_chunksize(model_);
    for (unsigned pos = 0; pos + frame <= count; pos += frame) {
      auto state = iface_->detect(model_, pcm + pos);
      if (state == ESP_MN_STATE_TIMEOUT) break;
      if (state == ESP_MN_STATE_DETECTED) {
        auto result = iface_->get_results(model_);
        int id = result && result->num ? result->command_id[0] : 0;
        return id > 0 && id <= int(sizeof(Commands) / sizeof(*Commands)) ? &Commands[id - 1] : nullptr;
      }
      delay(1);
    }
    return nullptr;
  }
};
