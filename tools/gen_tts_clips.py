#!/usr/bin/env python3
"""
离线 TTS 话术生成器

单一数据源：tools/phrases.csv（CSV，3 列：枚举名 / 话术内容 / 备注）

用法：
    python tools/gen_tts_clips.py            # 生成全部（复用 mp3 缓存）
    python tools/gen_tts_clips.py --force    # 强制重新合成（不缓存）
    python tools/gen_tts_clips.py --voice zh-CN-YunxiNeural    # 换男声

依赖（纯 Python，零外部工具）：
    pip install edge-tts miniaudio

输出：
    src/tts/tts_phrases.h    ← 枚举定义（自动生成）
    src/tts/tts_phrases.c    ← PCM 数据 + clips 初始化 + tts_phrase_text()
    cache/*.mp3             ← edge-tts 合成的 mp3 缓存（项目根下，下次运行跳过）
    cache/*.raw             ← 解码后的 raw PCM（备用）

修改：
    2026-11-16 新建，edge-tts + miniaudio 纯 Python 方案
    2026-11-17 CSV → xlsx，openpyxl 加载
    2026-11-17 xlsx → CSV 改回 csv 模块加载（utf-8-sig，Excel 可直接编辑）
"""
import asyncio, os, sys, argparse, re, csv
from pathlib import Path

import edge_tts
import miniaudio

# ====== 路径 ======
ROOT = Path(__file__).resolve().parent.parent
CSV_PATH = Path(__file__).resolve().parent / "phrases.csv"
_OUT_ROOT = ROOT / "lib" / "tts_phrases"          # 依赖库根（手写 library.json 在这）
OUT_DIR  = _OUT_ROOT / "cache"                    # 所有生成产物（h/c/mp3/raw）全在这
OUT_H    = OUT_DIR / "tts_phrases.h"
OUT_C    = OUT_DIR / "tts_phrases.c"
CACHE    = OUT_DIR                                 # CACHE 就是 OUT_DIR 本身

# ====== 合成参数 ======
VOICE = "zh-CN-XiaoxiaoNeural"     # 女声晓晓，和讯飞 xiaoyan 风格接近
SAMPLE_RATE = 16000                # ESP32 功放采样率

# ====== 读 csv ======
# CSV 列：前缀(可选), 枚举名, 话术内容, 备注
# 前缀留空默认 TTS_PHRASE_，用于跨项目切换（例如 WX_PHRASE_ / KWS_PHRASE_）
DEFAULT_PREFIX = "TTS_PHRASE_"

_VALID_NAME_RE = re.compile(r'^[A-Za-z0-9_]+$')   # C 宏命名允许 ASCII 字母数字下划线

def load_phrases(csv_path: Path) -> list:
    """返回 [(enum_name_full, text), ...]，跳过空行和表头。
    前缀和枚举名自动 uppercase；含中文/非 ASCII 字符报错退出。"""
    if not csv_path.exists():
        raise FileNotFoundError(f"找不到 {csv_path}，请创建 phrases.csv（UTF-8，前缀/枚举名/话术内容/备注）")
    phrases = []
    header_skipped = False
    errors = []
    lineno = 0
    with open(csv_path, newline="", encoding="utf-8-sig") as f:
        for row in csv.reader(f):
            lineno += 1
            if not row or not (len(row) > 1 and row[1].strip()):
                continue
            if not header_skipped:
                header_skipped = True
                continue
            prefix    = row[0].strip() if row[0].strip() else DEFAULT_PREFIX
            base_name = row[1].strip()
            text      = row[2].strip() if len(row) > 2 else ""
            if not base_name or not text:
                continue

            # 大写化
            prefix    = prefix.upper()
            base_name = base_name.upper()

            # 只允许 ASCII [A-Z0-9_]
            problems = []
            if not _VALID_NAME_RE.match(prefix):
                problems.append(f"前缀 \"{row[0].strip()}\" 含非法字符（只允许字母/数字/下划线）")
            if not _VALID_NAME_RE.match(base_name):
                problems.append(f"枚举名 \"{row[1].strip()}\" 含非法字符（只允许字母/数字/下划线）")
            if problems:
                errors.append(f"  第 {lineno} 行: {'; '.join(problems)}")
                continue

            enum_name = f"{prefix}{base_name}"
            phrases.append((enum_name, text))

    if errors:
        print("\n❌ CSV 校验失败！请修正后再跑：")
        for e in errors:
            print(e)
        print()
        raise SystemExit(1)

    if not phrases:
        raise RuntimeError(f"csv 没有有效话术行: {csv_path}")

    # 检查重复枚举名
    seen = {}
    for enum_name, text in phrases:
        if enum_name in seen:
            print(f"\n❌ 枚举名重复: {enum_name} 出现两次（分别对应 \"{seen[enum_name]}\" 和 \"{text}\"）")
            raise SystemExit(1)
        seen[enum_name] = text

    return phrases

# ====== C 变量名 ======
def enum_to_var(enum_name: str) -> str:
    return "s_" + enum_name.lower()

# ====== 反解析已有的 tts_phrases.h → [(enum_name, text), ...] ======
_H_ENUM_RE = re.compile(r'^\s+(TTS_PHRASE_\w+)\s*(?:=\s*\d+)?\s*,?\s*/\*\s*(.*?)\s*\*/')
def parse_existing_phrases_h(h_path: Path) -> list:
    """从旧 tts_phrases.h 提取枚举名和文案（跳过 TTS_PHRASE_MAX 哨兵）"""
    if not h_path.exists():
        return []
    out = []
    for line in h_path.read_text(encoding='utf-8').splitlines():
        m = _H_ENUM_RE.match(line)
        if m and m.group(1) != "TTS_PHRASE_MAX":
            out.append((m.group(1), m.group(2).strip()))
    return out

# ====== 反解析已有的 tts_phrases.c → {enum_name: bytes PCM} ======
_C_ARRAY_RE = re.compile(r'static const uint8_t\s+(s_tts_phrase_\w+)\s*\[(\d+)\]\s*=\s*\{(.*?)\}', re.DOTALL)
def parse_existing_pcm_c(c_path: Path) -> dict:
    """从旧 tts_phrases.c 提取每个 s_tts_phrase_xxx 数组的原始 bytes"""
    if not c_path.exists():
        return {}
    out = {}
    for m in _C_ARRAY_RE.finditer(c_path.read_text(encoding='utf-8')):
        var_name = m.group(1)
        enum_name = var_name[2:].upper()   # s_tts_phrase_portal_start → TTS_PHRASE_PORTAL_START
        hex_text = m.group(3)
        bytes_list = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{2}', hex_text)]
        out[enum_name] = bytes(bytes_list)
    return out

# ====== 合并：旧全集 + CSV 新（CSV 同名覆盖） ======
def merge_phrases(old_phrases: list, new_phrases: list) -> list:
    """old_phrases / new_phrases: [(enum_name, text), ...]
    返回并集，CSV 里的同名枚举覆盖旧的文案；旧枚举不在 CSV 里 → 保留。"""
    merged = {}
    for name, text in old_phrases:
        merged[name] = text            # 先放旧的
    for name, text in new_phrases:
        merged[name] = text            # 新的覆盖同名
    return list(merged.items())

# ====== raw bytes → C const 数组文本 ======
def bytes_to_c_array(data: bytes, var_name: str, per_line: int = 16) -> str:
    lines = []
    lines.append(f"/* {var_name}: {len(data)} bytes ({len(data)//2} samples @ 16kHz mono) */")
    lines.append(f"static const uint8_t {var_name}[{len(data)}] = {{")
    for i in range(0, len(data), per_line):
        chunk = data[i:i+per_line]
        hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append(f"    {hex_str},")
    lines.append("};")
    return "\n".join(lines)

# ====== edge-tts 合成 mp3（异步，支持缓存） ======
async def synthesize_one(text: str, mp3_path: Path) -> bytes:
    if mp3_path.exists():
        print(f"    [cache hit] {mp3_path.name}")
        return mp3_path.read_bytes()
    communicate = edge_tts.Communicate(text=text, voice=VOICE)
    await communicate.save(str(mp3_path))
    print(f"    [edge-tts]  → {mp3_path.name} ({mp3_path.stat().st_size} bytes)")
    return mp3_path.read_bytes()

# ====== miniaudio 解码 mp3 → 16kHz / 16bit / 单声道 raw PCM ======
def mp3_to_pcm(mp3: bytes) -> bytes:
    decoded = miniaudio.decode(
        mp3,
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=1,
        sample_rate=SAMPLE_RATE,
    )
    return decoded.samples.tobytes()   # numpy int16 → little-endian raw bytes

# ====== 生成 .h（枚举定义） ======
def gen_header(phrases: list) -> str:
    enum_lines = []
    enum_lines.append("    TTS_PHRASE_PORTAL_START = 0,   /* 配网中，请连接设备热点 */")
    # 注意：xlsx 第一行（跳过表头后）TTS_PHRASE_PORTAL_START 已经是 =0，后续不加显式值
    for enum_name, text in phrases[1:]:
        enum_lines.append(f"    {enum_name:<32}, /* {text} */")
    enum_lines.append("    TTS_PHRASE_MAX")

    return f"""\
#ifndef TTS_PHRASES_H
#define TTS_PHRASES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {{
#endif

/* 固定话术枚举 —— 由 tools/gen_tts_clips.py 从 tools/phrases.csv 自动生成，请勿手改 */
typedef enum {{
{chr(10).join(enum_lines)}
}} tts_phrase_t;

/* 一条离线语音片段：原始 s16le 单声道 PCM */
typedef struct {{
    const uint8_t *data;        /* PCM 数据，NULL 表示未提供 */
    uint32_t       len;         /* 字节数 */
    uint32_t       sample_rate; /* 采样率 Hz */
}} tts_clip_t;

/* 由 tools/gen_tts_clips.py 生成 */
extern const tts_clip_t tts_clips[TTS_PHRASE_MAX];

/* 该话术对应的中文文案（供云 TTS 兜底 / 日志） */
const char *tts_phrase_text(tts_phrase_t ph);

#ifdef __cplusplus
}}
#endif

#endif /* TTS_PHRASES_H */
"""

# ====== 生成 .c（PCM 数据 + clips 初始化 + tts_phrase_text） ======
def gen_source(phrases: list, pcm_map: dict) -> str:
    array_blocks = []
    for enum_name, _ in phrases:
        var = enum_to_var(enum_name)
        data = pcm_map[enum_name]
        array_blocks.append(bytes_to_c_array(data, var))

    phrases_init_lines = []
    for enum_name, _ in phrases:
        var = enum_to_var(enum_name)
        data = pcm_map[enum_name]
        phrases_init_lines.append(
            f"    [{enum_name}] = {{ (const uint8_t *){var}, {len(data)}, {SAMPLE_RATE} }},"
        )

    switch_lines = []
    for enum_name, text in phrases:
        # C 字符串里的双引号转义
        safe_text = text.replace('\\', '\\\\').replace('"', '\\"')
        switch_lines.append(f"    case {enum_name:<32}: return \"{safe_text}\";")

    return f"""\
/* 固定话术数据表
 *
 * 本文件由 tools/gen_tts_clips.py 自动生成，请勿手改。
 * 单一数据源：tools/phrases.csv
 * 语音: {VOICE}  采样: {SAMPLE_RATE} Hz / 16bit / 单声道
 *
 * 重新生成：
 *   pip install edge-tts miniaudio
 *   python tools/gen_tts_clips.py
 *   python tools/gen_tts_clips.py --force        # 忽略缓存全量重合成
 *   python tools/gen_tts_clips.py --voice zh-CN-YunxiNeural  # 换男声
 */
#include <stddef.h>
#include <stdint.h>
#include "tts_phrases.h"

/* ============ PCM 数据数组 ============ */

{chr(10).join(array_blocks)}

/* ============ clips 初始化表 ============ */

const tts_clip_t tts_clips[TTS_PHRASE_MAX] = {{
{chr(10).join(phrases_init_lines)}
}};

/* ============ 话术文本（云 TTS 兜底 / 日志） ============ */

const char *tts_phrase_text(tts_phrase_t ph)
{{
    switch (ph) {{
{chr(10).join(switch_lines)}
    default:                          return "";
    }}
}}
"""

async def main():
    global VOICE
    ap = argparse.ArgumentParser(
        prog="gen_tts_clips.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=(
            "离线 TTS 话术生成器\n"
            "\n"
            "功能：读取 tools/phrases.csv（枚举名/话术内容/备注 三列），用微软\n"
            "Edge TTS 在线合成每句话术，解码为 16kHz/16bit/单声道 PCM，生成：\n"
            "  - src/tts/tts_phrases.h   枚举 tts_phrase_t 定义（勿手改）\n"
            "  - src/tts/tts_phrases.c   PCM 数据 + clips 表 + tts_phrase_text()\n"
            "  - cache/*.mp3/.raw       合成缓存（项目根下，下次运行直接复用，不联网）\n"
            "\n"
            "依赖：pip install edge-tts miniaudio\n"
            "流程：CSV 读取 → edge-tts 合成 mp3 → miniaudio 解码 PCM → 生成 .h/.c"
        ),
        epilog=(
            "常用示例：\n"
            "  python tools/gen_tts_clips.py                          # 增量生成（复用缓存）\n"
            "  python tools/gen_tts_clips.py --force                  # 忽略缓存全部重合成\n"
            "  python tools/gen_tts_clips.py --voice zh-CN-YunxiNeural # 换男声云希\n"
            "\n"
            "全部中文音色（zh-CN 大陆普通话 8 个 + zh-HK 粤语 3 个 + zh-TW 台湾普通话 3 个）：\n"
            "  zh-CN-XiaoxiaoNeural              女声晓晓（默认，接近讯飞 xiaoyan）\n"
            "  zh-CN-XiaoyiNeural                女声晓伊（甜美，卡通）\n"
            "  zh-CN-YunjianNeural               男声云健（沉稳，运动）\n"
            "  zh-CN-YunxiNeural                 男声云希（阳光，活泼）\n"
            "  zh-CN-YunxiaNeural               男声云夏（少年，卡通）\n"
            "  zh-CN-YunyangNeural               男声云扬（播音腔，新闻）\n"
            "  zh-CN-liaoning-XiaobeiNeural      女声小北（东北方言，幽默）\n"
            "  zh-CN-shaanxi-XiaoniNeural        女声小妮（陕西方言，明亮）\n"
            "  zh-HK-HiuGaaiNeural              粤语女声曉佳（友善）\n"
            "  zh-HK-HiuMaanNeural              粤语女声曉曼（友善）\n"
            "  zh-HK-WanLungNeural              粤语男声雲龍（友善）\n"
            "  zh-TW-HsiaoChenNeural            台湾女声曉臻（友善）\n"
            "  zh-TW-HsiaoYuNeural              台湾女声曉雨（友善）\n"
            "  zh-TW-YunJheNeural               台湾男聲雲哲（友善）\n"
            "  注：Edge TTS 无真正中文童声，云夏（少年音）最接近，或用晓晓变调近似（本脚本暂不支持 pitch 参数）\n"
            "\n"
            "注意事项：\n"
            "  1. 换 --voice 后必须加 --force，否则命中旧音色缓存（缓存名不含音色）\n"
            "  2. csv 用 UTF-8 编码（Excel 另存时选 \"CSV UTF-8\"），表头行任意，枚举名必须 TTS_PHRASE_ 开头\n"
            "  3. 新增话术只需在 csv 加行再运行本脚本，旧句子自动走缓存秒级完成"
        ),
    )
    ap.add_argument(
        "--force", action="store_true",
        help="强制重新合成：删除全部 mp3 缓存后逐句重新合成（换音色后必须加此参数）",
    )
    ap.add_argument(
        "--purge-cache", action="store_true",
        help="清理 cache/ 里不在 CSV 中的残留 mp3/raw（例如删过的旧话术缓存）",
    )
    ap.add_argument(
        "--voice", default=VOICE, metavar="VOICE_NAME",
        help=(
            f"edge-tts 音色名，默认 {VOICE}（女声晓晓）。"
            "常用：zh-CN-YunxiNeural(男) / zh-CN-YunjianNeural(男) / "
            "zh-CN-XiaoyiNeural(女)，完整音色表见帮助底部"
        ),
    )
    ap.add_argument(
        "--export-dir", default=None, metavar="PATH",
        help=(
            f"导出目录（默认与生成目录相同：{OUT_DIR}）。"
            "指定后生成完成会把 .h/.c 复制过去，适合把产物推到 lib/tts_phrases/ 等依赖库位置。"
        ),
    )
    args = ap.parse_args()

    VOICE = args.voice

    # ====== 读 csv ======
    phrases = load_phrases(CSV_PATH)

    CACHE.mkdir(parents=True, exist_ok=True)
    if args.force:
        for p in CACHE.glob("*.mp3"):
            p.unlink()
            print(f"[force] removed {p.name}")
    if args.purge_cache:
        valid = {name.lower() for name, _ in phrases}
        for p in list(CACHE.glob("*")):
            if not p.is_file() or p.suffix not in (".mp3", ".raw"):
                continue
            ok = any(p.name == f"{v}{p.suffix}" or p.name.startswith(f"{v}_")
                     for v in valid)
            if not ok:
                p.unlink()
                print(f"[purge] removed {p.name}")
        kept = sum(1 for p in CACHE.glob("*.mp3"))
        print(f"\n[purge] 完成！剩余有效缓存: {kept} 个 mp3")
        return   # purge 独立模式，清完就退出

    print(f"=== 离线 TTS 话术生成器 ===")
    print(f"  CSV:     {CSV_PATH}")
    print(f"  语音:    {VOICE}")
    print(f"  采样:    {SAMPLE_RATE} Hz / 16bit / 单声道")
    print(f"  缓存:    {CACHE}")

    # ====== 决定输出模式 ======
    export_dir = Path(args.export_dir).resolve() if args.export_dir else None
    if export_dir:
        # --- 导出模式：反解析旧 export-dir/cache → 合并 CSV 新 → 全量写回 export-dir/cache ---
        OLD_H = export_dir / "cache" / "tts_phrases.h"
        OLD_C = export_dir / "cache" / "tts_phrases.c"
        old_phrases = parse_existing_phrases_h(OLD_H)
        old_pcm_map = parse_existing_pcm_c(OLD_C)
        phrases_all = merge_phrases(old_phrases, phrases)

        # 打印合并统计
        old_dict = dict(old_phrases)
        old_names = {n for n, _ in old_phrases}
        csv_names = {n for n, _ in phrases}
        only_old     = old_names - csv_names
        only_csv     = csv_names - old_names
        both_changed = {n for n in csv_names & old_names
                        if dict(phrases).get(n) != old_dict.get(n)}

        print(f"\n  📦 导出模式 (合并)")
        print(f"     CSV 新:      {len(phrases)} 句")
        print(f"     export-dir 旧: {len(old_phrases)} 句")
        print(f"     合并后全集:   {len(phrases_all)} 句")
        if only_csv:     print(f"     ➕ CSV 新增:   {only_csv}")
        if only_old:     print(f"     🔒 旧有保留:   {only_old}")
        if both_changed: print(f"     ✏️  文案更新:   {both_changed}")

        # Step 1: 合成 —— 旧枚举文案没变 → 直接复用 old_pcm_map，新枚举才合成
        print(f"\n[1/4] edge-tts 合成/复用 {len(phrases_all)} 句...")
        pcm_map = {}
        for enum_name, text in phrases_all:
            if enum_name in old_pcm_map and old_dict.get(enum_name) == text:
                # 文案没变 → 复用旧 PCM（跳过 edge-tts）
                print(f"  • {enum_name}: \"{text}\"  [复用旧 PCM {len(old_pcm_map[enum_name]):,} bytes]")
                pcm_map[enum_name] = old_pcm_map[enum_name]
            else:
                # 新增或文案变了 → edge-tts 合成
                safe = re.sub(r'[^\w]', '_', text)
                mp3_path = CACHE / f"{enum_name.lower()}_{safe[:12]}.mp3"
                print(f"  • {enum_name}: \"{text}\"")
                mp3 = await synthesize_one(text, mp3_path)

                raw_path = CACHE / f"{enum_name.lower()}.raw"
                if raw_path.exists() and mp3_path.stat().st_mtime <= raw_path.stat().st_mtime:
                    print(f"    [raw cache hit] {raw_path.name}")
                    pcm_map[enum_name] = raw_path.read_bytes()
                else:
                    pcm = mp3_to_pcm(mp3)
                    raw_path.write_bytes(pcm)
                    print(f"    [decode] → {raw_path.name} ({len(pcm)} bytes, {len(pcm)//SAMPLE_RATE}s)")
                    pcm_map[enum_name] = pcm

        # Step 2/3: 生成 → 写 export-dir/cache/（跟依赖库结构一致）
        export_cache = export_dir / "cache"
        export_cache.mkdir(parents=True, exist_ok=True)
        print(f"\n[2/4] 生成 {export_dir.name}/cache/tts_phrases.h ...")
        (export_cache / "tts_phrases.h").write_text(gen_header(phrases_all), encoding="utf-8")
        print(f"[3/4] 生成 {export_dir.name}/cache/tts_phrases.c ...")
        (export_cache / "tts_phrases.c").write_text(gen_source(phrases_all, pcm_map), encoding="utf-8")

        # Step 4: 汇总
        total_bytes = sum(len(v) for v in pcm_map.values())
        total_sec = total_bytes / 2 / SAMPLE_RATE
        print(f"\n✅ 导出完成（合并模式）！")
        print(f"   CSV:            {CSV_PATH}")
        print(f"   旧来源:         {OLD_H}")
        print(f"   导出目录:       {export_dir}")
        print(f"   {len(phrases_all)} 句合计: {total_bytes:,} bytes ({total_sec:.1f} 秒音频)")
        print(f"   📈 变化: +{len(only_csv)} 新增 / {len(only_old)} 旧有保留")
    else:
        # --- 普通模式：CSV 全量 → OUT_DIR ---
        print(f"  话术数:  {len(phrases)} 句")
        print(f"  输出:    {OUT_H} + {OUT_C}")
        print()

        # Step 1: 全量合成
        print(f"[1/4] edge-tts 合成 {len(phrases)} 句...")
        pcm_map = {}
        for enum_name, text in phrases:
            safe = re.sub(r'[^\w]', '_', text)
            mp3_path = CACHE / f"{enum_name.lower()}_{safe[:12]}.mp3"
            print(f"  • {enum_name}: \"{text}\"")
            mp3 = await synthesize_one(text, mp3_path)

            raw_path = CACHE / f"{enum_name.lower()}.raw"
            if raw_path.exists() and mp3_path.stat().st_mtime <= raw_path.stat().st_mtime:
                print(f"    [raw cache hit] {raw_path.name}")
                pcm_map[enum_name] = raw_path.read_bytes()
            else:
                pcm = mp3_to_pcm(mp3)
                raw_path.write_bytes(pcm)
                print(f"    [decode] → {raw_path.name} ({len(pcm)} bytes, {len(pcm)//SAMPLE_RATE}s)")
                pcm_map[enum_name] = pcm

        # Step 2/3: 生成
        OUT_DIR.mkdir(parents=True, exist_ok=True)
        print(f"\n[2/4] 生成 {OUT_H.name} ...")
        OUT_H.write_text(gen_header(phrases), encoding="utf-8")
        print(f"[3/4] 生成 {OUT_C.name} ...")
        OUT_C.write_text(gen_source(phrases, pcm_map), encoding="utf-8")

        # Step 4: 汇总
        total_bytes = sum(len(v) for v in pcm_map.values())
        total_sec = total_bytes / 2 / SAMPLE_RATE
        print(f"\n✅ 生成完成！")
        print(f"   CSV 源文件:       {CSV_PATH}")
        print(f"   生成目录:         {OUT_DIR}")
        print(f"   缓存目录:         {CACHE}")
        print(f"   {len(phrases)} 句合计: {total_bytes:,} bytes ({total_sec:.1f} 秒音频)")
        print(f"\n💡 提示: 如需导出合并到依赖库目录，加 --export-dir lib/tts_phrases")

    print(f"\n下一步: 编译烧录后，唤醒喊 \"Hi ESP\" → 应瞬间听到 \"我在，请说\"")

if __name__ == "__main__":
    asyncio.run(main())
