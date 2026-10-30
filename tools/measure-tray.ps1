<#
.SYNOPSIS
    Screenshots the notification area and reports each glyph's ink bounding box.

.DESCRIPTION
    The pipe stages (build-icons.ps1 -> .ico -> LoadImageW) can all be correct while the
    result still reads wrong on screen. This measures the rendered result: it captures the
    taskbar corner in real (DPI-aware) pixels, segments it into glyph clusters, and prints
    each glyph's ink extent and vertical centre.

    Compare the second cluster (the app under test) against the shell glyphs around it.
    Judgement calls that belong to a human:

      * height should be within a couple of px of the neighbouring shell glyphs;
      * vertical centre should match them (the pre-fix build sat ~4 px low);
      * width is artwork-dependent - designs 1 and 4 are legitimately narrower than the
        shell glyphs because their artwork is, and that is reported rather than gated.

.PARAMETER Threshold
    Luminance (0-255) at which a pixel counts as ink. The taskbar is ~33, glyphs ~255.
    Lower it if dim states are missed.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-tray.ps1
#>
[CmdletBinding()]
param([int]$Threshold = 100)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$cs = @'
using System; using System.Drawing; using System.Drawing.Imaging;
using System.Collections.Generic; using System.Runtime.InteropServices;
public static class TrayMeasure {
  [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] static extern int GetSystemMetrics(int n);

  public static List<string> Capture(int thr) {
    SetProcessDpiAwarenessContext(new IntPtr(-4)); // -4 = PER_MONITOR_AWARE_V2
    int W = GetSystemMetrics(0), H = GetSystemMetrics(1);
    int cw = 760, ch = 150;
    using (var bmp = new Bitmap(cw, ch))
    using (var g = Graphics.FromImage(bmp)) {
      g.CopyFromScreen(W - cw, H - ch, 0, 0, new Size(cw, ch));
      Func<int,int,int> lum = (x,y) => { var c = bmp.GetPixel(x,y); return (int)(0.299*c.R + 0.587*c.G + 0.114*c.B); };
      // Taskbar glyphs live in the lower band; the top of the capture is usually a flyout.
      const int Y0 = 70, Y1 = 140;
      var runs = new List<int[]>();
      int start = -1;
      for (int x = 0; x < cw; x++) {
        bool on = false;
        for (int y = Y0; y < Math.Min(Y1, ch); y++) if (lum(x,y) > thr) { on = true; break; }
        if (on && start < 0) start = x;
        else if (!on && start >= 0) { if (x - start >= 2) runs.Add(new int[]{start, x-1}); start = -1; }
      }
      if (start >= 0) runs.Add(new int[]{start, cw-1});
      var merged = new List<int[]>();
      foreach (var r in runs) {
        if (merged.Count > 0 && r[0] <= merged[merged.Count-1][1] + 10) merged[merged.Count-1][1] = r[1];
        else merged.Add(new int[]{r[0], r[1]});
      }
      var outp = new List<string>();
      outp.Add("ink extent measured at luminance > " + thr + " over taskbar background");
      outp.Add("these are VISIBLE-ink extents; antialiased fringes below the threshold are excluded.");
      outp.Add("compare against another screenshot measured at the SAME threshold, never against");
      outp.Add("an ICO's alpha>0 bounding box, which counts near-transparent fringe pixels that");
      outp.Add("are invisible once composited onto the dark taskbar.");
      outp.Add("");
      outp.Add("xStart  xEnd  width centerX inkTop inkBot hgt  vCent");
      foreach (var m in merged) {
        int top = int.MaxValue, bot = -1;
        for (int y = 0; y < ch; y++)
          for (int x = m[0]; x <= m[1]; x++)
            if (lum(x,y) > thr) { if (y < top) top = y; if (y > bot) bot = y; }
        if (bot < 0) continue;
        outp.Add(string.Format("{0,6} {1,5} {2,6} {3,6} {4,7} {5,6} {6,4} {7,6}",
          m[0], m[1], m[1]-m[0]+1, (m[0]+m[1])/2, top, bot, bot-top+1, (top+bot)/2.0));
      }
      return outp;
    }
  }
}
'@
Add-Type -TypeDefinition $cs -ReferencedAssemblies System.Drawing

"screen DPI-aware metrics; threshold $Threshold"
[TrayMeasure]::Capture($Threshold) | ForEach-Object { $_ }
""
"Cluster 2 is normally the app under test; the rest are shell glyphs (volume, wifi, battery, clock)."
"Compare its hgt and vCent against its neighbours."
