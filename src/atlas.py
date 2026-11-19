# -*- coding: utf-8 -*-
"""Bake a binary atlas (glyph bitmaps, app-icon tiles, UI masks) for the Launcher."""
import importlib.util, os, struct, sys
from PIL import Image, ImageDraw, ImageFont

B = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(B, 'atlas.bin')

MSYH = r'C:\Windows\Fonts'
RUBIK_VF = os.path.join(B, 'fonts', 'Rubik-VF.ttf')

# family -> ordered (weight label, font path, variable-instance name)
FAMILY_SPECS = [
    [('细', os.path.join(MSYH, 'msyhl.ttc'), None),
     ('常规', os.path.join(MSYH, 'msyh.ttc'), None),
     ('粗', os.path.join(MSYH, 'msyhbd.ttc'), None)],
    [('细', os.path.join(B, 'fonts', 'MI_LanTing_Light.ttf'), None),
     ('常规', os.path.join(B, 'fonts', 'MI_LanTing.ttf'), None),
     ('粗', os.path.join(B, 'fonts', 'MI_LanTing_Bold.ttf'), None)],
    [('细', RUBIK_VF, 'Light'),
     ('常规', RUBIK_VF, 'Regular'),
     ('中等', RUBIK_VF, 'Medium'),
     ('粗', RUBIK_VF, 'Bold')],
]
FACES = [(fi, wi, label, path, var)
         for fi, ws in enumerate(FAMILY_SPECS)
         for wi, (label, path, var) in enumerate(ws)]
SIZES = [16, 18, 20, 26, 38, 72, 96, 120]
CJK_SIZES = {16, 18, 20, 26}

CJK = (
    "室内空气客厅刚刚更新温度湿度光照优良中差偏湿舒适明亮干燥暗弱"
    "时钟天气相册音乐设置计算器日历相机浏览器地图文件视频便签商店工具电话图库"
    "搜索应用质量信息添加至主屏幕小组件移除深浅主题已切换钟点分秒"
    "月日周一二三四五六余额度电池充示例数据"
)
CJK += "凉热好无响应未接入°μ³²—…·色选择高关于本机更换主题壁纸明暗重启原固件型号米家空气检测仪系统界面自建运行点任意位置关闭出落返回开发轻样式今天星期明日周一二三四五六常规中细粗原始补偿标定基线恢复传感器初始化上次分钟静置自动扫描连接网络选择开始完成取消确定失败成功学习参考把放到通风良好新鲜空气处约后读取保存开关显示字体族字重以及检测数值正常异常倾向偏高偏低说明详情状态未"
CJK += "串从依修停前口含子当来板止源粒进（）"
CJK += "不剩可境备安持查环碰要触试请需，"
CJK += "质量主导污染重严据等级物浓"
CJK += "大渐小"
CJK += "晴云雾雷阵雨雪风感体最获取失败连给次天差当前预报"
CJK += "多阴第少毛冻米暴冰雹知"
CJK += "页先分秒"
CJK += "络附近"
CJK += "雅黑粗体常规亮度自动手动开机启动延迟秒钟小时上午下午今明日节气"
CJK += "变预报网络密码连接扫描线开关闭启用停刷新保存删除编辑城市自动定位成功失败错误超时重试搜索中请稍候载入正在已未输入确认取消"
CJK += "与书京会估供储写北华名填外尚并待挂按效暂析校格档求法满爱生略科称程缺解警证调钥闲间隔验"
CJK += "广州深圳上海北京天津重庆香港澳门台北乌鲁木齐拉萨成都杭州武汉西安南京青岛长沙沈阳厦门苏州合肥郑州济南福州南昌南宁贵阳昆明海口三亚哈尔滨长春大连宁波无锡常州佛山东莞珠海惠州中山江门肇庆清远韶关汕头湛江茂名"
CJK += "黄橙红蓝紫寒潮霜冻暴雪暴雨大风雷电冰雹高温低温沙尘雾霾干旱道路结冰森林草原火险强对流山洪地质灾害台风多云少云阴晴小雨中雨大雨阵雨毛毛雨雷阵雨雨夹雪雨雪冻雨小雪中雪大雪局部多云转晴间歇性阵雨雷暴大部分部分晴朗云量"
CJK += "均现能侧率压址"
CJK += "语言简"
CHARS = [chr(c) for c in range(32, 127)] + list(dict.fromkeys(CJK))


def spec(path):
    s = importlib.util.spec_from_file_location('mockv2', os.path.join(B, 'mock_v2.py'))
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


M = spec('mock_v2')


FB = (r'C:\Windows\Fonts\msyh.ttc', r'C:\Windows\Fonts\msyhbd.ttc')


def _render(f, ch, size, ascent, lh, pad):
    img = Image.new('L', (pad * 2 + size * 2, lh + size * 2), 0)
    ImageDraw.Draw(img).text((pad, pad + ascent), ch, font=f, fill=255, anchor='ls')
    bb = img.getbbox()
    if not bb:
        return None, None
    return img.crop(bb), bb


def open_face(path, var, size):
    f = ImageFont.truetype(path, size)
    if var:
        try:
            f.set_variation_by_name(var)
        except Exception:
            pass
    return f


def build_font(face_idx, size):
    fam_idx, weight_idx, label, path, var = FACES[face_idx]
    f = open_face(path, var, size)
    fb = ImageFont.truetype(FB[1], size)
    ascent, descent = f.getmetrics()
    lh = ascent + descent
    pad = size
    notdef, _ = _render(f, chr(0xE000), size, ascent, lh, pad)
    glyphs = []
    for ch in CHARS:
        is_cjk = ord(ch) > 0x2000
        if is_cjk and size not in CJK_SIZES:
            continue
        adv = int(round(f.getlength(ch)))
        if ch == ' ':
            glyphs.append((ord(ch), adv, 0, 0, 0, 0, b''))
            continue
        g, bb = _render(f, ch, size, ascent, lh, pad)
        # glyph missing from this family -> borrow it from the system CJK face
        if g is None or (notdef is not None and g.tobytes() == notdef.tobytes()):
            g, bb = _render(fb, ch, size, ascent, lh, pad)
            adv = int(round(fb.getlength(ch)))
        if g is None:
            glyphs.append((ord(ch), adv, 0, 0, 0, 0, b''))
            continue
        glyphs.append((ord(ch), adv, bb[0] - pad, bb[1] - pad - ascent, g.width, g.height, g.tobytes()))
    return glyphs, lh, ascent


WANT = ['时钟', '天气', '设置', '计算器', '日历', '便签', '文件']


def build_icons():
    tiles = []
    # keep only the apps we can really implement on this device
    order = {w: i for i, w in enumerate(WANT)}
    apps = sorted([a for a in M.APPS if a[0] in order], key=lambda a: order[a[0]])
    for name, bg, fn in apps:
        img = Image.new('RGBA', (72, 72), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((0, 0, 71, 71), radius=22, fill=bg)
        fn(d, 36, 36, (255, 255, 255), 72 / 64.0 * 1.15)
        tiles.append(img)
    return tiles


def build_masks():
    out = {}
    w, h = 28, 22
    img = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(img)
    c = 255
    cx, cy = 14, 12
    for r in (5.0, 10.0, 15.0):
        d.arc((cx - r, cy - r, cx + r, cy + r), start=200, end=340, fill=c, width=3)
    d.ellipse((cx - 2.5, cy - 1, cx + 2.5, cy + 4), fill=c)
    out['wifi'] = img

    w, h = 36, 18
    img = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((1, 1, 27, 16), radius=4, outline=c, width=2)
    d.rectangle((28, 6, 31, 11), fill=c)
    d.rounded_rectangle((4, 4, 24, 13), radius=2, fill=c)
    out['battery'] = img

    w, h = 22, 22
    img = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(img)
    d.ellipse((2, 2, 14, 14), outline=c, width=2)
    d.line((14, 14, 20, 20), fill=c, width=3)
    out['search'] = img

    w, h = 18, 24
    img = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((5, 1, 13, 14), radius=4, outline=c, width=2)
    d.arc((2, 6, 16, 20), start=0, end=180, fill=c, width=2)
    d.line((9, 20, 9, 24), fill=c, width=2)
    out['mic'] = img
    return out


def main():
    fonts = []
    for face_idx in range(len(FACES)):
        for si, size in enumerate(SIZES):
            fonts.append(build_font(face_idx, size))
    icons = build_icons()
    masks = build_masks()

    blob = bytearray()
    face_recs = []
    for glyphs, lh, ascent in fonts:
        rec = bytearray()
        for cp, adv, bx, by, w, h, data in glyphs:
            off = len(blob)
            blob += data
            rec += struct.pack('<IHhhHHII', cp, adv, bx, by, w, h, off, len(data))
        face_recs.append((bytes(rec), lh, ascent, len(glyphs)))

    icon_recs = []
    for img in icons:
        off = len(blob)
        blob += img.tobytes()
        icon_recs.append((off, 72, 72))
    mask_recs = []
    for k in ('wifi', 'battery', 'search', 'mic'):
        img = masks[k]
        off = len(blob)
        blob += img.tobytes()
        mask_recs.append((off, img.width, img.height))

    # layout: header(32) | face_dir(12*n) | icon_dir(12*n) | mask_dir(12*n) | face bodies | blob
    nf, ni, nm = len(face_recs), len(icon_recs), len(mask_recs)
    face_dir_off = 32
    icon_dir_off = face_dir_off + 12 * nf
    mask_dir_off = icon_dir_off + 12 * ni
    body_off = mask_dir_off + 12 * nm
    bodies = bytearray()
    face_dir = bytearray()
    for rec, lh, ascent, n in face_recs:
        face_dir += struct.pack('<III', body_off + len(bodies), n, (lh << 16) | (ascent & 0xFFFF))
        bodies += rec
    blob_off = body_off + len(bodies)

    out = bytearray()
    out += struct.pack('<IIHHHHIII', 0x314C5441, face_dir_off, nf, ni, nm, 0, icon_dir_off, mask_dir_off, blob_off)
    out += b'\0\0\0\0'  # pad header to the 32-byte offset the directory offsets assume
    out += face_dir
    for off, w, h in icon_recs:
        out += struct.pack('<IHHI', off, w, h, 0)
    for off, w, h in mask_recs:
        out += struct.pack('<IHHI', off, w, h, 1)
    out += bodies
    out += blob
    with open(OUT, 'wb') as f:
        f.write(out)
    print('faces', nf, 'icons', ni, 'masks', nm, 'total', len(out), 'blob', len(blob))


main()

