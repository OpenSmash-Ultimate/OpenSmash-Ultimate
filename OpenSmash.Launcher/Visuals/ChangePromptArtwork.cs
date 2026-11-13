using System.Drawing;
namespace OpenSmash.Visuals;
public sealed partial class ClassicSurface
{
        void PaintPluginPrompt(Graphics g, int W, int H)
        {
            float s = _s, e = ArtworkTheme.Ease(Clamp01((float)(Now - _pluginPromptAt) / 0.4f));
            using (var b = new SolidBrush(Color.FromArgb((int)(200 * e), 5, 5, 5))) g.FillRectangle(b, 0, 0, W, H);
            AddHit("plugin_prompt:block", 0, 0, W, H);

            var st = PushFade(g, e, 24 * s);
            float mw = 640 * s, mh = 320 * s, mx = (W - mw) / 2, my = (H - mh) / 2;
            using (var p = ArtworkTheme.CutCorners(mx, my, mw, mh, 30 * s))
            {
                using (var b = new SolidBrush(A(ArtworkTheme.Bg2))) g.FillPath(b, p);
                using (var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
                var cs = g.Save();
                g.SetClip(p, CombineMode.Intersect);
                using (var b = new SolidBrush(A(ArtworkTheme.Accent))) g.FillRectangle(b, mx, my, mw, 4 * s);
                g.Restore(cs);
            }
            float pad = 32 * s;
            bool refresh = _pluginPromptKind == "refresh";
            ArtworkTheme.Label(g, refresh ? "REFRESH PLUGIN" : "UPDATE PLUGINS", FD(32 * s), A(ArtworkTheme.Text), mx + pad, my + 34 * s, 30 * s, 0.32f * s);
            ArtworkTheme.DrawWrapped(g, _pluginPromptText, FB(15 * s), A(ArtworkTheme.Muted), mx + pad, my + 84 * s, mw - pad * 2, 130 * s);

            float bh = 46 * s, by = my + mh - bh - 28 * s, gap = 12 * s;
            float wBuild = 230 * s, wPlay = 160 * s, wCancel = 120 * s;
            float xBuild = mx + mw - pad - wBuild;
            float xPlay = xBuild - gap - wPlay;
            float xCancel = xPlay - gap - wCancel;
            ArtworkTheme.ButtonAnimated(g, "CANCEL", xCancel, by, wCancel, bh, s, false, Hov("plugin_prompt:cancel", _hover == "plugin_prompt:cancel"), true, 13 * s, _alpha, press: Press("plugin_prompt:cancel"));
            AddHit("plugin_prompt:cancel", xCancel, by, wCancel, bh);
            ArtworkTheme.ButtonAnimated(g, "PLAY ANYWAY", xPlay, by, wPlay, bh, s, false, Hov("plugin_prompt:play", _hover == "plugin_prompt:play"), true, 13 * s, _alpha, press: Press("plugin_prompt:play"));
            AddHit("plugin_prompt:play", xPlay, by, wPlay, bh);
            ArtworkTheme.ButtonAnimated(g, refresh ? "REFRESH PLUGIN" : "UPDATE PLUGINS", xBuild, by, wBuild, bh, s, true, Hov("plugin_prompt:build", _hover == "plugin_prompt:build"), true, 13 * s, _alpha, press: Press("plugin_prompt:build"));
            AddHit("plugin_prompt:build", xBuild, by, wBuild, bh);
            PopFade(g, st);
        }
        void PaintNewModsPrompt(Graphics g, int W, int H)
        {
            float s = _s, e = ArtworkTheme.Ease(Clamp01((float)(Now - _newModsPromptAt) / 0.4f));
            using (var b = new SolidBrush(Color.FromArgb((int)(200 * e), 5, 5, 5))) g.FillRectangle(b, 0, 0, W, H);
            AddHit("newmods:block", 0, 0, W, H);
            var st = PushFade(g, e, 24 * s);
            float mw = 660 * s, mh = 330 * s, mx = (W - mw) / 2, my = (H - mh) / 2;
            using (var p = ArtworkTheme.CutCorners(mx, my, mw, mh, 30 * s))
            {
                using (var b = new SolidBrush(A(ArtworkTheme.Bg2))) g.FillPath(b, p);
                using (var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
                var cs = g.Save();
                g.SetClip(p, CombineMode.Intersect);
                using (var b = new SolidBrush(A(ArtworkTheme.Accent))) g.FillRectangle(b, mx, my, mw, 4 * s);
                g.Restore(cs);
            }
            float pad = 32 * s;
            int count = _newMods.Count;
            bool build = _newMods.Any(n => !ModHasPlugin(n));
            ArtworkTheme.Label(g, count == 1 ? "NEW MOD FOUND" : "NEW MODS FOUND", FD(32 * s), A(ArtworkTheme.Text), mx + pad, my + 34 * s, 30 * s, 0.32f * s);
            string names = string.Join(", ", _newMods.Take(4));
            if (count > 4) names += $" and {count - 4} more";
            string body = count == 1
                ? $"{names} was dropped straight into the mods folder, so OpenSmash has not built its plugin yet."
                : $"{names} were dropped straight into the mods folder, so OpenSmash has not built their plugins yet.";
            body += " This must be done now: leaving it unbuilt breaks the rest of your mods.";
            if (build)
                body += count == 1
                    ? " Build the plugin now so the game loads it. Only the plugin is built, not the whole game."
                    : " Build the plugins now so the game loads them. Only the plugins are built, not the whole game.";
            else
                body += count == 1 ? " Add it now so the game loads it." : " Add them now so the game loads them.";
            ArtworkTheme.DrawWrapped(g, body, FB(15 * s), A(ArtworkTheme.Muted), mx + pad, my + 84 * s, mw - pad * 2, 130 * s);
            float bh = 46 * s, by = my + mh - bh - 28 * s;
            float wBuild = 260 * s;
            float xBuild = mx + (mw - wBuild) / 2;
            ArtworkTheme.ButtonAnimated(g, build ? "BUILD PLUGIN" : "ADD MOD", xBuild, by, wBuild, bh, s, true, Hov("newmods:build", _hover == "newmods:build"), true, 13 * s, _alpha, press: Press("newmods:build"));
            AddHit("newmods:build", xBuild, by, wBuild, bh);
            PopFade(g, st);
        }
        void PaintRebuildPrompt(Graphics g, int W, int H)
        {
            float s = _s, e = ArtworkTheme.Ease(Clamp01((float)(Now - _rebuildPromptAt) / 0.4f));
            using (var b = new SolidBrush(Color.FromArgb((int)(200 * e), 5, 5, 5))) g.FillRectangle(b, 0, 0, W, H);
            AddHit("rebuild_prompt:block", 0, 0, W, H);
            var st = PushFade(g, e, 24 * s);
            float mw = 700 * s, mh = 360 * s, mx = (W - mw) / 2, my = (H - mh) / 2;
            using (var p = ArtworkTheme.CutCorners(mx, my, mw, mh, 30 * s))
            {
                using (var b = new SolidBrush(A(ArtworkTheme.Bg2))) g.FillPath(b, p);
                using (var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
                var cs = g.Save();
                g.SetClip(p, CombineMode.Intersect);
                using (var b = new SolidBrush(A(ArtworkTheme.Accent))) g.FillRectangle(b, mx, my, mw, 4 * s);
                g.Restore(cs);
            }
            float pad = 32 * s;
            ArtworkTheme.Label(g, "REBUILD PLUGINS", FD(32 * s), A(ArtworkTheme.Text), mx + pad, my + 34 * s, 30 * s, 0.32f * s);
            ArtworkTheme.DrawWrapped(g, "REFRESH PLUGIN rebuilds only the mod plugins' DLLs. The game program stays as it is. Use it when a moveset or plugin file changed.", FB(14.5f * s), A(ArtworkTheme.Muted), mx + pad, my + 84 * s, mw - pad * 2, 56 * s);
            ArtworkTheme.DrawWrapped(g, "UPDATE PLUGINS rebuilds the plugins and relinks the game program too. Needed when a mod hooks a function, patches the game's code, or adds a community plugin.", FB(14.5f * s), A(ArtworkTheme.Muted), mx + pad, my + 148 * s, mw - pad * 2, 64 * s);
            float bh = 46 * s, by = my + mh - bh - 28 * s, gap = 12 * s;
            float wUp = 190 * s, wRef = 190 * s, wCancel = 120 * s;
            float xUp = mx + mw - pad - wUp;
            float xRef = xUp - gap - wRef;
            float xCancel = xRef - gap - wCancel;
            ArtworkTheme.ButtonAnimated(g, "CANCEL", xCancel, by, wCancel, bh, s, false, Hov("rebuild_prompt:cancel", _hover == "rebuild_prompt:cancel"), true, 13 * s, _alpha, press: Press("rebuild_prompt:cancel"));
            AddHit("rebuild_prompt:cancel", xCancel, by, wCancel, bh);
            ArtworkTheme.ButtonAnimated(g, "REFRESH PLUGIN", xRef, by, wRef, bh, s, false, Hov("rebuild_prompt:refresh", _hover == "rebuild_prompt:refresh"), true, 13 * s, _alpha, press: Press("rebuild_prompt:refresh"));
            AddHit("rebuild_prompt:refresh", xRef, by, wRef, bh);
            ArtworkTheme.ButtonAnimated(g, "UPDATE PLUGINS", xUp, by, wUp, bh, s, true, Hov("rebuild_prompt:update", _hover == "rebuild_prompt:update"), true, 13 * s, _alpha, press: Press("rebuild_prompt:update"));
            AddHit("rebuild_prompt:update", xUp, by, wUp, bh);
            PopFade(g, st);
        }
        void PaintShaderPrompt(Graphics g, int W, int H)
        {
            float s = _s, e = ArtworkTheme.Ease(Clamp01((float)(Now - _shaderPromptAt) / 0.4f));
            using (var b = new SolidBrush(Color.FromArgb((int)(200 * e), 5, 5, 5))) g.FillRectangle(b, 0, 0, W, H);
            AddHit("shader_prompt:block", 0, 0, W, H);

            var st = PushFade(g, e, 24 * s);
            float mw = 760 * s, mh = 300 * s, mx = (W - mw) / 2, my = (H - mh) / 2;
            using (var p = ArtworkTheme.CutCorners(mx, my, mw, mh, 30 * s))
            {
                using (var b = new SolidBrush(A(ArtworkTheme.Bg2))) g.FillPath(b, p);
                using (var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
                var cs = g.Save();
                g.SetClip(p, CombineMode.Intersect);
                using (var b = new SolidBrush(A(ArtworkTheme.Accent))) g.FillRectangle(b, mx, my, mw, 4 * s);
                g.Restore(cs);
            }
            float pad = 32 * s;
            ArtworkTheme.Label(g, "FINISH SHADERS", FD(32 * s), A(ArtworkTheme.Text), mx + pad, my + 34 * s, 30 * s, 0.32f * s);
            ArtworkTheme.DrawWrapped(g, "Shader compile has not finished, so a new effect can stutter the first time you see it. Pick how hard this PC should work. The bar along the bottom shows the progress. You can keep playing while it runs.",
                FB(15 * s), A(ArtworkTheme.Muted), mx + pad, my + 84 * s, mw - pad * 2, 90 * s);

            float bh = 46 * s, by = my + mh - bh - 28 * s, gap = 10 * s;
            float wLater = 120 * s, wPri = 140 * s;
            float xHigh = mx + mw - pad - wPri;
            float xMed = xHigh - gap - wPri;
            float xLow = xMed - gap - wPri;
            float xLater = xLow - gap - wLater;
            ArtworkTheme.ButtonAnimated(g, "LATER", xLater, by, wLater, bh, s, false, Hov("shader_prompt:later", _hover == "shader_prompt:later"), true, 13 * s, _alpha, press: Press("shader_prompt:later"));
            AddHit("shader_prompt:later", xLater, by, wLater, bh);
            ArtworkTheme.ButtonAnimated(g, "LOW", xLow, by, wPri, bh, s, false, Hov("shader_prompt:low", _hover == "shader_prompt:low"), true, 13 * s, _alpha, press: Press("shader_prompt:low"));
            AddHit("shader_prompt:low", xLow, by, wPri, bh);
            ArtworkTheme.ButtonAnimated(g, "MEDIUM", xMed, by, wPri, bh, s, false, Hov("shader_prompt:medium", _hover == "shader_prompt:medium"), true, 13 * s, _alpha, press: Press("shader_prompt:medium"));
            AddHit("shader_prompt:medium", xMed, by, wPri, bh);
            ArtworkTheme.ButtonAnimated(g, "HIGH", xHigh, by, wPri, bh, s, true, Hov("shader_prompt:high", _hover == "shader_prompt:high"), true, 13 * s, _alpha, press: Press("shader_prompt:high"));
            AddHit("shader_prompt:high", xHigh, by, wPri, bh);
            PopFade(g, st);
        }
        void PaintUpdatePrompt(Graphics g, int W, int H)
        {
            float s = _s, e = ArtworkTheme.Ease(Clamp01((float)(Now - _updatePromptAt) / 0.4f));
            using (var b = new SolidBrush(Color.FromArgb((int)(200 * e), 5, 5, 5))) g.FillRectangle(b, 0, 0, W, H);
            AddHit("update_prompt:block", 0, 0, W, H);
            var st = PushFade(g, e, 24 * s);
            float mw = 700 * s, mh = 360 * s, mx = (W - mw) / 2, my = (H - mh) / 2;
            using (var p = ArtworkTheme.CutCorners(mx, my, mw, mh, 30 * s))
            {
                using (var b = new SolidBrush(A(ArtworkTheme.Bg2))) g.FillPath(b, p);
                using (var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
                var cs = g.Save();
                g.SetClip(p, CombineMode.Intersect);
                using (var b = new SolidBrush(A(ArtworkTheme.Accent))) g.FillRectangle(b, mx, my, mw, 4 * s);
                g.Restore(cs);
            }
            float pad = 32 * s;
            ArtworkTheme.Label(g, "UPDATE DETECTED", FD(32 * s), A(ArtworkTheme.Text), mx + pad, my + 34 * s, 30 * s, 0.32f * s);
            ArtworkTheme.DrawWrapped(g, _updateMessage, FB(16 * s), A(ArtworkTheme.Text), mx + pad, my + 84 * s, mw - pad * 2, 60 * s);
            if (!string.IsNullOrEmpty(_updateDetail))
                ArtworkTheme.DrawWrapped(g, _updateDetail, FB(13.5f * s), A(ArtworkTheme.Muted), mx + pad, my + 150 * s, mw - pad * 2, 70 * s);
            float bh = 46 * s, by = my + mh - bh - 28 * s;
            float wUpdate = 170 * s, wLater = 170 * s;
            float xUpdate = mx + mw - pad - wUpdate;
            float xLater = xUpdate - 12 * s - wLater;
            ArtworkTheme.ButtonAnimated(g, "REMIND LATER", xLater, by, wLater, bh, s, false, Hov("update_prompt:later", _hover == "update_prompt:later"), true, 13 * s, _alpha, press: Press("update_prompt:later"));
            AddHit("update_prompt:later", xLater, by, wLater, bh);
            ArtworkTheme.ButtonAnimated(g, "UPDATE", xUpdate, by, wUpdate, bh, s, true, Hov("update_prompt:update", _hover == "update_prompt:update"), !_updateApplying, 13 * s, _alpha, press: Press("update_prompt:update"));
            if (!_updateApplying) AddHit("update_prompt:update", xUpdate, by, wUpdate, bh);
            float cb = 18 * s, cbx = mx + pad, cby = by + (bh - cb) / 2;
            using (var p = ArtworkTheme.Para(cbx, cby, cb, cb, 4 * s))
            {
                using (var b = new SolidBrush(A(_updateDontShow ? ArtworkTheme.Accent : White(10)))) g.FillPath(b, p);
                using (var pen = new Pen(A(_updateDontShow ? ArtworkTheme.Accent : ArtworkTheme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
            }
            if (_updateDontShow)
            {
                using var pen = new Pen(A(ArtworkTheme.Bg), Math.Max(1.6f, 2 * s)) { LineJoin = LineJoin.Miter };
                g.DrawLines(pen, new[] { new PointF(cbx + cb * 0.22f, cby + cb * 0.52f), new PointF(cbx + cb * 0.43f, cby + cb * 0.74f), new PointF(cbx + cb * 0.80f, cby + cb * 0.28f) });
            }
            Font cf = FM(12 * s);
            float csp = 12 * s * 0.10f;
            ArtworkTheme.Label(g, "DON'T SHOW THIS AGAIN", cf, A(_updateDontShow ? ArtworkTheme.Text : ArtworkTheme.Muted), cbx + cb + 10 * s, by, bh, csp);
            AddHit("update_prompt:dont", cbx - 6 * s, by, cb + 10 * s + ArtworkTheme.TextWidth(g, "DON'T SHOW THIS AGAIN", cf, csp) + 6 * s, bh);
            PopFade(g, st);
        }

}
