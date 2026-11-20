# PC 端对照实验：用与设备完全相同的参数直连讯飞 RTASR
# 目的：区分「账号/授权问题」vs「设备端 WS 库把 URL 改坏」
import hashlib, hmac, base64, time, ssl, sys
import urllib.parse
import websocket

APPID   = '5a99f947'
API_KEY = '68223aa9861d99fa37726c541f649'

ts = str(int(time.time()))
base = APPID + ts
md5hex = hashlib.md5(base.encode()).hexdigest()
signa = base64.b64encode(
    hmac.new(API_KEY.encode(), md5hex.encode(), hashlib.sha1).digest()
).decode()
signa_enc = urllib.parse.quote(signa, safe='')

url = f"wss://rtasr.xfyun.cn/v1/ws?appid={APPID}&ts={ts}&signa={signa_enc}"
print(f"ts    = {ts}")
print(f"signa = {signa}")
print(f"url   = {url}")

websocket.enableTrace(True)   # 打印真实发出的握手请求行
ws = websocket.create_connection(url, timeout=15, sslopt={"cert_reqs": ssl.CERT_NONE})
msg = ws.recv()
print("\n==== 服务器首条消息 ====")
print(msg)
ws.close()
