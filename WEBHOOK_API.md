# ESP32-C6 局域网告警与语音播报 Webhook 对接文档

本设备连上 Wi-Fi 后会自动在 **80 端口** 启动轻量 HTTP Webhook 服务，供局域网内的 Python Web 系统直接推送中文告警并**语音播报**。

---

## 一、如何随时查询 ESP32-C6 的 IP 地址

1. **直接看屏幕**：主界面天气卡片右上角会实时显示当前局域网 IP（如 `192.168.1.50`）。
2. **串口查询**：USB 串口监视器输入 `ip` 并回车，立即打印当前 IP 和接口地址。
3. **开机日志**：连上 Wi-Fi 后串口自动输出 `Wi-Fi connected! IP=192.168.x.x`。

---

## 二、HTTP 接口一览

| 接口 | 方法 | 用途 |
| :--- | :---: | :--- |
| `/api/status` | `GET` | 查询设备在线状态、IP、电量、当前是否有活跃告警/正在播报 |
| `/api/alert/audio` | `POST` | **推送告警文字 + 中文语音 WAV 播报**（支持循环播报，按 BOOT 键/点屏立即停止） |
| `/api/notify` | `POST` | 推送纯 JSON 文本告警（屏幕弹窗 + 告警提示音） |
| `/api/alert/stop` | `POST` | 远程停止当前语音播报并关闭告警弹窗（适合故障自动恢复场景） |

---

## 三、接口详细说明与 Python 对接示例

### 1. 查询设备状态：`GET /api/status`
```bash
curl http://192.168.1.50/api/status
```
返回示例：
```json
{
  "ok": true,
  "ip": "192.168.1.50",
  "battery": 86,
  "charging": true,
  "alert_active": false,
  "playing": false
}
```

---

### 2. 语音播报告警（推荐）：`POST /api/alert/audio`

Python 端把告警文字合成为 **8kHz 或 16kHz、16-bit、单声道 WAV** 音频，直接作为 HTTP Body 发送，标题/正文/级别通过 URL Query 传递。

- **Query 参数**（UTF-8 URL 编码）：
  - `level`：`error`（红色标题栏）/ `warn`（黄色）/ `info`（蓝色），默认 `error`
  - `title`：告警标题（支持中文，最多 63 字节）
  - `message`：告警正文（支持中文，屏幕自动换行最多 5 行，最多 191 字节）
  - `repeat`：`1`（默认，循环播报直到用户按 `BOOT` 键或点屏停止）/ `0`（仅播报一次）
- **Request Body**：原始 WAV 二进制数据（`Content-Type: audio/wav`）
  - 音频格式：**PCM 16-bit 单声道**，采样率 **8000 Hz（推荐，96KB 可播 6 秒）** 或 **16000 Hz（96KB 可播 3 秒）**
  - 大小限制：`45 ~ 98304` 字节（≤ 96 KB）

#### Python 完整示例（使用免费 `edge-tts` + `ffmpeg` 合成中文语音并推送）

```bash
pip install requests edge-tts
# 系统需安装 ffmpeg（用于将 mp3 转为 8kHz 16-bit 单声道 WAV）
```

```python
import asyncio
import subprocess
import tempfile
import os
import requests
import edge_tts

ESP32_IP = "192.168.1.50"

async def text_to_wav_bytes(text: str, sample_rate: int = 8000) -> bytes:
    """用微软 Edge 免费中文女声合成 8kHz 16-bit 单声道 WAV（6秒以内 <= 96KB）"""
    with tempfile.TemporaryDirectory() as td:
        mp3_path = os.path.join(td, "tts.mp3")
        wav_path = os.path.join(td, "tts.wav")
        communicate = edge_tts.Communicate(text, voice="zh-CN-XiaoxiaoNeural", rate="+10%")
        await communicate.save(mp3_path)

        subprocess.run(
            ["ffmpeg", "-y", "-i", mp3_path, "-ar", str(sample_rate), "-ac", "1", "-acodec", "pcm_s16le", wav_path],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        with open(wav_path, "rb") as f:
            return f.read()

def send_voice_alert(title: str, message: str, level: str = "error", repeat: bool = True):
    speech_text = f"警告，{title}，{message}"
    wav_bytes = asyncio.run(text_to_wav_bytes(speech_text, sample_rate=8000))
    if len(wav_bytes) > 98304:
        wav_bytes = wav_bytes[:98304]  # 截断到 96KB 上限（约 6 秒）

    resp = requests.post(
        f"http://{ESP32_IP}/api/alert/audio",
        params={
            "level": level,
            "title": title,
            "message": message,
            "repeat": "1" if repeat else "0",
        },
        data=wav_bytes,
        headers={"Content-Type": "audio/wav"},
        timeout=5,
    )
    print("ESP32 response:", resp.status_code, resp.text)

if __name__ == "__main__":
    send_voice_alert("数据库告警", "主库连接超时，请立即排查", level="error", repeat=True)
```

> **纯离线 TTS 备选**：如果你的 Python 服务器不能连外网，可以用 `pyttsx3`（调用 Windows 系统自带语音）直接导出 WAV，再用 `ffmpeg` 或 Python `wave`/`audioop` 转为 8kHz 16-bit 单声道即可。

---

### 3. 纯文本弹窗告警：`POST /api/notify`

不需要合成语音时，直接发 JSON（ESP32 会弹出中文卡片并循环播放告警提示音）：

```python
import requests

requests.post(
    "http://192.168.1.50/api/notify",
    json={
        "level": "warn",          # "info" / "warn" / "error"
        "title": "磁盘空间预警",
        "message": "/data 分区使用率已达 92%",
        "repeat": True            # True=循环响铃直到按键停止；False=响一次
    },
    timeout=3,
)
```

---

### 4. 远程结束播报：`POST /api/alert/stop`

当 Web 系统检测到告警自动恢复时，可主动取消板子上的播报与弹窗：

```python
requests.post("http://192.168.1.50/api/alert/stop", timeout=3)
```

---

## 四、设备端操作说明

- **结束语音播报**：
  1. **按下侧边物理 `BOOT (GPIO9)` 按键**（16ms 内立刻停止语音并关闭功放）；
  2. 或**点击屏幕上的绿色 `STOP` 按钮 / 屏幕任意位置**；
  3. 或由 Web 系统调用 `POST /api/alert/stop`。
- **新告警覆盖**：若上一条告警尚未消除时收到新告警，设备会自动中断旧播报并立即播报最新告警。
