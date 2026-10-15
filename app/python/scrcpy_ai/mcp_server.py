"""MCP server exposing scrcpy web-route devices as tools.

Runs over stdio. Each tool targets one of the running C backends
(scrcpy-web:18080 / scrcpy-isharing:18081 / scrcpy-kakao:18082).

Coordinate system: tools take/return SCREENSHOT coordinates. Take a
screenshot first (the response includes the size), then click/swipe using
those coordinates. The C backend clamps and maps them to the device frame.

Run:  ai-venv/bin/python -m scrcpy_ai.mcp_server
"""

import io
import logging
import os
import time

import httpx
from mcp.server.mcpserver import MCPServer
from mcp.server.mcpserver.utilities.types import Image
from mcp.server.mcpserver.tools.base import ToolError
from mcp.types import ToolAnnotations

# device name -> C backend port. Keep in sync with main.py WS slots.
DEVICES = {
    "phone": ("Z-Flip4 (USB)", 18080),
    "isharing": ("Redroid-iSharing", 18081),
    "kakao": ("Redroid-Kakao", 18082),
}
DEFAULT_DEVICE = "phone"

# Screenshot files are served to clients by path; purge them after a day.
SHOT_DIR = "/tmp/scrcpy-ai-shots"
SHOT_TTL = 24 * 3600


def _purge_old_shots() -> None:
    """Delete screenshot files older than SHOT_TTL (best effort)."""
    try:
        now = time.time()
        for name in os.listdir(SHOT_DIR):
            p = os.path.join(SHOT_DIR, name)
            if os.path.isfile(p) and now - os.path.getmtime(p) > SHOT_TTL:
                os.unlink(p)
    except OSError:
        pass

# stdio is our transport; never write logs to stdout
logging.basicConfig(level=logging.WARNING, stream=open("/tmp/scrcpy_mcp.log", "a"))

server = MCPServer(
    name="scrcpy",
    title="scrcpy device control",
    description="See and control Android devices mirrored by scrcpy.",
    instructions=(
        "Take a screenshot to see the screen (returns a JPEG image and its "
        "size in pixels). Then act using screenshot-pixel coordinates: "
        "click/long_press/swipe take x/y in the same coordinate space as the "
        "screenshot. After important actions, take another screenshot to "
        "verify the result. Use the 'device' argument to select which device "
        "to control (" + ", ".join(f"{k}={v[0]}" for k, v in DEVICES.items()) + ")."
    ),
)

_client = httpx.Client(timeout=httpx.Timeout(10.0, read=15.0))


def _base_url(device: str) -> str:
    if device not in DEVICES:
        raise ToolError(
            f"unknown device '{device}'. Available: {', '.join(DEVICES)}"
        )
    return f"http://127.0.0.1:{DEVICES[device][1]}"


def _post(device: str, path: str, payload: dict) -> str:
    """POST JSON, return the backend's JSON body as text (raises ToolError)."""
    try:
        r = _client.post(_base_url(device) + path, json=payload)
    except httpx.HTTPError as e:
        raise ToolError(f"cannot reach scrcpy backend for '{device}': {e}") from e
    body = r.text
    if r.status_code != 200 or '"error"' in body:
        raise ToolError(f"{path} failed (HTTP {r.status_code}): {body}")
    return body


@server.tool(
    name="screenshot",
    description="Capture the current screen of the device as a JPEG image. "
                "Always call this first to see the screen. The image size "
                "(width/height) defines the pixel coordinate space used by "
                "click/long_press/swipe.",
    annotations=ToolAnnotations(readOnlyHint=True, openWorldHint=False),
)
def screenshot(device: str = DEFAULT_DEVICE) -> list:
    """Capture the screen; retry on transient backend encode failures."""
    last_err = ""
    for attempt in range(3):
        try:
            r = _client.get(_base_url(device) + "/internal/screenshot")
        except httpx.HTTPError as e:
            raise ToolError(f"cannot reach scrcpy backend for '{device}': {e}") from e
        if r.status_code == 200:
            from PIL import Image as PILImage
            w, h = PILImage.open(io.BytesIO(r.content)).size
            # Persist to a file so clients that drop image blocks (e.g. CLI)
            # can still fetch the JPEG from the returned path.
            os.makedirs(SHOT_DIR, exist_ok=True)
            _purge_old_shots()
            path = os.path.join(SHOT_DIR, f"{device}_{int(time.time())}.jpg")
            with open(path, "wb") as f:
                f.write(r.content)
            return [
                Image(data=r.content, format="jpeg"),
                f"saved: {path} width={w} height={h} "
                f"(use these bounds for click/swipe coordinates)",
            ]
        last_err = f"HTTP {r.status_code}: {r.text[:200]}"
        if r.status_code == 500:  # transient encode failure on a stale frame
            time.sleep(0.8)
            continue
        break
    raise ToolError(f"screenshot failed ({last_err})")


@server.tool(
    name="screen_info",
    description="Get the device screen size in pixels "
                "(screenshot size and raw video frame size).",
    annotations=ToolAnnotations(readOnlyHint=True, openWorldHint=False),
)
def screen_info(device: str = DEFAULT_DEVICE) -> str:
    try:
        r = _client.get(_base_url(device) + "/internal/info")
    except httpx.HTTPError as e:
        raise ToolError(f"cannot reach scrcpy backend for '{device}': {e}") from e
    if r.status_code != 200:
        raise ToolError(f"info failed (HTTP {r.status_code}): {r.text[:200]}")
    return r.text


@server.tool(
    name="click",
    description="Tap at (x, y) in screenshot pixel coordinates.",
    annotations=ToolAnnotations(destructiveHint=False, openWorldHint=False),
)
def click(x: int, y: int, device: str = DEFAULT_DEVICE) -> str:
    return _post(device, "/internal/click", {"x": x, "y": y})


@server.tool(
    name="long_press",
    description="Long-press at (x, y) in screenshot pixel coordinates.",
    annotations=ToolAnnotations(openWorldHint=False),
)
def long_press(x: int, y: int, duration_ms: int = 500,
               device: str = DEFAULT_DEVICE) -> str:
    return _post(device, "/internal/long_press",
                 {"x": x, "y": y, "duration_ms": duration_ms})


@server.tool(
    name="swipe",
    description="Swipe from (x1, y1) to (x2, y2) in screenshot pixel "
                "coordinates, over duration_ms milliseconds.",
    annotations=ToolAnnotations(openWorldHint=False),
)
def swipe(x1: int, y1: int, x2: int, y2: int, duration_ms: int = 300,
          device: str = DEFAULT_DEVICE) -> str:
    return _post(device, "/internal/swipe",
                 {"x1": x1, "y1": y1, "x2": x2, "y2": y2,
                  "duration_ms": duration_ms})


# Common Android keycodes for convenience
_KEYS = {
    "back": 4, "home": 3, "app_switch": 187, "power": 26,
    "volume_up": 24, "volume_down": 25, "enter": 66, "escape": 111,
    "tab": 61, "space": 62, "del": 67,
}


@server.tool(
    name="key_press",
    description="Press an Android key. Use a name (back, home, app_switch, "
                "power, volume_up, volume_down, enter, escape, tab, space, "
                "del) or a raw AKEYCODE_ keycode number.",
    annotations=ToolAnnotations(openWorldHint=False),
)
def key_press(key: str | int, device: str = DEFAULT_DEVICE) -> str:
    key = str(key).strip().lower()
    if key in _KEYS:
        keycode = _KEYS[key]
    else:
        try:
            keycode = int(key)
        except ValueError:
            raise ToolError(
                f"unknown key '{key}'. Use one of {sorted(_KEYS)} or a number."
            ) from None
    return _post(device, "/internal/key", {"keycode": keycode})


@server.tool(
    name="input_text",
    description="Type a text string into the focused input field. Only ASCII "
                "is reliably supported by the backend.",
    annotations=ToolAnnotations(openWorldHint=False),
)
def input_text(text: str, device: str = DEFAULT_DEVICE) -> str:
    return _post(device, "/internal/text", {"text": text})


@server.tool(
    name="list_devices",
    description="List the Android devices available for control, with their "
                "device id (for the 'device' argument of other tools).",
    annotations=ToolAnnotations(readOnlyHint=True, openWorldHint=False),
)
def list_devices() -> str:
    lines = []
    for name, (label, port) in DEVICES.items():
        try:
            _client.get(f"http://127.0.0.1:{port}/internal/info", timeout=2.0)
            status = "online"
        except httpx.HTTPError:
            status = "offline"
        marker = " (default)" if name == DEFAULT_DEVICE else ""
        lines.append(f"{name}: {label} [port {port}] — {status}{marker}")
    return "\n".join(lines)


if __name__ == "__main__":
    import asyncio
    asyncio.run(server.run_stdio_async())
