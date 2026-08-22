from dataclasses import dataclass


@dataclass
class Config:
    # C backend (scrcpy internal API) — proxied for /ws/video and /ws/control
    scrcpy_host: str = "127.0.0.1"
    scrcpy_port: int = 18080

    # Python web server
    web_port: int = 8080

    # State directory (holds the TOTP secret)
    db_dir: str = ""  # set in __post_init__

    @property
    def scrcpy_url(self) -> str:
        return f"http://{self.scrcpy_host}:{self.scrcpy_port}"

    def __post_init__(self):
        import os
        base = os.path.expanduser("~/.scrcpy_ai")
        if not self.db_dir:
            self.db_dir = os.path.join(base, "db")


config = Config()
