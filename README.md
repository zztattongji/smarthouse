# 基于 CI1302 与 WS63 的多节点语音智能家居系统

本仓库整理了一个面向智能家居场景的语音 AIoT 原型系统代码与项目报告。系统以 CI1302 + WS63 语音中枢为交互入口，结合 BearPi-Pico H3863 外部节点、移动端/后端服务和华为云 IoTDA，实现语音控制、环境检测、设备执行、云端状态管理与 App 远程控制等功能。

> 说明：本仓库只保存二次开发代码、配置补丁和项目文档，不包含完整厂商 SDK、工具链、编译输出、固件包或任何真实密钥。

## 项目目标

项目希望在保留原有离在线语音交互能力的基础上，扩展 WS63 与外部智能家居节点之间的无线控制通信。整体思路是：CI1302 负责语音唤醒、离线识别和播报，WS63 负责联网、语义结果处理和控制命令转发，外部节点负责环境采集和设备执行，App 与华为云负责远程管理和状态同步。

系统设计中引入了一套统一 JSON/语义控制协议，用结构化字段描述设备类型、动作、参数、状态和命令来源，避免不同板卡之间“各说各话”，也方便后续扩展更多智能家居设备。

## 系统架构

```text
用户语音 / App 控制 / 云端场景
        |
        v
CI1302 + WS63 语音 AIoT 中枢
        |
        v
统一 JSON / 语义控制协议
        |
        +--> 网关节点：WiFi / SLE 通信与消息转发
        +--> 检测节点：温湿度、气压、空气质量、OLED 显示
        +--> 执行节点：LED、风扇、电机、舵机、继电器、蜂鸣器
        |
        v
App 后端 / 华为云 IoTDA / AI 服务
```

## 目录结构

```text
.
├── app_cloud/
│   ├── backend1_iot/          # 华为云 IoTDA 设备影子、命令下发、场景和日志服务
│   ├── backend2_ai/           # AI 对话、语音识别接口和设备控制意图解析
│   ├── nginx/                 # 反向代理配置
│   ├── docker-compose.yml
│   └── .env.example           # 环境变量模板，不包含真实密钥
├── boards_bearpi_h3863/
│   ├── application/samples/wifi/smarthouse_gateway/       # 网关节点代码
│   ├── application/samples/peripheral/smarthouse_sensor/   # 检测节点代码
│   ├── application/samples/peripheral/smarthouse_actuator/ # 执行节点代码
│   └── build/config/.../ws63_liteos_app.config             # 参考 menuconfig 配置
├── ci1302_voice_module/
│   └── README.md                                           # CI1302 语音芯片角色、协议和烧录说明
├── ws63_voice_center/
│   └── application/samples/peripheral/smarthouse_ai/       # WS63 语音中枢侧二次开发代码
├── sensor.jpg                                              # 检测节点/传感器相关实物图片
├── zhixing.jpg                                             # 执行节点相关实物图片
└── docs/
    └── 基于CI1302与WS63的多节点语音智能家居系统项目报告.docx
```

## CI1302 与 WS63 分工

本项目中的语音中枢由 CI1302 和 WS63 两部分组成：

- CI1302：负责语音唤醒、离线识别、本地播报，以及通过串口向 WS63 上报识别结果或状态事件。
- WS63：负责联网、协议解析、语义映射、外部节点通信和云端/App 控制链路。

由于 CI1302 原厂 SDK、语音模型和固件资源可能涉及厂商授权，仓库不直接上传完整 CI1302 SDK，只在 `ci1302_voice_module/` 中保留接口说明和联调记录。

## 主要功能

- 语音控制：基于 CI1302 进行语音唤醒、离线命令识别和语音播报，WS63 侧负责接收语义事件并转换为控制命令。
- 多节点通信：网关节点负责 WiFi/SLE 通信组织，检测节点和执行节点通过统一协议参与系统联动。
- 环境检测：检测节点支持温湿度、气压、空气质量等数据采集，并保留 OLED 本地显示能力。
- 设备执行：执行节点支持 LED、风扇、电机、舵机、继电器、蜂鸣器等常见智能家居执行器控制。
- 云端接入：后端服务对接华为云 IoTDA，提供设备影子查询、命令下发、场景联动和日志记录。
- AI 服务：提供文本/语音查询接口，可将自然语言中的控制意图转化为设备命令。

## 编译与烧录说明

本仓库不是完整 SDK，不能单独直接编译。使用时需要将对应目录复制到原厂 SDK 的相同路径下，再通过 HiSpark Studio 或厂商工具进行编译烧录。

### 1. 外部节点 SDK

外部 1、2、3 号节点对应 SDK 位于本地：

```text
D:\hispark_code\bearpi-pico_h3863
```

参考勾选方式：

- 网关节点：`Application -> Enable Sample -> Enable the Sample of WIFI -> Support Smarthouse Gateway Board`，并启用 `Enable the Sample of products -> Support SLE GATEWAY sample -> Enable SLE GATEWAY Server sample`。
- 检测节点：启用 `Enable peripheral -> Support Smarthouse Sensor`，并根据实际传感器选择 I2C/UART/OLED 相关能力。
- 执行节点：启用 `Enable peripheral -> Support Smarthouse Actuator`。

### 2. 语音中枢 WS63 侧

WS63 侧代码位于：

```text
ws63_voice_center/application/samples/peripheral/smarthouse_ai
```

该部分用于尝试解析 CI1302 侧语义事件，并将控制意图转发给外部智能家居节点。需要注意，CI1302 + WS63 原厂离在线对话方案可能包含私有协议、握手、联网大模型接入和状态同步逻辑。如果直接替换 WS63 固件，可能导致原有对话能力失效。因此建议在获得原厂协议文档或二次开发接口后继续联调。

## App 与云端配置

进入 `app_cloud/` 后复制环境变量模板：

```bash
cp .env.example .env
```

然后在本地 `.env` 中填写真实配置：

```env
IOTDA_AK=
IOTDA_SK=
IOTDA_PROJECT_ID=
IOTDA_REGION=cn-east-3
IOTDA_ENDPOINT=
AI_API_KEY=
AI_API_BASE=https://api.openai.com/v1
AI_MODEL=gpt-4o-mini
```

`.env` 已加入 `.gitignore`，不要把真实 AK/SK、API Key、MQTT 密码或设备密钥提交到仓库。

## 统一 JSON/语义协议

控制消息建议统一为类似结构：

```json
{
  "seq": 1024,
  "source": "voice",
  "device": "fan",
  "action": "on",
  "params": {
    "speed": 2
  },
  "state": "requested"
}
```

核心字段说明：

- `seq`：消息序号，用于请求响应追踪。
- `source`：命令来源，例如 `voice`、`app`、`cloud`。
- `device`：设备类型或设备 ID，例如 `light`、`fan`、`servo`。
- `action`：控制动作，例如 `on`、`off`、`set_angle`。
- `params`：动作参数，例如亮度、角度、档位等。
- `state`：状态描述，例如 `requested`、`running`、`error`。

## 当前进展

- 外部 1、2、3 号智能家居节点代码已完成编译验证。
- WS63 侧固件曾完成烧录验证。
- 检测节点已调整为本地检测优先模式，便于先验证 OLED 与传感器能力。
- App/云端侧已搭建华为云 IoTDA 与 AI 服务基础结构。
- CI1302 与 WS63 原厂离在线对话链路仍需结合原厂协议或示例工程继续确认。

## 安全说明

仓库中的所有 WiFi、MQTT、华为云和 AI API 配置均已替换为占位符或环境变量。若本地曾经硬编码过真实密钥，建议在对应平台重新生成密钥后再继续使用。

## 项目报告

项目报告位于：

```text
docs/基于CI1302与WS63的多节点语音智能家居系统项目报告.docx
```

报告内容包括作品概述、系统组成、硬件与软件设计、功能测试、当前问题分析和后续改进方向。

## 实物图片

仓库根目录保留了两张项目实物图片：

- `sensor.jpg`：检测节点或传感器板相关实物图。
- `zhixing.jpg`：执行节点相关实物图。

##  说明
老师您好，我们的 PCB 已完成焊接，但调试时出现部分故障；因排查所需电表尚未到货，时间上来不及进一步定位和修复。恳请老师结合我们已完成的设计、代码和调试工作，适当给予一些过程分。
