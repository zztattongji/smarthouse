"""
Smarthouse Backend 2 — AI 助手服务
- AI 大模型对话（OpenAI 兼容接口）
- 语音识别（WebSocket 流式 + HTTP 上传）
- 调用 IoT 后端执行设备控制
"""

import re
import os
import json
import base64
import logging
from typing import Any, Dict, Optional

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("ai-assistant")

# ═══════════════════════════════════════════
# App
# ═══════════════════════════════════════════

app = FastAPI(title="Smarthouse AI Assistant", version="3.0.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# ═══════════════════════════════════════════
# 配置
# ═══════════════════════════════════════════

AI_API_KEY = os.environ.get("AI_API_KEY", "")
AI_API_BASE = os.environ.get("AI_API_BASE", "https://api.openai.com/v1")
AI_MODEL = os.environ.get("AI_MODEL", "gpt-3.5-turbo")
ASR_API_KEY = os.environ.get("ASR_API_KEY", "")
ASR_API_BASE = os.environ.get("ASR_API_BASE", "")
IOT_BACKEND_URL = os.environ.get("IOT_BACKEND_URL", "http://backend1-iot:3000")

# ═══════════════════════════════════════════
# AI System Prompt
# ═══════════════════════════════════════════

SYSTEM_PROMPT = """你是 Smarthouse 智能家居系统的 AI 智能管家。你的职责包括：

1. **天气查询**：用户问天气时，结合传感器传来的实际室内温度、湿度、气压数据，给出「室外预报 + 室内实测」的对比和建议。

2. **环境分析**：根据当前传感器数据（温度、湿度、气压、光照、VOC、eCO2、PM2.5），判断室内环境是否健康舒适，给出简要评价和改善建议。

3. **设备控制**：用户要求开关设备时，理解意图并给出确认。常用设备：
   LED灯(set_led 0-100)、风扇(set_fan 0-100)、电机(set_motor forward/reverse/stop)、
   继电器(set_relay true/false)、舵机(set_servo 0-180)、蜂鸣器(set_buzzer true/false)。

4. **异常告警**：PM2.5 > 75 或 VOC > 260 或温度 > 35°C 时，主动提醒并建议措施。

5. **语音消息理解**：如果用户消息是"[语音消息]"开头，说明这是语音识别转写的结果，请友好回应。

回复控制在 100-200 字，友好简洁。如需控制设备，末尾附加一行：
[DEVICE_CMD]{"command": "set_led", "paras": {"brightness": 80}}
"""

# ═══════════════════════════════════════════
# AI 调用
# ═══════════════════════════════════════════

try:
    from openai import OpenAI as OpenAIClient
    _openai_available = True
except ImportError:
    _openai_available = False
    import httpx


async def call_ai_model_text(user_content: str) -> str:
    if _openai_available:
        return await _call_via_openai_sdk(user_content)
    else:
        return await _call_via_httpx(user_content)


async def _call_via_openai_sdk(user_content: str) -> str:
    client = OpenAIClient(api_key=AI_API_KEY, base_url=AI_API_BASE)
    try:
        resp = client.chat.completions.create(
            model=AI_MODEL,
            messages=[
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": user_content},
            ],
            temperature=0.7,
            max_tokens=500,
        )
        return resp.choices[0].message.content or "（AI 返回为空）"
    except Exception as e:
        logger.error(f"AI call failed (SDK): {e}")
        raise


async def _call_via_httpx(user_content: str) -> str:
    import httpx
    url = f"{AI_API_BASE.rstrip('/')}/chat/completions"
    headers = {
        "Authorization": f"Bearer {AI_API_KEY}",
        "Content-Type": "application/json",
    }
    payload = {
        "model": AI_MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": user_content},
        ],
        "temperature": 0.7,
        "max_tokens": 500,
    }
    async with httpx.AsyncClient(timeout=30.0) as client:
        resp = await client.post(url, json=payload, headers=headers)
    if resp.status_code != 200:
        logger.error(f"AI call failed (httpx): {resp.status_code} {resp.text[:200]}")
        raise RuntimeError(f"AI API returned {resp.status_code}")
    data = resp.json()
    return data["choices"][0]["message"]["content"] or "（AI 返回为空）"


async def call_asr(audio_bytes: bytes, fmt: str = "pcm") -> str:
    """调用语音识别服务，返回识别文字。未配置 ASR 时返回空字符串。"""
    if not ASR_API_KEY or not ASR_API_BASE:
        logger.warning("ASR not configured, skipping speech recognition")
        return ""
    import httpx
    try:
        b64 = base64.b64encode(audio_bytes).decode("utf-8")
        async with httpx.AsyncClient(timeout=30.0) as client:
            resp = await client.post(
                f"{ASR_API_BASE.rstrip('/')}/recognize",
                json={"audio": b64, "format": fmt, "language": "zh-CN"},
                headers={"Authorization": f"Bearer {ASR_API_KEY}"},
            )
        if resp.status_code == 200:
            data = resp.json()
            return data.get("text", "")
        logger.error(f"ASR failed: {resp.status_code}")
        return ""
    except Exception as e:
        logger.error(f"ASR call error: {e}")
        return ""


# ═══════════════════════════════════════════
# 本地回退
# ═══════════════════════════════════════════

def build_local_fallback(message: str, sensor_context: str) -> str:
    temp_match = re.search(r"温度[:：]\s*([\d.]+)", sensor_context)
    hum_match = re.search(r"湿度[:：]\s*([\d.]+)", sensor_context)
    pm25_match = re.search(r"PM2\.?5[:：]\s*([\d.]+)", sensor_context)
    voc_match = re.search(r"VOC[:：]\s*([\d.]+)", sensor_context)

    parts = ["📊 当前环境数据："]
    if temp_match:
        parts.append(f"• 温度 {temp_match.group(1)}°C")
    if hum_match:
        parts.append(f"• 湿度 {hum_match.group(1)}%")
    if pm25_match:
        pm = float(pm25_match.group(1))
        parts.append(f"• PM2.5 {pm}μg/m³ {'⚠ 偏高' if pm > 35 else '✅ 正常'}")
    if voc_match:
        v = float(voc_match.group(1))
        parts.append(f"• VOC {v}ppb {'⚠ 超标' if v > 260 else '✅ 正常'}")

    parts.append(f"\n💬 你问的是：「{message}」")
    parts.append("\n⚠ AI 模型未配置（请设置环境变量 AI_API_KEY），以上为本地分析结果。")
    return "\n".join(parts)


# ═══════════════════════════════════════════
# Pydantic 模型
# ═══════════════════════════════════════════

class AIQueryBody(BaseModel):
    message: str = ""
    sensor_context: str = ""
    audio_base64: str = ""


class AudioBody(BaseModel):
    audio_base64: str
    format: str = "pcm"
    sensor_context: str = ""


# ═══════════════════════════════════════════
# API 路由
# ═══════════════════════════════════════════

@app.get("/api/health")
def health():
    return {
        "ok": True,
        "service": "ai-assistant",
        "ai_available": _openai_available,
        "asr_configured": bool(ASR_API_KEY and ASR_API_BASE),
    }


@app.get("/api/ai/health")
def ai_health():
    return health()


@app.post("/api/ai/query")
async def ai_query(body: AIQueryBody):
    """AI 大模型对话（支持文字 + base64 语音）"""
    if not AI_API_KEY:
        logger.info("AI not configured, returning local fallback")
        return build_local_fallback(body.message, body.sensor_context)

    try:
        if body.audio_base64:
            user_content = f"[语音消息] 用户发来了一段语音。如果你能理解语音内容，请回应。当前传感器数据：{body.sensor_context}"
        else:
            user_content = f"当前传感器数据：\n{body.sensor_context}\n\n用户：{body.message}"

        reply = await call_ai_model_text(user_content)
        logger.info(f"AI reply: {reply[:80]}...")
        return reply
    except Exception as e:
        logger.error(f"AI query failed: {e}")
        return build_local_fallback(body.message, body.sensor_context)


@app.post("/api/voice/recognize")
async def voice_recognize(body: AudioBody):
    """HTTP 方式上传音频进行语音识别 + AI 对话"""
    try:
        audio_bytes = base64.b64decode(body.audio_base64)
    except Exception:
        raise HTTPException(status_code=400, detail={"ok": False, "error": {"message": "invalid base64 audio"}})

    # ASR 识别
    text = await call_asr(audio_bytes, body.format)
    if not text:
        return {"ok": True, "asr_text": "", "reply": "语音识别未配置或识别失败，请检查 ASR 配置。"}

    # AI 理解
    if not AI_API_KEY:
        return {"ok": True, "asr_text": text, "reply": build_local_fallback(text, body.sensor_context)}

    try:
        user_content = f"当前传感器数据：\n{body.sensor_context}\n\n用户（语音转写）：{text}"
        reply = await call_ai_model_text(user_content)
        return {"ok": True, "asr_text": text, "reply": reply}
    except Exception as e:
        logger.error(f"AI voice query failed: {e}")
        return {"ok": True, "asr_text": text, "reply": build_local_fallback(text, body.sensor_context)}


# ═══════════════════════════════════════════
# WebSocket 流式语音识别
# ═══════════════════════════════════════════

@app.websocket("/ws/voice")
async def websocket_voice(websocket: WebSocket):
    """
    WebSocket 流式语音识别端点。
    客户端发送二进制音频帧，服务端累积后识别。
    发送 JSON: {"type": "start", "format": "pcm", "sample_rate": 16000}
    发送 binary: 音频数据块
    发送 JSON: {"type": "end", "sensor_context": "..."}
    接收 JSON: {"type": "partial", "text": "..."} 或 {"type": "final", "text": "...", "reply": "..."}
    """
    await websocket.accept()
    audio_chunks: list = []
    audio_format = "pcm"
    sensor_context = ""

    try:
        while True:
            data = await websocket.receive()

            if "text" in data:
                msg = json.loads(data["text"])
                msg_type = msg.get("type", "")

                if msg_type == "start":
                    audio_format = msg.get("format", "pcm")
                    audio_chunks = []
                    logger.info(f"Voice WS: start, format={audio_format}")

                elif msg_type == "end":
                    sensor_context = msg.get("sensor_context", "")
                    logger.info(f"Voice WS: end, {len(audio_chunks)} chunks received")

                    if not audio_chunks:
                        await websocket.send_json({"type": "error", "message": "no audio data"})
                        continue

                    # 合并音频
                    full_audio = b"".join(audio_chunks)

                    # ASR 识别
                    text = await call_asr(full_audio, audio_format)

                    if not text:
                        await websocket.send_json({
                            "type": "final",
                            "asr_text": "",
                            "reply": "语音识别未配置或识别失败。",
                        })
                        continue

                    # AI 理解
                    if not AI_API_KEY:
                        reply = build_local_fallback(text, sensor_context)
                    else:
                        try:
                            user_content = f"当前传感器数据：\n{sensor_context}\n\n用户（语音转写）：{text}"
                            reply = await call_ai_model_text(user_content)
                        except Exception:
                            reply = build_local_fallback(text, sensor_context)

                    await websocket.send_json({
                        "type": "final",
                        "asr_text": text,
                        "reply": reply,
                    })
                    audio_chunks = []

                elif msg_type == "ping":
                    await websocket.send_json({"type": "pong"})

            elif "bytes" in data:
                audio_chunks.append(data["bytes"])

    except WebSocketDisconnect:
        logger.info("Voice WS: client disconnected")
    except Exception as e:
        logger.error(f"Voice WS error: {e}")
        try:
            await websocket.send_json({"type": "error", "message": str(e)})
        except Exception:
            pass


# ═══════════════════════════════════════════
# 启动
# ═══════════════════════════════════════════

if __name__ == "__main__":
    import uvicorn
    port = int(os.environ.get("PORT", "5000"))
    logger.info(f"AI Assistant starting on http://0.0.0.0:{port}")
    logger.info(f"AI Model: {AI_MODEL} @ {AI_API_BASE}")
    logger.info(f"AI configured: {bool(AI_API_KEY)}")
    logger.info(f"ASR configured: {bool(ASR_API_KEY and ASR_API_BASE)}")
    uvicorn.run(app, host="0.0.0.0", port=port)
