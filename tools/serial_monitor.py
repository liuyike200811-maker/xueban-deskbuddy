"""Read-only DeskBuddy serial console. No reset, no serial commands."""
import argparse
from collections import deque
from datetime import datetime
from pathlib import Path
import re
import sys
from time import monotonic, sleep
from contextlib import ExitStack

import serial

LOG_DIR = Path(__file__).resolve().parents[1] / "logs" / "serial"

LABELS = {
    "DESKBUDDY_READY": "固件已启动",
    "DESKBUDDY_WIFI_RECONNECTING": "正在连接 Wi-Fi",
    "DESKBUDDY_WIFI_DISCONNECTED": "Wi-Fi 断开或连接失败",
    "ALI_WAITING_FOR_NETWORK": "等待联网或对话开启",
    "ALI_INPUT_SEND_FAILED": "故障：麦克风音频上传失败",
    "ALI_MIC_INIT_FAILED": "故障：麦克风初始化失败",
    "ALI_MIC_READ_FAILED": "故障：麦克风读取不完整",
    "ALI_MIC_LEVEL": "麦克风电平与上行",
    "ALI_MIC_STEREO": "麦克风左右声道原始电平",
    "ALI_ERROR": "故障：百炼返回错误",
    "ALI_RESPONSE": "故障：百炼回合异常结束",
    "ALI_INVALID_JSON": "故障：回复数据解析失败",
    "ALI_SPEECH_START": "收音开始",
    "ALI_SPEECH_STOP": "收音结束，等待云端",
    "ALI_SPEECH_IGNORED": "云端判定为无效语音",
    "ALI_HEARD": "转写",
    "ALI_REPLY": "回复文本",
    "ALI_FIRST_AUDIO": "首段回复音频到达",
    "ALI_TURN_DONE": "云端回合结束",
    "ALI_TURN_INTERRUPTED": "检测到新语音，当前回复被打断",
    "ALI_WS_EVENT": "WebSocket 事件",
    "ALI_WS_FRAME_REJECT": "板端拒收超长帧",
    "ALI_WS_LOCAL_CLOSE": "板端主动关闭连接",
    "ALI_WS_PEER_CLOSE": "对端主动关闭连接",
    "ALI_WS_IO_TIMEOUT": "WebSocket 读写超时",
    "ALI_LISTEN_END": "监听结束",
    "ALI_TIMER_VERIFIED": "板端核对时长（秒）",
    "ALI_TIMER_DIRECT": "板端根据语音转写执行计时",
    "ALI_TIMER_DURATION_UNCONFIRMED": "未确认时长，未启动计时",
    "UI action": "计时执行结果",
    "ALI_SESSION": "百炼会话建立",
    "ALI_LISTENING": "开始监听",
    "ALI_NS_READY": "单麦降噪已启用",
    "DESKBUDDY_IP": "Wi-Fi 已连接",
    "TIMER_FINISHED": "倒计时结束",
    "MUSIC_PLAYING": "板端开始播放",
    "MUSIC_ERROR": "音乐播放失败",
    "MUSIC_END": "曲目播放结束或被切换",
    "MUSIC_STOPPED": "音乐已停止",
    "SPEAKER_VOLUME": "扬声器总音量",
    "VOICE_MODE_ON": "对话模式开启",
    "VOICE_MODE_OFF": "对话模式关闭",
    "TOUCH_FOCUS": "触摸单击：专注 25 分钟",
    "TOUCH_GPIO": "触摸输入电平",
    "TOUCH_REST": "触摸双击：休息 10 分钟",
}
CONTROL = re.compile(r"[\x00-\x1f\x7f-\x9f]")


class Monitor:
    def __init__(self, log=None, automatic_port=None):
        self.log = log
        self.automatic_port = automatic_port
        self.log_day = None
        self.recent = deque(maxlen=64)
        self.phase = ""
        self.since = None
        self.warned = False
        self.failed_at = None

    def show(self, label, message):
        print(f"{datetime.now():%H:%M:%S}  [{label}] {message}", flush=True)

    def save(self, line):
        stamp = datetime.now()
        if self.automatic_port and stamp.date() != self.log_day:
            self.log_day = stamp.date()
            if self.log:
                self.log.close()
                self.log = None
            path = LOG_DIR / f"{stamp:%Y-%m-%d}-{self.automatic_port}.log"
            try:
                path.parent.mkdir(parents=True, exist_ok=True)
                self.log = path.open("a", encoding="utf-8")
                self.show("日志", f"自动保存到 {path}")
            except OSError as exc:
                self.show("日志保存失败", str(exc))
        if self.log:
            try:
                print(f"{stamp.isoformat(timespec='milliseconds')} {line}", file=self.log, flush=True)
            except OSError as exc:
                self.show("日志保存失败", str(exc))
                self.log.close()
                self.log = None

    def close(self):
        if self.log:
            self.log.close()

    def line(self, raw, event_time=None):
        line = CONTROL.sub("", raw).lstrip("\ufffd ").strip()
        now = monotonic() if event_time is None else event_time
        while self.recent and now - self.recent[0][0] > 1:
            self.recent.popleft()
        if not line or any(previous == line for _, previous in self.recent):
            return
        self.recent.append((now, line))
        self.save(line)
        key = next((k for k in LABELS if line == k or line.startswith(k + " ")), "")
        if line.startswith("WebRtcNs_Create:"):
            return
        self.show(LABELS.get(key, "原始"), line)
        if key == "ALI_INPUT_SEND_FAILED":
            self.failed_at = now
            self.since = None
            self.show("诊断", "这一段麦克风音频未能发出。等待会话恢复；此日志本身不能判断 Wi-Fi 是否掉线。")
        elif key == "ALI_LISTENING" and self.failed_at is not None:
            self.show("恢复", f"已重新进入监听，距上传失败 {now - self.failed_at:.1f} 秒。")
            self.failed_at = None
            self.since = None
        elif key == "ALI_LISTEN_END" and "failed=1" in line:
            if self.failed_at is None:
                self.failed_at = now
            self.since = None
            if "speech=0" in line and "audio=0" in line:
                self.show("诊断", "监听结束时没有正在记录的收音或回复音频；先看上方最近一条故障。")
        elif key in ("ALI_SPEECH_START", "ALI_SPEECH_STOP", "ALI_HEARD", "ALI_FIRST_AUDIO"):
            self.phase = LABELS[key]
            self.since = monotonic()
            self.warned = False
        elif key in ("ALI_SPEECH_IGNORED", "ALI_TURN_DONE", "ALI_TURN_INTERRUPTED", "ALI_LISTEN_END", "ALI_LISTENING") or line.startswith("ALI_ERROR"):
            self.since = None

    def check_wait(self):
        if self.since is not None and not self.warned and monotonic() - self.since >= 8:
            self.show("等待", f"停在「{self.phase}」已超过 8 秒；未看到下一阶段日志。")
            self.warned = True


def main():
    parser = argparse.ArgumentParser(description="DeskBuddy 只读串口日志")
    parser.add_argument("--port", default="COM18")
    parser.add_argument("--baud", type=int, default=921600)
    logging = parser.add_mutually_exclusive_group()
    logging.add_argument("--log", type=Path, help="指定日志路径，代替按日期自动保存")
    logging.add_argument("--no-log", action="store_true", help="仅显示，不保存日志")
    parser.add_argument("--replay", type=Path, help="回放日志，不打开串口")
    parser.add_argument("--seconds", type=float, help="采集指定秒数后退出")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    with ExitStack() as stack:
        log = stack.enter_context(args.log.open("a", encoding="utf-8")) if args.log else None
        automatic = None if args.no_log or args.log or args.replay else re.sub(r"[^A-Za-z0-9_-]", "_", args.port)
        monitor = Monitor(log, automatic)
        stack.callback(monitor.close)
        if args.replay:
            for line in args.replay.read_text(encoding="utf-8").splitlines():
                match = re.match(r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?) (.*)$", line)
                if match:
                    monitor.line(match[2], datetime.fromisoformat(match[1]).timestamp())
                else:
                    monitor.line(line)
            return
        end = monotonic() + args.seconds if args.seconds is not None else float("inf")
        monitor.show("串口", f"{args.port} @ {args.baud} | 只读 | Ctrl+C 退出")
        if automatic:
            monitor.save(f"MONITOR_START port={args.port} baud={args.baud}")
        last_error = ""
        while monotonic() < end:
            try:
                port = serial.Serial(port=None, baudrate=args.baud, timeout=0.2)
                port.dtr = port.rts = False
                port.port = args.port
                with port:
                    monitor.show("串口", "已连接，等待板端日志")
                    monitor.save(f"MONITOR_CONNECTED port={args.port}")
                    last_error = ""
                    pending = bytearray()
                    while monotonic() < end:
                        pending.extend(port.read(port.in_waiting or 1))
                        while (index := pending.find(b"\n")) >= 0:
                            monitor.line(pending[:index].decode("utf-8", errors="replace"))
                            del pending[:index + 1]
                        if len(pending) > 8192:
                            pending.clear()
                        monitor.check_wait()
            except (serial.SerialException, OSError) as exc:
                if str(exc) != last_error:
                    monitor.show("串口不可用", f"{exc}。若正在烧录或 Arduino 串口监视器已打开，关闭占用后会重试。")
                    last_error = str(exc)
                monitor.since = None
                sleep(1)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n串口监听已结束。")
