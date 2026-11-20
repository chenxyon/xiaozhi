#!/usr/bin/env python3
"""星火大模型凭证连通性测试（PC 侧）

用法：python tools/test_spark_llm.py
返回：打印星火服务器响应，code=0 表示凭证有 LLM 权限，可直接接入。
"""
import asyncio, base64, hashlib, hmac, json, datetime
from urllib.parse import quote
from wsgiref.handlers import format_date_time
from time import mktime

import websockets

APPID = "5a99f947"
API_KEY = "e542c421f3bdf399222de862f95c9bba"
API_SECRET = "6002f68c3ecf4969ee2313113cad46da"

HOST = "spark-api.xf-yun.com"
PATH = "/v3.1/chat"      # generalv3 版本


def build_auth_url() -> str:
    date = format_date_time(mktime(datetime.datetime.now().timetuple()))
    origin = f"host: {HOST}\ndate: {date}\nGET {PATH} HTTP/1.1"
    sign = hmac.new(API_SECRET.encode(), origin.encode(), hashlib.sha256).digest()
    sign_b64 = base64.b64encode(sign).decode()
    auth = base64.b64encode(
        f'api_key="{API_KEY}", algorithm="hmac-sha256", '
        f'headers="host date request-line", signature="{sign_b64}"'.encode()
    ).decode()
    return (f"wss://{HOST}{PATH}?authorization={quote(auth)}"
            f"&date={quote(date)}&host={HOST}")


async def main() -> None:
    url = build_auth_url()
    print("连接:", f"wss://{HOST}{PATH}?...")
    try:
        async with websockets.connect(url, open_timeout=15) as ws:
            print("WebSocket 连接成功！")
            req = {
                "header": {"app_id": APPID, "uid": "esp32test"},
                "parameter": {"chat": {"domain": "generalv3", "max_tokens": 50}},
                "payload": {"message": {"text": [{"role": "user", "content": "你好"}]}},
            }
            await ws.send(json.dumps(req, ensure_ascii=False))
            print("已发送提问: 你好")
            while True:
                msg = json.loads(await ws.recv())
                code = msg.get("header", {}).get("code", -1)
                status = msg.get("header", {}).get("status", -1)
                text = (msg.get("payload", {}).get("choices", {})
                          .get("text", [{}])[0].get("content", ""))
                if code != 0:
                    print(f"★ 服务器返回错误 code={code} message={msg['header'].get('message')}")
                    return
                if text:
                    print("星火回复片段:", text)
                if status == 2:
                    print("★ 完成 —— 凭证有效，星火 LLM 可直接接入！")
                    return
    except Exception as e:
        print(f"★ 连接失败: {type(e).__name__}: {e}")


if __name__ == "__main__":
    asyncio.run(main())
