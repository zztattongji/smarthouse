"""
Smarthouse Backend 1 — IoT 核心服务
- 华为云 IoTDA 设备影子 & 命令下发
- 场景联动规则管理
- 通信日志管理
"""

import os
import json
import logging
from typing import Any, Dict, List
from datetime import datetime, timezone, timedelta

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

# ── 华为云 IoTDA SDK ──
from huaweicloudsdkcore.region.region import Region
from huaweicloudsdkcore.exceptions import exceptions as hw_exceptions
from huaweicloudsdkiotda.v5 import (
    IoTDAClient,
    ShowDeviceShadowRequest,
    CreateCommandRequest,
    DeviceCommandRequest,
)
from huaweicloudsdkcore.auth.credentials import BasicCredentials, DerivedCredentials

# ── 日志 ──
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("iot-core")

# ═══════════════════════════════════════════
# App 初始化
# ═══════════════════════════════════════════

app = FastAPI(title="Smarthouse IoT Core", version="3.0.0")

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

IOTDA_AK = os.environ.get("IOTDA_AK", "")
IOTDA_SK = os.environ.get("IOTDA_SK", "")
IOTDA_PROJECT_ID = os.environ.get("IOTDA_PROJECT_ID", "")
IOTDA_REGION = os.environ.get("IOTDA_REGION", "cn-east-3")
IOTDA_ENDPOINT = os.environ.get("IOTDA_ENDPOINT", "")
IOT_BACKEND_URL = os.environ.get("IOT_BACKEND_URL", "http://backend1-iot:3000")

# ═══════════════════════════════════════════
# 内存存储
# ═══════════════════════════════════════════

TZ = timezone(timedelta(hours=8))

_scenes: List[Dict[str, Any]] = [
    {
        "id": "sc_1", "name": "VOC超标自动排风", "enabled": True,
        "condition_sensor": "voc", "condition_operator": ">", "condition_value": 260,
        "action_control": "motor_state", "action_value": "forward",
    },
    {
        "id": "sc_2", "name": "PM2.5超标开净化", "enabled": True,
        "condition_sensor": "pm25", "condition_operator": ">", "condition_value": 50,
        "action_control": "fan_speed", "action_value": 80,
    },
    {
        "id": "sc_3", "name": "夜间自动关灯", "enabled": False,
        "condition_sensor": "light", "condition_operator": "<", "condition_value": 10,
        "action_control": "led_brightness", "action_value": 0,
    },
]

_logs: List[Dict[str, Any]] = []


def _add_log(message: str, type_: str = "system", topic: str = "/sys/backend"):
    entry = {
        "id": f"log_{int(datetime.now(TZ).timestamp() * 1000)}",
        "timestamp": datetime.now(TZ).strftime("%H:%M:%S"),
        "topic": topic,
        "message": message,
        "type": type_,
    }
    _logs.append(entry)
    if len(_logs) > 200:
        _logs.pop(0)
    logger.info(f"[{type_}] {message}")


# ═══════════════════════════════════════════
# 华为云 IoTDA 客户端
# ═══════════════════════════════════════════

def get_iotda_client() -> IoTDAClient:
    credentials = BasicCredentials(IOTDA_AK, IOTDA_SK, IOTDA_PROJECT_ID)
    credentials.with_derived_predicate(DerivedCredentials.get_default_derived_predicate())
    region = Region(IOTDA_REGION, IOTDA_ENDPOINT)
    return (
        IoTDAClient.new_builder()
        .with_credentials(credentials)
        .with_region(region)
        .build()
    )


def extract_shadow_properties(shadow_response: Any) -> Dict[str, Any]:
    d = shadow_response
    if hasattr(d, "to_dict"):
        d = d.to_dict()
    if not isinstance(d, dict):
        return {}
    shadows = d.get("shadow", [])
    if not shadows:
        return {}
    reported = shadows[0].get("reported", {})
    props = reported.get("properties", {})
    if not props and isinstance(reported, dict):
        props = {k: v for k, v in reported.items() if k not in ("event_time",)}
    return props if isinstance(props, dict) else {}


# ═══════════════════════════════════════════
# Pydantic 模型
# ═══════════════════════════════════════════

class CommandBody(BaseModel):
    service_id: str = "actuator"
    command_name: str
    paras: Dict[str, Any] = {}


class SceneCreateBody(BaseModel):
    name: str
    enabled: bool = True
    condition_sensor: str
    condition_operator: str = ">"
    condition_value: float = 0
    action_control: str
    action_value: str = ""


# ═══════════════════════════════════════════
# API 路由
# ═══════════════════════════════════════════

@app.get("/api/health")
def health():
    return {"ok": True, "service": "iot-core"}


# ── 设备影子 ──
@app.get("/api/devices/{device_id}/shadow")
def device_shadow(device_id: str):
    client = get_iotda_client()
    try:
        req = ShowDeviceShadowRequest()
        req.device_id = device_id
        resp = client.show_device_shadow(req)
        props = extract_shadow_properties(resp)
        _add_log(f"获取设备影子成功，提取到 {len(props)} 个属性", "subscribe", f"/iot/{device_id}/shadow")
        return props if props else {"ok": True, "raw": str(resp.to_dict() if hasattr(resp, "to_dict") else resp)}
    except hw_exceptions.ClientRequestException as e:
        _add_log(f"影子获取失败: {e.error_msg}", "alert", f"/iot/{device_id}/error")
        raise HTTPException(
            status_code=502,
            detail={
                "ok": False,
                "error": {
                    "status_code": e.status_code,
                    "error_code": e.error_code,
                    "error_msg": e.error_msg,
                },
            },
        )


# ── 命令下发 ──
@app.post("/api/devices/{device_id}/commands")
def send_command(device_id: str, body: CommandBody):
    client = get_iotda_client()
    try:
        req = CreateCommandRequest()
        req.device_id = device_id
        cmd = DeviceCommandRequest()
        cmd.service_id = body.service_id
        cmd.command_name = body.command_name
        cmd.paras = body.paras
        req.body = cmd
        resp = client.create_command(req)
        _add_log(
            f"下发命令: {body.service_id}/{body.command_name} -> {json.dumps(body.paras, ensure_ascii=False)}",
            "publish",
            f"/iot/{device_id}/commands",
        )
        return {
            "ok": True,
            "data": resp.to_dict() if hasattr(resp, "to_dict") else str(resp),
        }
    except hw_exceptions.ClientRequestException as e:
        _add_log(f"命令下发失败: {e.error_msg}", "alert", f"/iot/{device_id}/error")
        raise HTTPException(
            status_code=502,
            detail={
                "ok": False,
                "error": {
                    "status_code": e.status_code,
                    "error_code": e.error_code,
                    "error_msg": e.error_msg,
                },
            },
        )


# ── 场景联动 CRUD ──

@app.get("/api/scenes")
def list_scenes():
    return {"ok": True, "data": _scenes}


@app.post("/api/scenes")
def add_scene(body: SceneCreateBody):
    import uuid
    scene = {
        "id": f"sc_{uuid.uuid4().hex[:8]}",
        "name": body.name,
        "enabled": body.enabled,
        "condition_sensor": body.condition_sensor,
        "condition_operator": body.condition_operator,
        "condition_value": body.condition_value,
        "action_control": body.action_control,
        "action_value": body.action_value,
    }
    _scenes.append(scene)
    _add_log(f"新增场景规则: {body.name}", "system", "/sys/scenes")
    return {"ok": True, "data": scene}


@app.post("/api/scenes/{scene_id}/toggle")
def toggle_scene(scene_id: str):
    for s in _scenes:
        if s["id"] == scene_id:
            s["enabled"] = not s["enabled"]
            _add_log(f"场景 [{s['name']}] -> {'启用' if s['enabled'] else '禁用'}", "system", "/sys/scenes/toggle")
            return {"ok": True, "data": s}
    raise HTTPException(status_code=404, detail={"ok": False, "error": {"message": "场景不存在"}})


@app.delete("/api/scenes/{scene_id}")
def delete_scene(scene_id: str):
    global _scenes
    target = next((s for s in _scenes if s["id"] == scene_id), None)
    if target:
        _scenes = [s for s in _scenes if s["id"] != scene_id]
        _add_log(f"删除场景: [{target['name']}]", "system", "/sys/scenes/delete")
        return {"ok": True, "data": None}
    raise HTTPException(status_code=404, detail={"ok": False, "error": {"message": "场景不存在"}})


# ── 通信日志 ──

@app.get("/api/logs")
def list_logs(limit: int = 50):
    return {"ok": True, "data": _logs[-limit:]}


@app.post("/api/logs/clear")
def clear_logs():
    _logs.clear()
    _add_log("日志已清空", "system", "/sys/logs")
    return {"ok": True}


# ═══════════════════════════════════════════
# 启动
# ═══════════════════════════════════════════

if __name__ == "__main__":
    import uvicorn
    port = int(os.environ.get("PORT", "3000"))
    logger.info(f"IoT Core starting on http://0.0.0.0:{port}")
    uvicorn.run(app, host="0.0.0.0", port=port)
