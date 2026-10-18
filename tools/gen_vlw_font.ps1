# Rasterize a TrueType font into LovyanGFX VLW anti-aliased font using Windows GDI+.
#
#   VLW format (all fields big-endian u32):
#     header 24 bytes : glyphCount, version, yAdvance, reserved, ascent, descent
#     per glyph 28 bytes : unicode, height, width, xAdvance, dY, dX, reserved
#     bitmap block : in record order, width*height bytes each, 1 byte/pixel alpha (0 clear, 255 opaque)
#     records MUST be sorted by ascending unicode (LovyanGFX uses lower_bound)
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\gen_vlw_font.ps1 `
#       -Family YouYuan -Px 16 -Charset gb2312 -Out fonts\ui16.vlw
#
# NOTE: keep this file pure ASCII. PowerShell 5.1 reads .ps1 with the ANSI codepage on
# Chinese Windows, so non-ASCII bytes here would be garbled and break Add-Type.

param(
    [string]$Family  = "YouYuan",
    [int]$Px         = 20,
    [string]$Charset = "gb2312",     # gb2312 | gb2312-l1 | ascii
    [string]$Out     = "fonts\ui16.vlw"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$code = @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public static class VlwGen
{
    static Bitmap _bmp;
    static Graphics _g;
    static Font _font;
    static StringFormat _sf;
    static Brush _brush;
    static int _side;
    static byte[] _pix;
    static int _baseline;      // distance from render origin down to the baseline

    public static string Run(string family, int px, string charset, string outPath)
    {
        var fam = FindFamily(family);
        // YouYuan has no real bold face - GDI+ simulates it (embolden + wider advance).
        _font = new Font(fam, px, FontStyle.Bold, GraphicsUnit.Pixel);
        _sf = StringFormat.GenericTypographic;
        _sf.FormatFlags |= StringFormatFlags.MeasureTrailingSpaces;
        _side = (int)Math.Ceiling(px * 2.0) + 40;
        _pix = new byte[_side * _side * 4];
        _bmp = new Bitmap(_side, _side, PixelFormat.Format32bppArgb);
        _g = Graphics.FromImage(_bmp);
        // AntiAliasGridFit keeps the anti-aliased edges but snaps stems onto the pixel
        // grid, so fully covered pixels really reach coverage 255. Plain AntiAlias leaves
        // the glyph core around 200-220, which reads as "washed out / semi transparent".
        _g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
        _g.SmoothingMode = SmoothingMode.AntiAlias;
        // HighQuality offsets the raster by half a pixel and blurs every edge - keep None.
        _g.PixelOffsetMode = PixelOffsetMode.None;
        _brush = new SolidBrush(Color.FromArgb(255, 255, 255, 255));

        const int OX = 20, OY = 20;

        // Calibrate the baseline with 'H': its flat bottom sits exactly on the baseline.
        int[] inkH = Ink('H', OX, OY);
        if (inkH == null) throw new Exception("cannot calibrate baseline with 'H'");
        _baseline = inkH[3] - OY;

        var cps = BuildCharset(charset);
        var recs = new List<int[]>();      // [uni, w, h, adv, dY, dX]
        var bits = new List<byte[]>();

        int maxAscent = 0, maxDescent = 0;

        foreach (int cp in cps)
        {
            char ch = (char)cp;
            int[] ink = Ink(ch, OX, OY);
            int adv = Advance(ch);

            int w = 0, h = 0, dX = 0, dY = 0;
            byte[] sub = null;
            if (ink != null)
            {
                w = ink[2] - ink[0] + 1;
                h = ink[3] - ink[1] + 1;
                dX = ink[0] - OX;
                dY = (OY + _baseline) - ink[1];
                sub = new byte[w * h];
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                    {
                        int si = ((ink[1] + y) * _side + (ink[0] + x)) * 4 + 2;   // R channel = coverage
                        sub[y * w + x] = _pix[si];
                    }
                if (ch != '\u3000')
                {
                    if (dY > maxAscent) maxAscent = dY;
                    if (h - dY > maxDescent) maxDescent = h - dY;
                }
            }
            if (w > 255 || h > 255) throw new Exception("glyph too large: U+" + cp.ToString("X4"));
            if (dX < -128 || dX > 127) throw new Exception("dX out of range: U+" + cp.ToString("X4"));
            if (adv > 255) adv = 255;
            if (adv < 0) adv = 0;

            recs.Add(new int[] { cp, w, h, adv, dY, dX });
            bits.Add(sub);
        }

        // sort by ascending unicode
        var order = new int[recs.Count];
        for (int i = 0; i < order.Length; i++) order[i] = i;
        Array.Sort(order, (a, b) => recs[a][0].CompareTo(recs[b][0]));

        using (var fs = new FileStream(outPath, FileMode.Create, FileAccess.Write))
        using (var bw = new BinaryWriter(fs))
        {
            Write32(bw, recs.Count);
            Write32(bw, 3);                 // vlw encoder version
            Write32(bw, px);                // yAdvance placeholder, recomputed on load
            Write32(bw, 0);
            Write32(bw, maxAscent);
            Write32(bw, maxDescent);

            foreach (int i in order)
            {
                var r = recs[i];
                Write32(bw, r[0]);          // unicode
                Write32(bw, r[2]);          // height
                Write32(bw, r[1]);          // width
                Write32(bw, r[3]);          // xAdvance
                Write32(bw, r[4]);          // dY
                Write32(bw, r[5]);          // dX
                Write32(bw, 0);
            }
            foreach (int i in order)
                if (bits[i] != null) bw.Write(bits[i]);
        }

        long len = new FileInfo(outPath).Length;
        var sb = new StringBuilder();
        sb.AppendLine(string.Format("[VLW] {0} family={1} px={2} chars={3} ascent={4} descent={5} size={6}KB",
                    Path.GetFileName(outPath), fam.Name, px, recs.Count, maxAscent, maxDescent, len / 1024));
        sb.AppendLine(string.Format("[VLW] advance A={0} H={1} 0={2} U+4E2D={3} U+3002={4} baseline={5}",
                    Advance('A'), Advance('H'), Advance('0'), Advance('\u4E2D'), Advance('\u3002'), _baseline));
        return sb.ToString();
    }

    static FontFamily FindFamily(string name)
    {
        foreach (var f in FontFamily.Families)
            if (string.Equals(f.Name, name, StringComparison.OrdinalIgnoreCase)) return f;
        foreach (var f in FontFamily.Families)
            if (f.Name.IndexOf(name, StringComparison.OrdinalIgnoreCase) >= 0) return f;
        throw new Exception("font family not found: " + name);
    }

    static int Advance(char ch)
    {
        var s = _g.MeasureString(ch.ToString(), _font, new PointF(0, 0), _sf);
        return (int)Math.Round(s.Width);
    }

    // returns [minX, minY, maxX, maxY] of the ink box (coverage >= 8); null when blank
    static int[] Ink(char ch, int ox, int oy)
    {
        // Opaque black background + white brush: R/G/B carry the coverage (255 = solid
        // ink). Reading the ARGB alpha on a transparent surface gave cores around 200
        // instead of 255, which made every glyph look translucent.
        _g.Clear(Color.FromArgb(255, 0, 0, 0));
        _g.DrawString(ch.ToString(), _font, _brush, ox, oy, _sf);

        var bd = _bmp.LockBits(new Rectangle(0, 0, _side, _side),
                               ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        Marshal.Copy(bd.Scan0, _pix, 0, _pix.Length);
        _bmp.UnlockBits(bd);

        int minX = int.MaxValue, minY = int.MaxValue, maxX = -1, maxY = -1;
        for (int y = 0; y < _side; y++)
            for (int x = 0; x < _side; x++)
                if (_pix[(y * _side + x) * 4 + 2] >= 8)
                {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
        if (maxX < 0) return null;
        return new int[] { minX, minY, maxX, maxY };
    }

    static void Write32(BinaryWriter bw, int v)
    {
        bw.Write((byte)((v >> 24) & 0xFF));
        bw.Write((byte)((v >> 16) & 0xFF));
        bw.Write((byte)((v >> 8) & 0xFF));
        bw.Write((byte)(v & 0xFF));
    }

    static List<int> BuildCharset(string spec)
    {
        var set = new SortedSet<int>();

        for (int c = 0x20; c <= 0x7E; c++) set.Add(c);   // ASCII

        int[] extra = {
            0x00B0, 0x00B7, 0x00D7, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026, 0x2103,
            0x3000, 0x3001, 0x3002, 0x300A, 0x300B, 0x300C, 0x300D, 0x300E, 0x300F,
            0x3010, 0x3011, 0x3014, 0x3015,
            0xFF01, 0xFF08, 0xFF09, 0xFF0C, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF5E, 0xFFE0, 0xFFE1
        };
        foreach (int c in extra) set.Add(c);

        if (spec != "ascii")
        {
            var gb = Encoding.GetEncoding(936);
            int lastQu = (spec == "gb2312-l1") ? 55 : 87;  // 16..55 level-1 hanzi, 56..87 level-2
            for (int qu = 16; qu <= lastQu; qu++)
                for (int wei = 1; wei <= 94; wei++)
                {
                    byte[] b = { (byte)(0xA0 + qu), (byte)(0xA0 + wei) };
                    string s = gb.GetString(b);
                    if (s.Length == 1) set.Add(s[0]);
                }
        }

        return new List<int>(set);
    }
}
'@

Add-Type -TypeDefinition $code -Language CSharp -ReferencedAssemblies System.Drawing

$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
$dir = Split-Path -Parent $Out
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }

$sw = [System.Diagnostics.Stopwatch]::StartNew()
[VlwGen]::Run($Family, $Px, $Charset, $Out)
$sw.Stop()
"elapsed: {0:N1}s" -f $sw.Elapsed.TotalSeconds