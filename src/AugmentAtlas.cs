using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;
using System.Text;

// Adds only newly used CJK glyphs to the existing atlas. The next full
// atlas.py bake uses the same CJK list and replaces this temporary bridge.
public static class AugmentAtlas
{
    static PrivateFontCollection MiLight, MiRegular, MiBold;

    static PrivateFontCollection LoadMi(string root, string file)
    {
        var fonts = new PrivateFontCollection();
        fonts.AddFontFile(Path.Combine(root, "fonts", file));
        return fonts;
    }

    sealed class Face
    {
        public byte[] Records = Array.Empty<byte>();
        public int Count, LineHeight, Ascent;
    }

    static int U16(byte[] b, int p) { return BitConverter.ToUInt16(b, p); }
    static int U32(byte[] b, int p) { return BitConverter.ToInt32(b, p); }

    static byte[] Render(char ch, int size, int ascent, int family,
                         out short adv, out short bx, out short by,
                         out ushort width, out ushort height)
    {
        int pad = size, side = size * 4;
        using (var bmp = new Bitmap(side, side, PixelFormat.Format32bppArgb))
        using (var font = family >= 3 && family <= 5
                   ? new Font((family == 3 ? MiLight : family == 4 ? MiRegular : MiBold).Families[0],
                              size, FontStyle.Regular, GraphicsUnit.Pixel)
                   : new Font("Microsoft YaHei", size,
                              family == 2 || family >= 6 ? FontStyle.Bold : FontStyle.Regular,
                              GraphicsUnit.Pixel))
        using (var g = Graphics.FromImage(bmp))
        {
            g.Clear(Color.Transparent);
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.DrawString(ch.ToString(), font, Brushes.White, pad, pad,
                         StringFormat.GenericTypographic);
            adv = (short)size;
            int x0 = side, y0 = side, x1 = -1, y1 = -1;
            for (int y = 0; y < side; y++) for (int x = 0; x < side; x++)
                if (bmp.GetPixel(x, y).A != 0) {
                    if (x < x0) x0 = x; if (x > x1) x1 = x;
                    if (y < y0) y0 = y; if (y > y1) y1 = y;
                }
            if (x1 < x0) { bx = by = 0; width = height = 0; return Array.Empty<byte>(); }
            width = (ushort)(x1 - x0 + 1);
            height = (ushort)(y1 - y0 + 1);
            bx = (short)(x0 - pad);
            // GDI DrawString places its em box higher than Pillow's baseline.
            // Match the original atlas' CJK top bearing (about size/3 lower).
            by = (short)(y0 - pad - ascent + (size + 1) / 3);
            byte[] result = new byte[width * height];
            for (int y = 0; y < height; y++) for (int x = 0; x < width; x++)
                result[y * width + x] = bmp.GetPixel(x0 + x, y0 + y).A;
            return result;
        }
    }

    public static void Run(string input, string output, string missing)
    {
        string root = Path.GetDirectoryName(input) ?? ".";
        MiLight = LoadMi(root, "MI_LanTing_Light.ttf");
        MiRegular = LoadMi(root, "MI_LanTing.ttf");
        MiBold = LoadMi(root, "MI_LanTing_Bold.ttf");
        byte[] old = File.ReadAllBytes(input);
        if (U32(old, 0) != 0x314C5441) throw new Exception("bad atlas magic");
        int faceDir = U32(old, 4), nf = U16(old, 8), ni = U16(old, 10), nm = U16(old, 12);
        int iconDir = U32(old, 16), maskDir = U32(old, 20), oldBlobOff = U32(old, 24);
        Face[] faces = new Face[nf];
        using (var blob = new MemoryStream())
        {
            blob.Write(old, oldBlobOff, old.Length - oldBlobOff);
            int[] sizes = {16, 18, 20, 26, 38, 72, 96, 120};
            for (int i = 0; i < nf; i++)
            {
                int p = faceDir + i * 12, body = U32(old, p), count = U32(old, p + 4);
                var f = new Face { Count = count, LineHeight = U16(old, p + 8),
                                   Ascent = (short)U16(old, p + 10) };
                using (var recs = new MemoryStream())
                using (var wr = new BinaryWriter(recs, Encoding.UTF8, true))
                {
                    recs.Write(old, body, count * 22);
                    if (i % 8 < 4)
                    {
                        foreach (char ch in missing)
                        {
                            bool exists = false;
                            for (int k = 0; k < count; k++)
                                if (U32(old, body + k * 22) == ch) { exists = true; break; }
                            if (exists) continue;
                            short adv, bx, by; ushort w, h;
                            byte[] pixels = Render(ch, sizes[i % 8], f.Ascent, i / 8,
                                                   out adv, out bx, out by, out w, out h);
                            wr.Write((uint)ch); wr.Write((ushort)adv); wr.Write(bx); wr.Write(by);
                            wr.Write(w); wr.Write(h); wr.Write((uint)blob.Length);
                            wr.Write((uint)pixels.Length);
                            blob.Write(pixels, 0, pixels.Length);
                            f.Count++;
                        }
                    }
                    f.Records = recs.ToArray();
                }
                faces[i] = f;
            }
            int bodyOff = maskDir + nm * 12, newBlobOff = bodyOff;
            foreach (var f in faces) newBlobOff += f.Records.Length;
            using (var outputStream = File.Create(output))
            using (var outw = new BinaryWriter(outputStream))
            {
                byte[] header = new byte[32]; Array.Copy(old, header, 32);
                Array.Copy(BitConverter.GetBytes(newBlobOff), 0, header, 24, 4);
                outw.Write(header);
                int at = bodyOff;
                foreach (var f in faces) {
                    outw.Write(at); outw.Write(f.Count);
                    outw.Write((ushort)f.LineHeight); outw.Write((ushort)f.Ascent);
                    at += f.Records.Length;
                }
                outw.Write(old, iconDir, bodyOff - iconDir);
                foreach (var f in faces) outw.Write(f.Records);
                blob.Position = 0; blob.CopyTo(outputStream);
            }
        }
    }
}
