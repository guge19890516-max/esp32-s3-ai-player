# 生成 Unicode<->GBK 转换表（esp32_ai_chat/gbk_table.cpp）
# 用法: python gen_gbk_table.py
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "esp32", "esp32_ai_chat", "gbk_table.cpp")

# GBK 2 字节区：首字节 0x81-0xFE，次字节 0x40-0x7E、0x80-0xFE（不含 0x7F）
# 表按 (lead-0x81)*190 + (trail 压缩索引) 线性排布
def trail_list():
    return list(range(0x40, 0x7F)) + list(range(0x80, 0xFF))

def trail_idx(t):
    return t - 0x40 if t < 0x7F else t - 0x41

def gbk_idx(lead, trail):
    return (lead - 0x81) * 190 + trail_idx(trail)

GBK_N = 126 * 190
g2u = [0xFFFD] * GBK_N
u2g = [0] * 65536   # 0 = 不可映射（ASCII 区在代码里单独处理）

for lead in range(0x81, 0xFF):
    for trail in trail_list():
        b = bytes([lead, trail])
        try:
            ch = b.decode("gbk")
        except UnicodeDecodeError:
            continue
        cp = ord(ch)
        g2u[gbk_idx(lead, trail)] = cp
        if cp < 0x10000 and u2g[cp] == 0:
            u2g[cp] = (lead << 8) | trail

mapped = sum(1 for v in u2g if v)
print("GBK mapped 2-byte codes:", sum(1 for v in g2u if v != 0xFFFD))
print("Unicode chars mappable:", mapped)

def emit_arr(name, arr, per_line=12):
    lines = []
    lines.append("static const uint16_t %s[%d] = {" % (name, len(arr)))
    for i in range(0, len(arr), per_line):
        row = arr[i:i + per_line]
        lines.append("    " + ",".join("0x%04X" % v for v in row) + ",")
    lines.append("};")
    return "\n".join(lines)

with open(OUT, "w", encoding="utf-8") as f:
    f.write("// 自动生成：tools/gen_gbk_table.py（勿手改）\n")
    f.write("// GBK/GB2312 与 Unicode 双向转换表，ESP32 上 const 数组放 flash（约 180KB）\n")
    f.write("#include <Arduino.h>\n\n")
    f.write("#define GBK_N %d\n" % GBK_N)
    f.write("#define GBK_LEAD0 0x81\n#define GBK_LEAD1 0xFE\n\n")
    f.write("// GBK 双字节码点 -> Unicode（0xFFFD=不可映射）\n")
    f.write(emit_arr("g_gbk2uni", g2u))
    f.write("\n\n// Unicode -> GBK 双字节码点（0=不可映射；ASCII<0x80 在代码里直通）\n")
    f.write(emit_arr("g_uni2gbk", u2g))
    f.write("\n\nuint16_t gbkTableN() { return GBK_N; }\n")
    f.write("uint16_t gbkToUni(uint16_t g) { return g_gbk2uni[g]; }\n")
    f.write("uint16_t uniToGbk(uint16_t u) { return g_uni2gbk[u]; }\n")
print("written:", OUT)
