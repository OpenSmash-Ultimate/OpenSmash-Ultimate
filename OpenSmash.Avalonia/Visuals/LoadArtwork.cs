// Original process-loading artwork; maintained in the shared launcher.
using System.Drawing;
namespace OpenSmash.Visuals;
public sealed partial class ClassicSurface
{
        void PaintLoad(Graphics g, int W, int H)
        {
            float s = _s;
            float x = 104 * s;
            float w = Math.Min(640 * s, W - x - 80 * s);
            float labelY = 618 * s;
            float barY = 644 * s;
            float barH = 8 * s;
            float statusTop = H - StatusH * s;
            if (barY + barH > statusTop - 10 * s)
            {
                barY = statusTop - 18 * s;
                labelY = barY - 24 * s;
            }
            // The mods page keeps its buttons in this band, so the bar sits above them.
            float occupied = statusTop - 12 * s;
            if (_view == "mods") occupied = Math.Min(occupied, H - 104 * s - 14 * s);
            if (barY + barH > occupied)
            {
                barY = occupied - barH;
                labelY = barY - 24 * s;
            }
            string label = string.IsNullOrEmpty(_loadLabel) ? "STARTING THE GAME" : _loadLabel;
            ArtworkTheme.Label(g, label, FM(12 * s), A(ArtworkTheme.Muted), x, labelY, 18 * s, 12 * s * 0.08f);

            float cut = 5 * s;
            using (var track = ArtworkTheme.Para(x, barY, w, barH, cut))
            using (var b = new SolidBrush(A(White(16))))
                g.FillPath(b, track);

            float p0, p1;
            if (_loadProgress < 0)
            {
                float span = 0.28f;
                float t = (float)(Now % 1.3 / 1.3);
                p0 = t * (1 + span) - span;
                p1 = p0 + span;
            }
            else
            {
                p0 = 0;
                p1 = Math.Clamp(_loadProgress, 0.02f, 1f);
            }
            float x0 = x + Math.Max(0, p0) * w;
            float x1 = x + Math.Min(1, p1) * w;
            if (x1 - x0 > 1)
            {
                var st = g.Save();
                using (var clip = ArtworkTheme.Para(x, barY, w, barH, cut))
                    g.SetClip(clip, CombineMode.Intersect);
                using (var fill = ArtworkTheme.Para(x0, barY, Math.Max(barH, x1 - x0), barH, cut))
                using (var b = new SolidBrush(A(ArtworkTheme.Accent)))
                    g.FillPath(b, fill);
                g.Restore(st);
            }
        }

}
