#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
T5AI 语音对话板 · API 链路自检

依次验证三条通道：
  1. REST LLM     : qwen-plus（compatible-mode）
  2. REST ASR     : qwen-audio-3.1-asr-flash（multimodal-generation，base64 直传）
  3. Realtime WS  : qwen-audio-3.1-realtime-flash（server VAD 自动断句，全双工）

用法：
    python3 api_check.py            # 全量自检
    python3 api_check.py --asr      # 只测 ASR
    python3 api_check.py --audio /path/to/16k_mono.wav

说明：
  - 实时通道必须带 Origin: https://dashscope.aliyuncs.com，否则网关会直接断连。
  - 一轮对话必须靠 server VAD 收尾：推完语音后补约 2 秒静音，不要手动 commit。
"""

import argparse
import base64
import io
import json
import os
import sys
import time
import wave

try:
    import requests
except ImportError:
    sys.exit("需要 requests：pip3 install requests")

try:
    import websocket  # websocket-client
except ImportError:
    sys.exit("需要 websocket-client：pip3 install websocket-client")

API_KEY = os.environ.get("DASHSCOPE_API_KEY", "")
if not API_KEY:
    sys.exit("缺少环境变量 DASHSCOPE_API_KEY，请先 export DASHSCOPE_API_KEY=sk-xxxx")
HOST = "llm-rwc82wtulg6ogzm2.cn-beijing.maas.aliyuncs.com"
BASE = "https://" + HOST
WS_URL = ("wss://" + HOST +
          "/api-ws/v1/realtime?model=qwen-audio-3.1-realtime-flash")
CHAT_URL = BASE + "/compatible-mode/v1/chat/completions"
ASR_URL = BASE + "/api/v1/services/aigc/multimodal-generation/generation"
GATEWAY_ORIGIN = "https://dashscope.aliyuncs.com"

DEFAULT_AUDIO = "/tmp/q.wav"
VOICE = "longanqian"


def head(title):
    print("\n" + "─" * 52)
    print(title)
    print("─" * 52)


def load_pcm(path):
    """读 16k 单声道 16bit wav，返回 raw pcm16 bytes。"""
    with wave.open(path, "rb") as w:
        rate, ch, width, frames = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
        data = w.readframes(frames)
    if rate != 16000 or ch != 1 or width != 2:
        raise ValueError("需要 16kHz / 单声道 / 16bit wav，实际 %d/%d/%d" % (rate, ch, width))
    return data


def check_llm():
    head("1. REST LLM · qwen-plus")
    body = {
        "model": "qwen-plus",
        "messages": [
            {"role": "system", "content": "你是一个简短实用的中文语音助手，回答口语化，控制在60字以内。"},
            {"role": "user", "content": "用一句话介绍杭州。"},
        ],
        "temperature": 0.5,
    }
    t0 = time.time()
    r = requests.post(CHAT_URL, headers={
        "Authorization": "Bearer " + API_KEY,
        "Content-Type": "application/json",
    }, json=body, timeout=40)
    cost = time.time() - t0
    if r.status_code != 200:
        print("FAIL  HTTP %d  %s" % (r.status_code, r.text[:200]))
        return False
    text = r.json()["choices"][0]["message"]["content"].strip()
    print("PASS  %.1fs  →  %s" % (cost, text))
    return True


def check_asr(audio_path):
    head("2. REST ASR · qwen-audio-3.1-asr-flash")
    pcm = load_pcm(audio_path)
    b64 = base64.b64encode(pcm).decode()
    body = {
        "model": "qwen-audio-3.1-asr-flash",
        "input": {
            "messages": [
                {"role": "user", "content": [
                    {"audio": "data:audio/wav;base64," + b64}
                ]}
            ]
        },
        "parameters": {"format": "pcm", "sample_rate": "16000"},
    }
    t0 = time.time()
    r = requests.post(ASR_URL, headers={
        "Authorization": "Bearer " + API_KEY,
        "Content-Type": "application/json",
        "X-DashScope-SSE": "disable",
    }, json=body, timeout=60)
    cost = time.time() - t0
    if r.status_code != 200:
        print("FAIL  HTTP %d  %s" % (r.status_code, r.text[:200]))
        return False
    data = r.json()
    root = data.get("output", {}).get("sentence") or {}
    text = root.get("text", "")
    if not text:
        print("FAIL  未解析到文本：%s" % json.dumps(data, ensure_ascii=False)[:260])
        return False
    print("PASS  %.1fs  →  %s" % (cost, text))
    return True


def check_realtime(audio_path, timeout=45):
    head("3. Realtime WS · qwen-audio-3.1-realtime-flash")
    pcm = load_pcm(audio_path)
    ws = None
    asr_text, ai_text, audio_bytes = "", "", 0
    try:
        ws = websocket.create_connection(WS_URL, timeout=20, header=[
            "Origin: " + GATEWAY_ORIGIN,
            "Authorization: Bearer " + API_KEY,
        ])
        print("PASS  握手成功")

        session = {
            "type": "session.update",
            "session": {
                "modalities": ["text", "audio"],
                "voice": VOICE,
                "instructions": "你是简短口语化中文助手，回答60字以内，直接回答不要寒暄。",
                "input_audio_format": "pcm16",
                "output_audio_format": "pcm16",
                "turn_detection": {
                    "type": "server_vad",
                    "threshold": 0.5,
                    "silence_duration_ms": 800,
                },
            },
        }
        ws.send(json.dumps(session))

        # 语音分块推流（40ms/块）
        chunk = 1280
        for i in range(0, len(pcm), chunk):
            seg = pcm[i:i + chunk]
            ws.send(json.dumps({
                "type": "input_audio_buffer.append",
                "audio": base64.b64encode(seg).decode(),
            }))
            time.sleep(0.04)

        # 2 秒静音尾巴，触发 VAD 收尾
        silence = b"\x00" * chunk
        for _ in range(50):
            ws.send(json.dumps({
                "type": "input_audio_buffer.append",
                "audio": base64.b64encode(silence).decode(),
            }))
            time.sleep(0.04)

        t0 = time.time()
        ws.settimeout(8)
        while time.time() - t0 < timeout:
            try:
                raw = ws.recv()
            except Exception:
                break
            if not raw:
                continue
            try:
                ev = json.loads(raw)
            except Exception:
                continue
            ty = ev.get("type", "")
            if ty == "conversation.item.input_audio_transcription.completed":
                asr_text = ev.get("transcript", "")
            elif ty == "response.audio_transcript.delta":
                ai_text += ev.get("delta", "")
            elif ty == "response.audio_transcript.done":
                ai_text = ev.get("transcript", ai_text)
            elif ty == "response.audio.delta":
                audio_bytes += len(base64.b64decode(ev.get("delta", "") or ""))
            elif ty == "error":
                print("FAIL  服务端错误：%s" % json.dumps(ev, ensure_ascii=False)[:240])
                return False
            elif ty == "response.done":
                break
    except Exception as e:
        print("FAIL  连接/推流异常：%s" % e)
        return False
    finally:
        if ws is not None:
            try:
                ws.close()
            except Exception:
                pass

    ok = bool(asr_text or ai_text or audio_bytes)
    print(("PASS" if ok else "FAIL") + "  ASR「%s」" % asr_text)
    print("      AI「%s」" % ai_text)
    print("      下行音频 %d 字节（约 %.1fs）" % (audio_bytes, audio_bytes / 48000.0))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--audio", default=DEFAULT_AUDIO)
    ap.add_argument("--asr", action="store_true", help="只测 ASR")
    ap.add_argument("--llm", action="store_true", help="只测 LLM")
    ap.add_argument("--rt", action="store_true", help="只测实时通道")
    args = ap.parse_args()

    if not os.path.exists(args.audio):
        print("音频不存在：%s（可用 say -o a.aiff 转 16k wav）" % args.audio)
        return 2

    only = args.asr or args.llm or args.rt
    results = {}
    if not only or args.llm:
        results["LLM"] = check_llm()
    if not only or args.asr:
        results["ASR"] = check_asr(args.audio)
    if not only or args.rt:
        results["Realtime"] = check_realtime(args.audio)

    head("汇总")
    for k, v in results.items():
        print("%-9s %s" % (k, "PASS" if v else "FAIL"))
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
