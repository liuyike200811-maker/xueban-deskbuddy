# 自选音频包

24 kHz、单声道、有符号 16 位小端原始 PCM。

| 文件名 | 播放时机 |
|---|---|
| ask_focus.pcm | 单击：想专注多久？ |
| ask_rest.pcm | 双击：想休息多久？ |
| ask_chat.pcm | 长按：我在听。 |
| focus_done.pcm | 专注结束的鼓励语 |
| focus_music.pcm | 专注结束鼓励语之后的音乐 |
| rest_done.pcm | 休息结束，提醒回到学习 |
| focus_5.pcm / focus_10.pcm / focus_25.pcm | 离线确认专注计时 |
| rest_5.pcm / rest_10.pcm | 离线确认休息计时 |
| timer_stopped.pcm | 离线确认停止 |
| not_understood.pcm | 未识别到有效命令 |

缺少的提示文件使用内置短音；缺少音乐文件则不播放音乐。

板端点歌使用原 MP3，按 `track1.mp3` 到 `track6.mp3` 编号：

| 编号 | 素材原名 |
|---|---|
| 1 | 11月7日 (1).mp3 |
| 2 | 11月7日(1).mp3 |
| 3 | 11月7日(1)(1).mp3 |
| 4 | 11月7日(2).mp3 |
| 5 | 11月7日(3).mp3 |
| 6 | 11月7日(4).mp3 |

分类：**鼓励**是第 6、1 段；**打鸡血**是第 2、3、4、5 段。专注结束按 6 → 1 各播一次。休息结束按 2 → 3 → 4 → 5 循环；说「停歌」、重新开始或停止计时均可结束循环。直接说「鼓励我」或「放首鼓励音乐」会从第 1、6 段选一段，MCP 工具是 `play_encouragement`。

直接说「放首歌」「播放第3首」或「停歌」。ESP32 调用本地播放动作；外部 MCP 客户端可通过 `backend/timer_mcp.py` 的同名工具控制。用 `tools/Build-DeskBuddy.ps1 -Flash -FlashAudio -Port COM18` 同时写入固件和曲库。

说「扬声器音量调到低档／中档／高档」调整总音量。三档分别为原始 PCM 振幅的 30%、50%、100%，同时影响语音、歌曲和提示音。重启默认中档。外部 MCP 工具为 `set_speaker_volume`，档位参数是 `low`、`medium`、`high`，使用已有的板端 API 认证。
