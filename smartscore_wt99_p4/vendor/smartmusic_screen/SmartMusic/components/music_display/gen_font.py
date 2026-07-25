#!/usr/bin/env python3
"""Generate the LVGL Leland subset used by the MusicXML renderer.

All code points are canonical SMuFL mappings. Geometry such as staff lines,
stems, beams, ties, and slurs is produced by the engraving backend; every
musical symbol is supplied by Leland.
"""
from PIL import Image, ImageDraw, ImageFont
import os, sys

FONT_SIZE = 72  # 90pt → 72pt (-20%)
BPP = 8  # 改为8bpp避开LVGL v9 4bpp渲染问题
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
FONT_PATH = os.path.join(BASE_DIR, "Leland.otf")
OUT_PATH = os.path.join(BASE_DIR, "lv_font_LelandSMuFL_72.c")
FONT_SIZE = int(sys.argv[1]) if len(sys.argv) > 1 else FONT_SIZE
BRACE_ONLY = len(sys.argv) > 2 and sys.argv[2] == "brace"
if BRACE_ONLY:
    OUT_PATH = os.path.join(BASE_DIR, f"lv_font_LelandBrace_{FONT_SIZE}.c")
    FONT_SYMBOL = f"lv_font_LelandBrace_{FONT_SIZE}"
    FONT_GUARD = f"LV_FONT_LELANDBRACE_{FONT_SIZE}"
else:
    OUT_PATH = os.path.join(BASE_DIR, f"lv_font_LelandSMuFL_{FONT_SIZE}.c")
    FONT_SYMBOL = f"lv_font_LelandSMuFL_{FONT_SIZE}"
    FONT_GUARD = f"LV_FONT_LELANDSMUFL_{FONT_SIZE}"

# ── SMuFL glyph 列表 ──
# (codepoint, name)
GLYPHS = [
    (0x0020, "space"),
    (0x002D, "hyphen"),
    (0xE044, "repeatDots"),
    (0xE050, "treble_clef"),
    (0xE05C, "alto_clef"),
    (0xE062, "bass_clef"),
    *[(0xE080 + digit, f"timeSig{digit}") for digit in range(10)],
    (0xE08A, "timeSigCommon"),
    (0xE08B, "timeSigCutCommon"),
    (0xE0A2, "whole_note"),
    (0xE0A3, "half_note"),
    (0xE0A4, "quarter_note"),
    (0xE1E7, "augmentationDot"),
    (0xE240, "flag8thUp"),
    (0xE241, "flag8thDown"),
    (0xE242, "flag16thUp"),
    (0xE243, "flag16thDown"),
    (0xE244, "flag32ndUp"),
    (0xE245, "flag32ndDown"),
    (0xE260, "flat"),
    (0xE261, "natural"),
    (0xE262, "sharp"),
    (0xE263, "doubleSharp"),
    (0xE264, "doubleFlat"),
    (0xE4A0, "accentAbove"),
    (0xE4A1, "accentBelow"),
    (0xE4A2, "staccatoAbove"),
    (0xE4A3, "staccatoBelow"),
    (0xE4A4, "tenutoAbove"),
    (0xE4A5, "tenutoBelow"),
    (0xE4C0, "fermataAbove"),
    (0xE4C1, "fermataBelow"),
    (0xE4E3, "wholeRest"),
    (0xE4E4, "halfRest"),
    (0xE4E5, "quarterRest"),
    (0xE4E6, "eighthRest"),
    (0xE4E7, "sixteenthRest"),
    (0xE4E8, "thirtySecondRest"),
]
if BRACE_ONLY:
    # The canonical SMuFL brace is one em high. Render it at 252 px so its
    # 1000-unit outline spans a 14sp piano grand staff at staffSpace=18px.
    GLYPHS = [(0xE000, "brace")]

# ── 已知正确的 metrics ──
# Leland 字体全部走自动计算，这里只放空占位的
ORIG_METRICS = {
    0x0020: (0, 0, 144, 0, 0),
    0x002D: (0, 0, 288, 0, 0),
    # Leland brace advance: 62/1000 em. At 252px this is 15.624px,
    # represented by LVGL's 8.4 fixed-point advance as 250.
    0xE000: (0, 0, 250, 17, 251),
}

# ── 从 Bravura.otf CFF 表自动提取 glyph metrics ──
def compute_cff_metrics(font_path, font_size, cp_list):
    """使用 fontTools 从 CFF 表获取 glyph 边界框, 计算 LVGL METRICS."""
    try:
        from fontTools.ttLib import TTFont
        font_file = TTFont(font_path)
        units_per_em = 1000
        scale = font_size / units_per_em
        
        cff = font_file['CFF ']
        top_dict = cff.cff.topDictIndex[0]
        char_strings = top_dict.CharStrings
        private = top_dict.Private
        nominal_width = private.nominalWidthX if hasattr(private, 'nominalWidthX') else 0
        hmtx = font_file['hmtx']
        cmap = font_file.getBestCmap()
        
        result = {}
        for cp in cp_list:
            glyph_name = cmap[cp]
            aw = hmtx[glyph_name][0]  # advance width in font units
            cs = char_strings[glyph_name]
            try:
                bounds = cs.calcBounds(nominal_width)
                if bounds:
                    xmin, ymin, xmax, ymax = bounds
                else:
                    xmin = ymin = xmax = ymax = 0
            except:
                xmin = ymin = xmax = ymax = 0
            
            ofs_x = int(round(xmin * scale))
            ofs_y = int(round(-ymax * scale))   # LVGL Y-down vs FreeType Y-up
            box_w = max(0, int(round((xmax - xmin) * scale)))
            box_h = max(0, int(round((ymax - ymin) * scale)))
            adv_w = int(round(aw * scale * 16))  # LVGL 8.4 fixed-point
            result[cp] = (ofs_x, ofs_y, adv_w, box_w, box_h)
        font_file.close()
        return result
    except ImportError:
        print("WARNING: fontTools not available, will use fallback metrics")
        return {}
    except Exception as e:
        print(f"WARNING: fontTools error: {e}")
        return {}

# ── 为没有原始 METRICS 的 glyph 自动计算 ──
NEW_CP = [cp for cp, _ in GLYPHS if cp not in ORIG_METRICS]
cff_metrics = compute_cff_metrics(FONT_PATH, FONT_SIZE, NEW_CP)

# 合并: 原始 + 自动计算
METRICS = dict(ORIG_METRICS)
for cp, vals in cff_metrics.items():
    METRICS[cp] = vals
    print(f"  [auto] 0x{cp:04X}: {vals}")

# ── 渲染参数 ──
BASE_LINE = round(60 * FONT_SIZE / 72)
LINE_HEIGHT = round(159 * FONT_SIZE / 72)

font = ImageFont.truetype(FONT_PATH, FONT_SIZE)
results = []

for cp, name in GLYPHS:
    # Draw from an explicit baseline in a generous canvas. At 72 px the old
    # canvas ended at y=288 while the G-clef tail reached y=293.
    canvas_size = FONT_SIZE * 6
    origin = FONT_SIZE * 3
    img = Image.new("RGBA", (canvas_size, canvas_size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    draw.text((origin, origin), chr(cp), font=font,
              fill=(255, 255, 255, 255), anchor="ls")
    bbox = img.getbbox()
    if bbox and bbox[2] > bbox[0] and bbox[3] > bbox[1]:
        cropped = img.crop(bbox)
        gw, gh = cropped.size
        alpha = [p[3] for p in list(cropped.getdata())]
        render_ox = bbox[0] - origin
        render_oy = bbox[1] - origin
        if 0xE240 <= cp <= 0xE245:
            # LVGL expects the bottom bearing, not Pillow's top offset.
            # Otherwise flags are mirrored away from the stem endpoint.
            render_oy = -(render_oy + gh)
        if BRACE_ONLY:
            # lv_font_fmt_txt_glyph_dsc_t stores ofs_y in int8_t. Anchor the
            # 251px brace at its visual centre so the signed offset remains
            # representable; the layout emits the grand-staff centre as Y.
            render_oy = -((gh + 1) // 2)
    else:
        gw, gh = 0, 0
        alpha = []
        render_ox = render_oy = 0
    results.append((cp, name, gw, gh, render_ox, render_oy, alpha))
    print(f"  {name}: {gw}x{gh}")

# Build LVGL font data
all_bmp = bytearray()
entries = []
unicode_list = []

for cp, name, gw, gh, render_ox, render_oy, alpha in results:
    if gw > 0 and gh > 0:
        if BPP == 4:
            if gw % 2 == 1:
                orig_gw = gw
                new_alpha = []
                for row in range(gh):
                    start = row * orig_gw
                    new_alpha.extend(alpha[start:start + orig_gw])
                    new_alpha.append(0)
                gw = gw + 1
                alpha = new_alpha
        bmp = bytearray()
        for row in range(gh):
            for col in range(gw):
                bmp.append(alpha[row * gw + col])
        bi = len(all_bmp)
        all_bmp.extend(bmp)
    else:
        bi = 0
    _, _, aw, _, _ = METRICS[cp]
    ox, oy = render_ox, render_oy
    entries.append((bi, aw, gw, gh, ox, oy))
    unicode_list.append(cp - 32)  # SPARSE_TINY 存的是相对偏移: codepoint - range_start

# ── 排序: LVGL SPARSE_TINY 用二分搜索, unicode_list 必须升序 ──
combined = list(zip(unicode_list, entries))
combined.sort(key=lambda x: x[0])
unicode_list = [x[0] for x in combined]
entries = [x[1] for x in combined]

# Write C file
with open(OUT_PATH, "w", encoding="utf-8") as f:
    f.write('#include "lvgl.h"\n\n')
    f.write(f"#ifndef {FONT_GUARD}\n#define {FONT_GUARD} 1\n#endif\n")
    f.write(f"#if {FONT_GUARD}\n\n")
    f.write("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n")
    for i, b in enumerate(all_bmp):
        if i % 16 == 0:
            f.write("    ")
        f.write("0x%02x," % b)
        if i % 16 == 15:
            f.write("\n")
    if len(all_bmp) % 16 != 0:
        f.write("\n")
    f.write("};\n\n")

    f.write("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
    f.write("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},\n")
    for i, (bi, aw, bw, bh, ox, oy) in enumerate(entries):
        comma = "," if i < len(entries) - 1 else ""
        f.write("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d}%s\n" % (bi, aw, bw, bh, ox, oy, comma))
    f.write("};\n\n")

    f.write("static const uint16_t unicode_list_0[] = {\n")
    for i, cp in enumerate(unicode_list):
        if i % 10 == 0:
            f.write("    ")
        f.write("0x%04x," % cp)
        if i % 10 == 9:
            f.write("\n")
    if len(unicode_list) % 10 != 0:
        f.write("\n")
    f.write("};\n\n")

    # Inclusive range through the highest sparse entry. The previous fixed
    # value ended at U+E4E4, so U+E4E5..U+E4E8 rest glyphs could never be
    # resolved even though their descriptors and bitmaps were generated.
    cmap_range_length = max(unicode_list) + 1
    f.write("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
    f.write("    {\n")
    f.write("        .range_start = 32, .range_length = %d, .glyph_id_start = 1,\n" % cmap_range_length)
    f.write("        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL,\n")
    f.write("        .list_length = %d, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n" % len(unicode_list))
    f.write("    }\n")
    f.write("};\n\n")

    f.write("#if LVGL_VERSION_MAJOR == 8\n")
    f.write("static lv_font_fmt_txt_glyph_cache_t cache;\n")
    f.write("#endif\n\n")

    f.write("#if LVGL_VERSION_MAJOR >= 8\n")
    f.write("static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
    f.write("#else\n")
    f.write("static lv_font_fmt_txt_dsc_t font_dsc = {\n")
    f.write("#endif\n")
    f.write("    .glyph_bitmap = glyph_bitmap,\n")
    f.write("    .glyph_dsc = glyph_dsc,\n")
    f.write("    .cmaps = cmaps,\n")
    f.write("    .kern_dsc = NULL,\n")
    f.write("    .kern_scale = 0,\n")
    f.write("    .cmap_num = 1,\n")
    f.write("    .bpp = %d,\n" % BPP)
    f.write("    .kern_classes = 0,\n")
    f.write("    .bitmap_format = 0,\n")
    f.write("    .stride = 0,\n")
    f.write("#if LVGL_VERSION_MAJOR == 8\n")
    f.write("    .cache = &cache\n")
    f.write("#endif\n")
    f.write("};\n\n")

    f.write("#if LVGL_VERSION_MAJOR >= 8\n")
    f.write(f"const lv_font_t {FONT_SYMBOL} = {{\n")
    f.write("#else\n")
    f.write(f"lv_font_t {FONT_SYMBOL} = {{\n")
    f.write("#endif\n")
    f.write("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n")
    f.write("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n")
    f.write("    .line_height = %d,\n" % LINE_HEIGHT)
    f.write("    .base_line = %d,\n" % BASE_LINE)
    f.write("    .subpx = LV_FONT_SUBPX_NONE,\n")
    f.write("    .underline_position = -7,\n")
    f.write("    .underline_thickness = 5,\n")
    f.write("    .dsc = &font_dsc,\n")
    f.write("    .fallback = NULL,\n")
    f.write("    .user_data = NULL,\n")
    f.write("};\n\n")
    f.write(f"#endif /* {FONT_GUARD} */\n")

print("\nDone! Output:", OUT_PATH)
print("Bitmap size:", len(all_bmp), "bytes")
print("Glyphs:", len(results))
