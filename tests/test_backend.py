import io
import json
import os
import unittest
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
import wave
from unittest.mock import patch

from backend import server

class VoiceContract(unittest.TestCase):
    def test_transcription_audio_format(self):
        pcm = bytes(160000)
        def upstream(path, data, content_type):
            self.assertEqual(path, "/audio/transcriptions")
            boundary = content_type.split("boundary=")[1].encode()
            payload = data.split(b"Content-Type: audio/wav\r\n\r\n")[1].split(b"\r\n--" + boundary)[0]
            with wave.open(io.BytesIO(payload)) as stream:
                self.assertEqual((stream.getnchannels(), stream.getsampwidth(), stream.getframerate()), (1, 2, 16000))
                self.assertEqual(stream.readframes(80000), pcm)
            return b'{"text":"start"}'
        with patch.object(server, "request", side_effect=upstream):
            self.assertEqual(server.transcribe(pcm), "start")

    def test_plain_chat_loads_prompt_without_tools(self):
        with patch.object(server, "model_message", return_value={"content": "Hello."}) as model:
            self.assertEqual(server.answer("Hi", "chat", "idle", ()), ("Hello.", None))
            payload = model.call_args.args[0]
            self.assertTrue(payload["messages"][0]["content"].startswith(server.PROMPT.read_text(encoding="utf-8-sig")))
            self.assertNotIn("tools", payload)

    def test_model_timer_call_reaches_mcp_and_confirmed_board(self):
        requests = []
        class Board(BaseHTTPRequestHandler):
            def log_message(self, *_): pass
            def do_POST(self):
                requests.append((self.path, self.headers["Authorization"],
                                 self.rfile.read(int(self.headers["Content-Length"]))))
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{"ok":true,"mode":"focus","seconds":600,"revision":8}')
        with HTTPServer(("127.0.0.1", 0), Board) as board:
            thread = threading.Thread(target=board.serve_forever, daemon=True)
            thread.start()
            call = {"id": "one", "type": "function", "function": {
                "name": "start_timer", "arguments": '{"mode":"focus","seconds":600}'}}
            context = {"url": f"http://127.0.0.1:{board.server_port}", "token": "test-token", "revision": "7"}
            try:
                with patch.object(server, "model_message", return_value={"tool_calls": [call]}) as model:
                    reply, state = server.answer("Ten minutes", "focus", "idle", (), context)
                self.assertTrue(reply)
                self.assertEqual(state["revision"], 8)
                self.assertEqual(requests, [("/api/timer", "Bearer test-token", b"action=focus&seconds=600&revision=7")])
                self.assertEqual({t["function"]["name"] for t in model.call_args.args[0]["tools"]},
                                 {"start_timer", "stop_timer", "get_timer"})
                call["function"]["arguments"] = '{"mode":"focus","seconds":7201}'
                with patch.object(server, "model_message", return_value={"tool_calls": [call]}):
                    _, rejected = server.answer("Invalid duration", "focus", "idle", (), context)
                self.assertFalse(rejected["ok"])
                self.assertEqual(len(requests), 1)
            finally:
                board.shutdown()
                thread.join()

if __name__ == "__main__":
    unittest.main()
