import hashlib, hmac, base64, urllib.parse

# ====== 用户实际参数（来自终端日志和代码）======
appid   = '5a99f947'
ts      = '1795090775'
api_key = '68223aa9861d99fa37726c541f649'  # kws_hub.c 里的 RTASR 专用 key

# 终端 URL 里的 signa（%3D 是 =）
url_signa = '7dQdLxG821UNWjTGtgHMiaeCR08%3D'
expected_signa = urllib.parse.unquote(url_signa)

print(f'appid    = {appid}')
print(f'ts       = {ts}')
print(f'api_key  = {api_key}')
print(f'URL signa= {url_signa}')
print(f'URL signa(解码) = {expected_signa}')

# ====== 计算 ======
baseString = appid + ts
md5hex = hashlib.md5(baseString.encode()).hexdigest()
sha1out = hmac.new(api_key.encode(), md5hex.encode(), hashlib.sha1).digest()
our_signa = base64.b64encode(sha1out).decode()

print()
print(f'baseString = {baseString}')
print(f'MD5        = {md5hex}')
print(f'正确 signa = {our_signa}')
print(f'终端 signa = {expected_signa}')
print(f'匹配       = {our_signa == expected_signa}')

# ====== 反过来：如果用旧 key 呢？======
old_key = 'e542c421f3bdf399222de862f95c9bba'
sha1_old = hmac.new(old_key.encode(), md5hex.encode(), hashlib.sha1).digest()
old_signa = base64.b64encode(sha1_old).decode()
print(f'\n用旧 key (e542...) 的 signa = {old_signa}')
print(f'旧 key 匹配 = {old_signa == expected_signa}')
