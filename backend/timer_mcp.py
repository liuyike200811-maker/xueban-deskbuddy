"""DeskBuddy timer MCP server. Start this module with an MCP stdio client."""
import json
import os
from typing import Annotated, Literal
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from mcp.server.fastmcp import FastMCP
from pydantic import Field

mcp = FastMCP("deskbuddy-timer", log_level="WARNING")

def device(action=None, seconds=None, track=None, volume=None, category=None):
    base = os.getenv("DESKBUDDY_BOARD_URL")
    token = os.getenv("DESKBUDDY_DEVICE_TOKEN")
    if not base or not token:
        return {"ok": False, "error": "没有可用的板端局域网连接"}
    headers = {"Authorization": "Bearer " + token}
    data = None
    if action is not None:
        fields = {"action": action, "seconds": seconds}
        if revision := os.getenv("DESKBUDDY_BOARD_REVISION"):
            fields["revision"] = revision
        data = urlencode(fields).encode()
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    path = "/api/volume" if volume is not None else "/api/music" if track is not None or category else "/api/timer"
    if path != "/api/timer":
        data = urlencode({"level": volume} if volume is not None else {"category": category} if category else {"track": track}).encode()
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    req = Request(base.rstrip("/") + path, data=data, headers=headers)
    try:
        with urlopen(req, timeout=4) as response:
            result = json.load(response)
        if path != "/api/timer":
            return result
        if result.get("ok") is not True or result.get("mode") not in ("idle", "focus", "rest") or type(result.get("seconds")) is not int:
            return {"ok": False, "error": "板端没有返回可确认的计时结果"}
        return result
    except HTTPError as error:
        return {"ok": False, "error": "计时状态已变化，请重试" if error.code == 409 else f"板端拒绝请求，HTTP {error.code}"}
    except (TimeoutError, URLError, ValueError):
        return {"ok": False, "error": "未收到板端确认"}

@mcp.tool()
def start_timer(mode: Literal["focus", "rest"], seconds: Annotated[int, Field(ge=1, le=7200, strict=True)]) -> dict[str, object]:
    """在桌搭上启动或替换倒计时。focus 为专注，rest 为休息；seconds 是秒。缺少时长先追问。"""
    return device(mode, seconds)

@mcp.tool()
def stop_timer() -> dict[str, object]:
    """用户明确要求停止时，结束桌搭上的当前倒计时。"""
    return device("stop", 0)

@mcp.tool()
def get_timer() -> dict[str, object]:
    """查询桌搭上的当前模式和剩余秒数。"""
    return device()

@mcp.tool()
def play_music(track: Annotated[int, Field(ge=1, le=6, strict=True)] = 1) -> dict[str, object]:
    """让桌搭播放板上指定编号的音乐。返回是否已排队。"""
    return device(track=track)

@mcp.tool()
def stop_music() -> dict[str, object]:
    """停止桌搭上的音乐，不影响倒计时。"""
    return device(track=0)

@mcp.tool()
def play_encouragement() -> dict[str, object]:
    """从第 1 或第 6 段鼓励音频中选一段播放。"""
    return device(category="encouragement")

@mcp.tool()
def set_speaker_volume(level: Literal["low", "medium", "high"]) -> dict[str, object]:
    """调整桌搭扬声器总音量：低、中、高，影响语音、音乐和提示音。"""
    return device(volume=level)

if __name__ == "__main__":
    mcp.run(transport="stdio")

