"""DeskBuddy LAN bridge: PCM -> transcription -> model -> PCM."""
import asyncio
import hmac
import io
import json
import os
import secrets
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
import wave
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.request import Request, urlopen

PAGE = Path(__file__).with_name("dashboard.html")
VOICE_SAMPLES = PAGE.parent.parent / ".build"
WINDOWS_VOICES = ("Huihui", "Kangkang", "Yaoyao")
PROMPT = PAGE.with_name("deskbuddy-prompt.md")

def request(path, data, content_type, *, model_service=False):
    base = os.getenv("DESKBUDDY_MODEL_BASE_URL", "https://api.openai.com/v1") if model_service else "https://api.openai.com/v1"
    if model_service and os.getenv("DESKBUDDY_MODEL_PROVIDER") == "deepseek":
        base = "https://api.deepseek.com"
        key = os.environ["DEEPSEEK_API_KEY"]
    else:
        key = (os.getenv("DESKBUDDY_MODEL_API_KEY") if model_service else None) or os.environ["OPENAI_API_KEY"]
    req = Request(base.rstrip("/") + path, data=data, headers={
        "Content-Type": content_type, "Authorization": "Bearer " + key,
    })
    with urlopen(req, timeout=20) as response:
        return response.read()

def json_request(path, payload, **kwargs):
    return request(path, json.dumps(payload).encode(), "application/json", **kwargs)

def local_transcriber():
    import numpy as np
    from faster_whisper import WhisperModel

    model = WhisperModel(os.getenv("DESKBUDDY_WHISPER_MODEL", "base"), device="cpu", compute_type="int8")

    def transcribe_local(pcm):
        samples = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768
        segments, _ = model.transcribe(samples, language="zh", beam_size=1, vad_filter=True)
        return "".join(segment.text for segment in segments).strip()

    return transcribe_local

def transcribe(pcm):
    wav = io.BytesIO()
    with wave.open(wav, "wb") as f:
        f.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
        f.writeframes(pcm)
    boundary = secrets.token_hex(16)
    model = os.getenv("DESKBUDDY_STT_MODEL", "gpt-transcribe")
    data = (
        f'--{boundary}\r\nContent-Disposition: form-data; name="model"\r\n\r\n{model}\r\n'
        f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="turn.wav"\r\n'
        'Content-Type: audio/wav\r\n\r\n'
    ).encode() + wav.getvalue() + f'\r\n--{boundary}--\r\n'.encode()
    return json.loads(request("/audio/transcriptions", data, "multipart/form-data; boundary=" + boundary))["text"].strip()

def chat_payload(messages):
    deepseek = os.getenv("DESKBUDDY_MODEL_PROVIDER") == "deepseek"
    payload = {"model": os.getenv("DESKBUDDY_MODEL", "deepseek-flash" if deepseek else "gpt-4.1-mini"),
               "messages": messages, "temperature": 0, "max_tokens": 200}
    if deepseek:
        payload["thinking"] = {"type": "disabled"}
    return payload

def model_message(payload):
    result = json.loads(json_request("/chat/completions", payload, model_service=True))
    return result["choices"][0]["message"]

def spoken(message):
    reply = message.get("content")
    if not isinstance(reply, str) or not reply.strip():
        raise ValueError("模型没有返回可播报的内容")
    return reply.strip()[:150]

async def mcp_answer(messages, context):
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client

    params = StdioServerParameters(command=sys.executable, args=[str(PAGE.with_name("timer_mcp.py"))], env={
        "PYTHONPATH": os.getenv("PYTHONPATH", ""),
        "DESKBUDDY_BOARD_URL": context["url"],
        "DESKBUDDY_DEVICE_TOKEN": context["token"],
        "DESKBUDDY_BOARD_REVISION": context["revision"],
    })
    async with stdio_client(params) as (rx, tx):
        async with ClientSession(rx, tx) as session:
            await session.initialize()
            tools = (await session.list_tools()).tools
            allowed = {t.name for t in tools}
            payload = chat_payload(messages)
            payload["tools"] = [{"type": "function", "function": {
                "name": t.name, "description": t.description, "parameters": t.inputSchema,
            }} for t in tools]
            message = await asyncio.to_thread(model_message, payload)
            calls = message.get("tool_calls") or []
            if not calls:
                return spoken(message), None
            if len(calls) != 1 or calls[0]["function"]["name"] not in allowed:
                raise ValueError("模型请求了不允许的计时操作")
            call = calls[0]
            name = call["function"]["name"]
            args = json.loads(call["function"]["arguments"])
            result = await session.call_tool(name, args)
            state = result.structuredContent if not result.isError else None
            if not state:
                state = {"ok": False, "error": "MCP 工具未确认操作"}
            messages.extend((
                {"role": "assistant", "content": message.get("content"), "tool_calls": calls},
                {"role": "tool", "tool_call_id": call["id"], "content": json.dumps(state, ensure_ascii=False)},
            ))
            if name != "get_timer":
                # The device result is the source of truth for spoken confirmations.
                if state.get("ok") is not True:
                    return "这次没确认设置成功。" + state.get("error", ""), state
                if name == "stop_timer":
                    return "好，计时停了。", state
                seconds = state["seconds"]
                mins, secs = divmod(seconds, 60)
                duration = (f"{mins} 分钟" if mins else "") + (f"{secs} 秒" if secs else "")
                return f"好，{duration}{'专注' if state['mode'] == 'focus' else '休息'}，现在开始。", state
            return spoken(await asyncio.to_thread(model_message, chat_payload(messages))), state

def answer(text, mode, timer, history, context=None):
    entry = {"chat": "自由对话", "focus": "学习设置", "rest": "休息设置"}[mode]
    instruction = PROMPT.read_text(encoding="utf-8-sig")
    if context is None:
        instruction += "\n本次未连接设备控制工具。可以聊天，但不能执行或确认计时。"
    messages = [{"role": "system", "content": instruction}, *history,
                {"role": "user", "content": f"交互入口：{entry}；当前计时：{timer}。用户说：{text}"}]
    if context is None:
        return spoken(model_message(chat_payload(messages))), None
    return asyncio.run(mcp_answer(messages, context))

def speak(text):
    return json_request("/audio/speech", {
        "model": os.getenv("DESKBUDDY_TTS_MODEL", "gpt-4o-mini-tts"),
        "voice": os.getenv("DESKBUDDY_VOICE", "alloy"), "input": text, "response_format": "pcm",
    })

def windows_speak(text, voice):
    import av

    job = json.dumps({"text": text, "voice": voice}).encode()
    wave_bytes = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-File", str(PAGE.with_name("windows-voice.ps1"))],
        input=job, capture_output=True, check=True, timeout=15,
    ).stdout
    converter = av.AudioResampler(format="s16", layout="mono", rate=24000)
    chunks = []
    with av.open(io.BytesIO(wave_bytes), format="wav") as source:
        for frame in source.decode(audio=0):
            chunks.extend(bytes(out.planes[0])[:out.samples * 2] for out in converter.resample(frame))
        chunks.extend(bytes(out.planes[0])[:out.samples * 2] for out in converter.resample(None))
    return b"".join(chunks)

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def local(self):
        return self.client_address[0] == "127.0.0.1" and self.headers.get("Host", "").split(":")[0] in ("127.0.0.1", "localhost")

    def send_bytes(self, data, content_type):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def send_json(self, value):
        self.send_bytes(json.dumps(value, ensure_ascii=False).encode(), "application/json; charset=utf-8")

    def do_GET(self):
        if self.path == "/health":
            self.send_bytes(b"OK", "text/plain")
        elif self.local() and self.path == "/":
            self.send_bytes(PAGE.read_bytes(), "text/html; charset=utf-8")
        elif self.local() and self.path.startswith("/sample/"):
            voice = self.path.removeprefix("/sample/")
            path = VOICE_SAMPLES / f"{voice}.wav"
            if voice not in WINDOWS_VOICES or not path.is_file():
                self.send_error(404)
                return
            self.send_bytes(path.read_bytes(), "audio/wav")
        elif self.local() and self.path == "/feedback":
            self.send_json({"events": list(self.server.events), "turn": self.server.turn,
                            "serial": self.server.serial_status, "local_stt": self.server.local_stt,
                            "provider": os.getenv("DESKBUDDY_MODEL_PROVIDER", "openai"),
                            "configured": self.server.model_configured(), "voice": self.server.voice})
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path in ("/setup", "/text", "/voice-choice"):
            origin = self.headers.get("Origin")
            if not self.local() or (origin and origin != "http://" + self.headers["Host"]):
                self.send_error(403)
                return
            if self.headers.get("Content-Type") != "application/json":
                self.send_error(415)
                return
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 2048:
                self.send_error(400)
                return
            try:
                data = json.loads(self.rfile.read(length))
                if self.path == "/setup":
                    if data.get("key"):
                        os.environ["DEEPSEEK_API_KEY"] = data["key"].strip()
                        os.environ["DESKBUDDY_MODEL_PROVIDER"] = "deepseek"
                    if "ssid" in data:
                        self.server.provision(data["ssid"], data.get("password", ""), data["board"])
                    self.send_json({"ok": True})
                elif self.path == "/voice-choice":
                    if data.get("voice") not in WINDOWS_VOICES:
                        raise ValueError("Unknown voice")
                    self.server.voice = data["voice"]
                    self.send_json({"ok": True})
                else:
                    start = time.perf_counter()
                    reply, _ = answer(data["text"][:500], "chat", "idle", ())
                    self.send_json({"reply": reply, "model_ms": round((time.perf_counter() - start) * 1000)})
            except (OSError, KeyError, ValueError, TypeError, AttributeError) as error:
                self.send_json({"ok": False, "error": str(error) if isinstance(error, ValueError) else
                                f"{type(error).__name__} {getattr(error, 'code', '')}".strip()})
            return
        if self.path != "/voice":
            self.send_error(404)
            return
        auth = self.headers.get("Authorization", "")
        expected = "Bearer " + self.server.token
        if not hmac.compare_digest(auth.encode(), expected.encode()):
            self.send_error(401)
            return
        if self.headers.get("Content-Length") != "160000":
            self.send_error(400, "Expected five seconds of 16 kHz mono PCM")
            return
        mode = self.headers.get("X-Mode")
        if mode not in ("focus", "rest", "chat"):
            self.send_error(400, "Invalid mode")
            return
        pcm = self.rfile.read(160000)
        if len(pcm) != 160000:
            self.send_error(400, "Incomplete audio")
            return
        turn = {"id": secrets.token_hex(6), "mode": mode, "transport": self.headers.get("X-Transport", "wifi"), "stage": "transcription", "ms": {}}
        self.server.turn = turn
        start = time.perf_counter()
        try:
            text = self.server.transcriber(pcm)
            turn["ms"]["stt"] = round((time.perf_counter() - start) * 1000)
            turn["text"] = text
            turn["stage"] = "model"
            stage = time.perf_counter()
            if text:
                wifi = turn["transport"] == "wifi" and self.client_address[0] != "127.0.0.1"
                board_url = f"http://{self.client_address[0]}" if wifi else os.getenv("DESKBUDDY_BOARD_URL")
                context = {"url": board_url, "token": self.server.token,
                           "revision": self.headers.get("X-Timer-Revision", "")} if board_url else None
                reply, tool = answer(text, mode, self.headers.get("X-Timer", "unknown"), self.server.history, context)
                if tool is not None:
                    turn["timer_tool"] = tool
            else:
                reply = "我没听清，能再说一遍吗？"
            turn["ms"]["model"] = round((time.perf_counter() - stage) * 1000)
            turn["reply"] = reply
            if self.server.pc_audio:
                audio = b""
            else:
                turn["stage"] = "tts"
                stage = time.perf_counter()
                audio = windows_speak(reply, self.server.voice) if self.server.windows_tts else speak(reply)
                if not audio or len(audio) % 2:
                    raise ValueError("Invalid speech audio")
                turn["ms"]["tts"] = round((time.perf_counter() - stage) * 1000)
            turn["stage"] = "ready"
            turn["ms"]["backend"] = round((time.perf_counter() - start) * 1000)
        except Exception as error:
            turn.update(stage="error", error=f"{type(error).__name__} {getattr(error, 'code', '')}".strip())
            print("Voice upstream failed:", turn["error"], flush=True)
            self.send_error(502, "Voice upstream unavailable")
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(audio)))
        self.send_header("X-Action", "none")
        self.send_header("X-Seconds", "0")
        self.send_header("X-PC-Audio", "1" if self.server.pc_audio else "0")
        self.end_headers()
        self.wfile.write(audio)
        if text:
            self.server.history.extend(({"role": "user", "content": text}, {"role": "assistant", "content": reply}))

class Bridge(ThreadingHTTPServer):
    def __init__(self, address):
        super().__init__(address, Handler)
        self.token = os.getenv("DESKBUDDY_DEVICE_TOKEN") or secrets.token_urlsafe(24)
        self.events = deque(maxlen=80)
        self.history = deque(maxlen=12)
        self.turn = {}
        self.serial_status = "未打开"
        self.serial_port = None
        self.voice = "Huihui"
        self.windows_tts = os.getenv("DESKBUDDY_TTS", "openai") == "windows"
        self.pc_audio = os.getenv("DESKBUDDY_PC_AUDIO", "1") == "1"
        self.local_stt = os.getenv("DESKBUDDY_STT", "openai") == "local"
        self.transcriber = local_transcriber() if self.local_stt else transcribe

    def model_configured(self):
        if os.getenv("DESKBUDDY_MODEL_PROVIDER") == "deepseek":
            return bool(os.getenv("DEEPSEEK_API_KEY"))
        return bool(os.getenv("DESKBUDDY_MODEL_API_KEY") or os.getenv("OPENAI_API_KEY"))

    def read_serial(self, port):
        import serial

        try:
            link = serial.Serial(baudrate=921600, timeout=0.3, write_timeout=2)
            link.port = port
            link.dtr = False
            link.rts = False
            with link:
                self.serial_port = link
                self.serial_status = port
                while True:
                    line = link.readline().decode("ascii", errors="replace").strip()
                    if line.startswith("VOICE_BEGIN "):
                        fields = line.split()
                        if len(fields) != 4:
                            continue
                        _, mode, timer_mode, seconds = fields
                        pcm = bytearray()
                        deadline = time.monotonic() + 5
                        while len(pcm) < 160000 and time.monotonic() < deadline:
                            pcm.extend(link.read(160000 - len(pcm)))
                        self.events.append({"time": time.time(), "line": f"UI usb_rx {len(pcm)}"})
                        action, duration, audio = "error", "0", b""
                        if len(pcm) == 160000:
                            req = Request(f"http://127.0.0.1:{self.server_port}/voice", bytes(pcm), headers={
                                "Authorization": "Bearer " + self.token, "X-Mode": mode,
                                "X-Timer": f"{timer_mode} {seconds}s", "X-Transport": "usb",
                            })
                            try:
                                with urlopen(req, timeout=55) as result:
                                    response_audio = result.read(1800001)
                                    if len(response_audio) <= 1800000:
                                        action, duration = result.headers["X-Action"], result.headers["X-Seconds"]
                                        audio = response_audio
                            except OSError as error:
                                print("USB voice request failed:", type(error).__name__, flush=True)
                        link.write(f"RES\t{action}\t{duration}\t{len(audio)}\n".encode())
                        for offset in range(0, len(audio), 8192):
                            link.write(audio[offset:offset + 8192])
                        continue
                    if line.startswith(("UI ", "OFFLINE ", "TIMER_FINISHED ", "DESKBUDDY_", "No offline command", "Chinese MultiNet")):
                        self.events.append({"time": time.time(), "line": line})
        except OSError:
            self.serial_status = f"{port} 无法打开或已断开"
        finally:
            self.serial_port = None

    def provision(self, ssid, password, board):
        if not self.serial_port:
            raise ValueError("串口未打开")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
            route.connect((board, 80))
            host = route.getsockname()[0]
        values = [ssid, password, f"http://{host}:{self.server_port}", self.token]
        if any(any(sep in x for sep in "\r\n\t") for x in values):
            raise ValueError("配置不能包含换行或制表符")
        self.serial_port.write(("CFG\t" + "\t".join(values) + "\n").encode())
        self.serial_port.flush()

def main():
    server = Bridge((os.getenv("DESKBUDDY_BIND", "0.0.0.0"), int(os.getenv("DESKBUDDY_PORT", "8765"))))
    if port := os.getenv("DESKBUDDY_SERIAL"):
        threading.Thread(target=server.read_serial, args=(port,), daemon=True).start()
    print(f"Dashboard: http://127.0.0.1:{server.server_port}", flush=True)
    server.serve_forever()

if __name__ == "__main__":
    main()
