// Original Windows drawing layouts, now maintained in the shared launcher.
using System.Drawing;
namespace OpenSmash.Visuals;
public sealed partial class ClassicSurface
{
        static readonly Dictionary<string, string> SettingInfo = new()
        {
            ["window"] = "A window you can resize, borderless over the monitor, or full screen. F11 switches while playing.",
            ["window_size"] = "The inside of the window. The picture stays 16:9.",
            ["pacing"] = "Steady 60 on any monitor. G-Sync / FreeSync shows each frame as soon as it is ready.",
            ["preload_shaders"] = "Runs the installer's shader compilation. Progress appears along the bottom; completed work is kept.",
            ["render_scale"] = "How big the game draws. 2 is four times the pixels of 1.",
            ["gpu"] = "Auto picks the fastest card.",
            ["renderer"] = "Auto uses Vulkan on AMD, NVIDIA and Intel graphics. Direct3D 11 is available as a fallback.",
            ["hide_cursor"] = "Hides the pointer over the game after 2 seconds without moving it.",
            ["overlay"] = "The frame rate in the corner. F1 shows or hides it while playing.",
            ["volume"] = "The game's whole sound (display.ini volume, the same setting as the in-game menu's Volume row). 100 is full. It applies the next time you press Play.",
            ["name"] = "What other players see. Leave it empty and the game picks one.",
            ["lobby"] = "Public: everyone's rooms. Private: only players with the same code. Offline: no network.",
            ["code"] = "A word your friends type too. Capital letters count.",
            ["server"] = "host:port of a lobby you run, for example 192.168.1.20:39420.",
            ["direct"] = "On is faster. Off sends everything through the lobby, so your address stays private.",
            ["keyboard_player"] = "The keyboard adds to that player's controller.",
            ["background"] = "Off: controllers work only while the game's window is in front.",
            ["pads"] = "Pins and button layouts. Each controller is also set up in the game, under Controllers.",
            ["hot_reload_mod_assets"] = "Applies next game launch. Refreshes changed files that fit loaded buffers. Larger files and GPU textures/models need a new match.",
        };

        void PaintSettings(Graphics g, int W, int H)
        {
            float s = _s, x = Gutter * s, cw = W - 2 * x;
            PaintSectionHead(g, "03 / SETTINGS", "SETTINGS");

            var vis = GetVisibleSettings();
            bool developer = _settingsTabs[Math.Clamp(_settingsTab, 0, _settingsTabs.Length - 1)].Name == "Developer";
            float headerH = 34 * s, rowH = (developer ? 150 : 74) * s;
            _settingsRowH = rowH;
            int tab = _settingsTabs.Length == 0 ? 0 : Math.Clamp(_settingsTab, 0, _settingsTabs.Length - 1);
            _settingsTab = tab;
            int pageStart = SectionStart(tab);
            int pageCount = tab < _settingsTabs.Length ? _settingsTabs[tab].SettingKeys.Length : 0;
            if (pageCount > 0 && (_sel < pageStart || _sel >= pageStart + pageCount)) _sel = pageStart;
            var rowTops = new float[vis.Length];
            float content = headerH;
            int cursor = pageStart;
            if (tab < _settingsTabs.Length)
            {
                foreach (var key in _settingsTabs[tab].SettingKeys)
                {
                    if (cursor >= vis.Length || vis[cursor].Key != key) continue;
                    rowTops[cursor] = content;
                    content += rowH;
                    cursor++;
                }
            }
            content += 12 * s;
            _sectionTop = new[] { 0f };
            _rowTop = rowTops;

            float listTop = ContentTop * s + 52 * s;
            float listBottom = H - StatusH * s - 8 * s;
            float viewH = Math.Max(rowH, listBottom - listTop);
            _settingsViewTop = listTop;
            _settingsViewH = viewH;
            _settingsMaxScroll = Math.Max(0, content - viewH);
            if (_settingsScroll > _settingsMaxScroll) _settingsScroll = _settingsMaxScroll;

            int active = tab;

            {
                float ty = ContentTop * s - 4 * s, th = 38 * s;
                float px = 12 * s, gap = 6 * s, pad = 28 * s;
                Font f = FM(px);
                float total = _settingsTabs.Sum(tb => ArtworkTheme.TextWidth(g, tb.Name.ToUpperInvariant(), f, px * 0.1f) + pad) + gap * (_settingsTabs.Length - 1);
                if (total > cw) { px *= cw / total; f = FM(px); pad *= cw / Math.Max(total, 1); }
                float tx = x;
                for (int t = 0; t < _settingsTabs.Length; t++)
                {
                    string id = $"tab:{t}", name = _settingsTabs[t].Name.ToUpperInvariant();
                    bool on = t == active;
                    var st = PushReveal(g, t, repeat: false);
                    float tw = ArtworkTheme.TextWidth(g, name, f, px * 0.1f) + pad;
                    float hv = Hov(id, _hover == id);
                    using (var p = ArtworkTheme.Para(tx, ty, tw, th, 6 * s))
                    {
                        if (on) using (var b = new SolidBrush(A(ArtworkTheme.Text))) g.FillPath(b, p);
                        using (var pen = new Pen(A(on ? ArtworkTheme.Text : ArtworkTheme.Mix(ArtworkTheme.Line, ArtworkTheme.Text, hv)), Math.Max(1, s))) g.DrawPath(pen, p);
                    }
                    ArtworkTheme.Label(g, name, f, A(on ? ArtworkTheme.Bg : ArtworkTheme.Mix(ArtworkTheme.Muted, ArtworkTheme.Text, hv)), tx, ty, th, px * 0.1f, StringAlignment.Center, tw);
                    AddHit(id, tx, ty, tw, th);
                    PopFade(g, st);
                    tx += tw + gap;
                }
            }

            if (tab < _settingsTabs.Length && string.Equals(_settingsTabs[tab].Name, "Storage", StringComparison.OrdinalIgnoreCase))
            {
                _settingsMaxScroll = 0;
                _settingsScroll = 0;
                if (_storageText.Length == 0 && !_storageBusy) Avalonia.Threading.Dispatcher.UIThread.Post(() => _ = StorageStatusAsync());
                PaintStorageSettings(g, W, H, x, cw, listTop, viewH);
                return;
            }
            if (tab < _settingsTabs.Length && string.Equals(_settingsTabs[tab].Name, "Update", StringComparison.OrdinalIgnoreCase))
            {
                _settingsMaxScroll = 0;
                _settingsScroll = 0;
                PaintUpdateSettings(g, W, H, x, cw, listTop, viewH);
                return;
            }

            _volTrackValid = false;
            Font valueFont = FM(13 * s);
            float valueSpacing = 13 * s * 0.06f;
            float longestValue = 0;
            foreach (var set in _settings)
            {
                if (set.Labels == null) continue;
                for (int vi = 0; vi < set.Labels.Length; vi++)
                {
                    // A fixed monospace budget keeps the established settings columns
                    // stable when the automatic renderer's description changes.
                    string lab = set.Key=="renderer"&&vi==0?new string('M',48):set.Labels[vi];
                    if (string.IsNullOrEmpty(lab)) continue;
                    longestValue = Math.Max(longestValue, ArtworkTheme.TextWidth(g, set.Text ? lab : lab.ToUpperInvariant(), valueFont, valueSpacing));
                }
            }
            float ctlW = Math.Clamp(longestValue + 72 * s, 260 * s, Math.Max(260 * s, cw - 340 * s));
            if (developer) ctlW = 260 * s;
            float ctlX = x + cw - ctlW, ctlH = 40 * s;
            var clip = g.Save();
            g.SetClip(new RectangleF(x - 8 * s, listTop, cw + 36 * s, viewH));
            HLine(g, x, x + cw, listTop + (0 - _settingsScroll));
            int openIdx = -1;
            cursor = pageStart;
            if (tab < _settingsTabs.Length)
            {
                float hy = listTop + (0 - _settingsScroll);
                string name = _settingsTabs[tab].Name.ToUpperInvariant();
                Font hf = FM(12 * s);
                ArtworkTheme.Label(g, name, hf, A(ArtworkTheme.Accent), x, hy, headerH, 12 * s * 0.16f);
                float rule = x + ArtworkTheme.TextWidth(g, name, hf, 12 * s * 0.16f) + 14 * s;
                if (rule < x + cw) HLine(g, rule, x + cw, hy + headerH / 2);
                int local = 0;
                foreach (var key in _settingsTabs[tab].SettingKeys)
                {
                    if (cursor >= vis.Length || vis[cursor].Key != key) continue;
                    var set = vis[cursor];
                    int idx = Array.IndexOf(_settings, set);
                    int i = cursor;
                    cursor++;
                    local++;
                    string id = $"set:{idx}";
                    bool isOpen = _openDropdownKey == set.Key;
                    if (isOpen) openIdx = i;
                    bool enabled = SettingOn(set);
                    bool on = enabled && (_hover == id || _hover == id + ":dd" || isOpen || (_keys && _sel == i));
                    float e = ArtworkTheme.Ease(Hov(id, on));
                    float ry = listTop + (rowTops[i] - _settingsScroll);
                    bool inView = ry + rowH > listTop && ry < listTop + viewH;
                    var st = PushReveal(g, i + 1);

                    RowWash(g, x, ry, cw, rowH, e);
                    float shift = 16 * s * e;
                    var num = TextPath(local.ToString("00"), 40 * s);
                    var nb = num.GetBounds();
                    var ns = g.Save();
                    g.TranslateTransform(x + shift, ry + (rowH - nb.Height) / 2 - nb.Top);
                    using (var pen = new Pen(A(ArtworkTheme.Mix(ArtworkTheme.Dim, ArtworkTheme.Accent, e)), Math.Max(1, 1.5f * s))) g.DrawPath(pen, num);
                    g.Restore(ns);

                    float lx = x + 76 * s + shift;
                    if (developer)
                        ArtworkTheme.DrawWrapped(g, set.Label.ToUpperInvariant(), FD(24 * s), A(ArtworkTheme.Text), lx, ry + 14 * s, ctlX - lx - 24 * s, 68 * s);
                    else
                        ArtworkTheme.Label(g, set.Label.ToUpperInvariant(), FD(24 * s), A(enabled ? ArtworkTheme.Text : ArtworkTheme.Dim), lx, ry + 14 * s, 22 * s, 0.24f * s);
                    if (developer)
                        ArtworkTheme.DrawWrapped(g, SettingInfo[set.Key], FB(13.5f * s), A(ArtworkTheme.Muted), lx, ry + 86 * s, ctlX - lx - 24 * s, 52 * s);
                    else
                        ArtworkTheme.DrawText(g, SettingInfo.GetValueOrDefault(set.Key, ""), FB(13.5f * s), A(enabled ? ArtworkTheme.Muted : ArtworkTheme.Dim), lx, ry + 42 * s, ctlX - lx - 24 * s, 20 * s);

                    float cy = ry + (rowH - ctlH) / 2;
                    string val = ShownValue(set);
                    if (set.Text && inView)
                        _editRects[set.Key] = new Rectangle((int)ctlX, (int)cy, (int)ctlW, (int)ctlH);
                    if (set.Action != null)
                    {
                        ArtworkTheme.ButtonAnimated(g, val.ToUpperInvariant(), ctlX, cy, ctlW, ctlH, s, false,
                            Hov(id + ":dd", enabled && _hover == id + ":dd"), enabled, 13 * s, _alpha, press: Press(id + ":dd"));
                    }
                    else if (set.Slider)
                    {
                        if (inView) PaintVolume(g, ctlX, cy, ctlW, ctlH);
                    }
                    else
                    {
                        float fh = Hov(id + ":dd", enabled && (_hover == id + ":dd" || isOpen));
                        using (var p = ArtworkTheme.Para(ctlX, cy, ctlW, ctlH, 8 * s))
                        {
                            using (var b = new SolidBrush(A(White(14 + (int)(10 * fh))))) g.FillPath(b, p);
                            bool editingBox = set.Text && _editingKey == set.Key;
                        using (var pen = new Pen(A(isOpen || editingBox ? ArtworkTheme.Accent : ArtworkTheme.Mix(ArtworkTheme.Line, White(150), fh)), Math.Max(1, s))) g.DrawPath(pen, p);
                        }
                        bool editing = set.Text && _editingKey == set.Key;
                        bool hint = set.Text && string.IsNullOrEmpty(_iniValues.GetValueOrDefault(set.Key, ""));
                        if (!editing)
                        {
                            var valueClip = g.Save();
                            g.SetClip(new RectangleF(ctlX + 14 * s, cy, Math.Max(1, ctlW - (set.Text ? 28 * s : 52 * s)), ctlH), CombineMode.Intersect);
                            ArtworkTheme.Label(g, set.Text ? val : val.ToUpperInvariant(), valueFont, A(!enabled ? ArtworkTheme.Dim : hint ? ArtworkTheme.Muted : ArtworkTheme.Text), ctlX + 20 * s, cy, ctlH, valueSpacing);
                            g.Restore(valueClip);
                        }
                        if (!set.Text)
                        {
                            float chx = ctlX + ctlW - 32 * s, chy = cy + ctlH / 2, ca = 4.5f * s, dir = isOpen ? -1 : 1;
                            using (var pen = new Pen(A(isOpen ? ArtworkTheme.Accent : ArtworkTheme.Mix(ArtworkTheme.Muted, ArtworkTheme.Text, fh)), Math.Max(1.2f, 1.6f * s)))
                                g.DrawLines(pen, new[] { new PointF(chx - ca, chy - ca * 0.5f * dir), new PointF(chx, chy + ca * 0.5f * dir), new PointF(chx + ca, chy - ca * 0.5f * dir) });
                        }
                    }
                    if (enabled && inView)
                    {
                        AddHit(id, x, Math.Max(ry, listTop), cw, Math.Min(ry + rowH, listTop + viewH) - Math.Max(ry, listTop));
                        if (set.Slider) AddHit("vol:" + set.Key, ctlX, cy, ctlW, ctlH);
                        else AddHit(id + ":dd", ctlX, cy, ctlW, ctlH);
                    }
                    HLine(g, x, x + cw, ry + rowH);
                    PopFade(g, st);
                }
            }
            g.Restore(clip);

            if (_settingsMaxScroll > 0)
            {
                float trackX = x + cw + 16 * s;
                float thumbH = Math.Max(28 * s, viewH * viewH / content);
                float thumbY = listTop + (viewH - thumbH) * (_settingsScroll / _settingsMaxScroll);
                _settingsTrackTop = listTop;
                _settingsTrackH = viewH;
                _settingsThumbH = thumbH;
                using (var b = new SolidBrush(White(28))) g.FillRectangle(b, trackX, listTop, 3 * s, viewH);
                using (var b = new SolidBrush(ArtworkTheme.Accent)) g.FillRectangle(b, trackX, thumbY, 3 * s, thumbH);
                AddHit("setscroll", trackX - 10 * s, listTop, 24 * s, viewH);
            }


            if (openIdx >= 0)
            {
                float ry = listTop + (rowTops[openIdx] - _settingsScroll);
                PaintDropdown(g, H, vis[openIdx], Array.IndexOf(_settings, vis[openIdx]), ctlX, ry + (rowH - ctlH) / 2, ctlW, ctlH);
            }
        }

        void PaintVolume(Graphics g, float ctlX, float cy, float ctlW, float ctlH)
        {
            float s = _s;
            int vol = 100;
            int.TryParse(_iniValues.GetValueOrDefault("volume", "100"), out vol);
            vol = Math.Clamp(vol, 0, 100);
            float pad = 16 * s, labelW = 52 * s;
            float trackX = ctlX + pad;
            float trackW = Math.Max(20 * s, ctlW - pad - labelW - 8 * s);
            float trackH = Math.Max(3 * s, 4 * s);
            float mid = cy + ctlH / 2;
            _volTrackX = trackX;
            _volTrackW = trackW;
            _volTrackY = cy;
            _volTrackH = ctlH;
            _volTrackValid = true;
            using (var b = new SolidBrush(A(White(40)))) g.FillRectangle(b, trackX, mid - trackH / 2, trackW, trackH);
            using (var b = new SolidBrush(A(ArtworkTheme.Accent))) g.FillRectangle(b, trackX, mid - trackH / 2, trackW * vol / 100f, trackH);
            float r = 7 * s, tx = trackX + trackW * vol / 100f;
            using (var b = new SolidBrush(A(ArtworkTheme.Text))) g.FillEllipse(b, tx - r, mid - r, r * 2, r * 2);
            ArtworkTheme.Label(g, vol.ToString(), FM(13 * s), A(ArtworkTheme.Text), ctlX + ctlW - labelW - 10 * s, cy, ctlH, 13 * s * 0.04f, StringAlignment.Far, labelW);
        }

        void PaintDropdown(Graphics g, int H, Setting set, int idx, float fx, float fy, float fw, float fh)
        {
            float s = _s;
            var vals = set.Values;
            const int MaxRows = 8;
            int count = Math.Min(vals.Length, MaxRows);
            string current = _iniValues.GetValueOrDefault(set.Key, vals.Length > 0 ? vals[0] : "");
            if (_ddFirst < 0) _ddFirst = Math.Max(0, Array.IndexOf(vals, current) - MaxRows / 2);
            if (_dropdownHoverIndex >= 0 && _dropdownHoverIndex < _ddFirst) _ddFirst = _dropdownHoverIndex;
            if (_dropdownHoverIndex >= _ddFirst + MaxRows) _ddFirst = _dropdownHoverIndex - MaxRows + 1;
            _ddFirst = Math.Clamp(_ddFirst, 0, Math.Max(0, vals.Length - MaxRows));

            float itemH = 34 * s, listH = count * itemH + 8 * s;
            float ly = fy + fh + 6 * s;
            bool up = ly + listH > H - StatusH * s - 8 * s;
            if (up) ly = fy - 6 * s - listH;
            float e = ArtworkTheme.Ease(Clamp01((float)(Now - _ddAt) / 0.25f));

            var st = g.Save();
            float shown = listH * e;
            g.SetClip(new RectangleF(fx - 2 * s, up ? ly + listH - shown : ly, fw + 4 * s, shown + 2 * s), CombineMode.Intersect);
            using (var p = ArtworkTheme.CutCorner(fx, ly, fw, listH, 14 * s))
            {
                using (var b = new SolidBrush(ArtworkTheme.Bg2)) g.FillPath(b, p);
                using (var pen = new Pen(White(60), Math.Max(1, s))) g.DrawPath(pen, p);
            }
            for (int r = 0; r < count; r++)
            {
                int vi = _ddFirst + r;
                float iy = ly + 4 * s + r * itemH;
                bool hot = _dropdownHoverIndex == vi, sel = vals[vi] == current;
                if (hot)
                {
                    using (var b = new SolidBrush(Color.FromArgb(46, ArtworkTheme.Accent))) g.FillRectangle(b, fx + 1, iy, fw - 2, itemH);
                    using (var b = new SolidBrush(ArtworkTheme.Accent)) g.FillRectangle(b, fx + 1, iy, 3 * s, itemH);
                }
                string opt = set.Labels != null && vi < set.Labels.Length ? set.Labels[vi] : vals[vi];
                var optClip = g.Save();
                g.SetClip(new RectangleF(fx + 14 * s, iy, Math.Max(1, fw - 48 * s), itemH), CombineMode.Intersect);
                ArtworkTheme.Label(g, opt.ToUpperInvariant(), FM(13 * s), hot || sel ? ArtworkTheme.Text : ArtworkTheme.Muted, fx + 20 * s, iy, itemH, 13 * s * 0.06f);
                g.Restore(optClip);
                if (sel)
                    using (var b = new SolidBrush(ArtworkTheme.Accent))
                        g.FillPolygon(b, new[] { new PointF(fx + fw - 28 * s, iy + itemH / 2 - 4 * s), new PointF(fx + fw - 18 * s, iy + itemH / 2 - 4 * s), new PointF(fx + fw - 21 * s, iy + itemH / 2 + 4 * s), new PointF(fx + fw - 31 * s, iy + itemH / 2 + 4 * s) });
                AddHit($"set:{idx}:opt:{vi}", fx, iy, fw, itemH);
            }
            if (vals.Length > MaxRows)
            {
                float trackH = count * itemH, thumbH = trackH * MaxRows / vals.Length;
                float thumbY = ly + 4 * s + (trackH - thumbH) * _ddFirst / (vals.Length - MaxRows);
                using var b = new SolidBrush(ArtworkTheme.Accent);
                g.FillRectangle(b, fx + fw - 5 * s, thumbY, 2 * s, thumbH);
            }
            g.Restore(st);
        }

        void PaintStorageSettings(Graphics g, int W, int H, float x, float cw, float listTop, float viewH)
        {
            float s = _s;
            var st = PushFade(g, Up((float)(Now - _viewAt), 0.2f), 16 * s);
            float cwIn = Math.Min(cw, 720 * s);
            float ch = Math.Min(viewH, 420 * s);
            float pad = 30 * s;
            using (var p = ArtworkTheme.CutCorners(x, listTop, cwIn, ch, 18 * s))
            {
                using (var b = new SolidBrush(A(White(10)))) g.FillPath(b, p);
                using (var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
            }
            Font hf = FM(12 * s);
            float hsp = 12 * s * 0.16f;
            ArtworkTheme.Label(g, "STORAGE", hf, A(ArtworkTheme.Accent), x + pad, listTop, 52 * s, hsp);
            float rule = x + pad + ArtworkTheme.TextWidth(g, "STORAGE", hf, hsp) + 14 * s;
            if (rule < x + cwIn - pad) HLine(g, rule, x + cwIn - pad, listTop + 26 * s);
            string text = _storageText.Length > 0 ? _storageText : "The game's files (dump) and the compiled game (build) take tens of GB. They can live on another drive: the move leaves a link in their place.";
            ArtworkTheme.DrawWrapped(g, text, FB(14 * s), A(ArtworkTheme.Muted), x + pad, listTop + 50 * s, cwIn - pad * 2, ch - 140 * s);
            float bh = 44 * s, by = listTop + ch - bh - 28 * s, wMove = 260 * s, wRef = 150 * s;
            float xMove = x + cwIn - pad - wMove, xRef = xMove - 12 * s - wRef;
            bool can = !_storageBusy;
            ArtworkTheme.ButtonAnimated(g, "REFRESH", xRef, by, wRef, bh, s, false, Hov("storage:refresh", can && _hover == "storage:refresh"), can, 12 * s, _alpha, press: Press("storage:refresh"));
            if (can) AddHit("storage:refresh", xRef, by, wRef, bh);
            ArtworkTheme.ButtonAnimated(g, _storageBusy ? "WORKING…" : "MOVE TO ANOTHER DRIVE", xMove, by, wMove, bh, s, true, Hov("storage:move", can && _hover == "storage:move"), can, 13 * s, _alpha, press: Press("storage:move"));
            if (can) AddHit("storage:move", xMove, by, wMove, bh);
            PopFade(g, st);
        }

        void PaintUpdateSettings(Graphics g, int W, int H, float x, float cw, float listTop, float viewH)
        {
            float s = _s;
            var st = PushFade(g, Up((float)(Now - _viewAt), 0.2f), 16 * s);
            float cwIn = Math.Min(cw, 720 * s);
            float ch = Math.Min(viewH, 420 * s);
            float pad = 30 * s;
            using (var p = ArtworkTheme.CutCorners(x, listTop, cwIn, ch, 18 * s))
            {
                using (var b = new SolidBrush(A(White(10)))) g.FillPath(b, p);
                using (var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
            }
            Font hf = FM(12 * s);
            float hsp = 12 * s * 0.16f;
            ArtworkTheme.Label(g, "UPDATE", hf, A(ArtworkTheme.Accent), x + pad, listTop, 52 * s, hsp);
            float rule = x + pad + ArtworkTheme.TextWidth(g, "UPDATE", hf, hsp) + 14 * s;
            if (rule < x + cwIn - pad) HLine(g, rule, x + cwIn - pad, listTop + 26 * s);
            string status; Color color;
            switch (_updateState)
            {
                case "checking": status = "CHECKING GITHUB\u2026"; color = ArtworkTheme.Muted; break;
                case "updating": status = "UPDATING OPENSMASH\u2026"; color = ArtworkTheme.Accent; break;
                case "available": status = _updateBehind == 1 ? "1 NEW COMMIT" : $"{_updateBehind} NEW COMMITS"; color = ArtworkTheme.Accent; break;
                case "uptodate": status = "UP TO DATE"; color = ArtworkTheme.Text; break;
                case "error": status = "COULD NOT CHECK"; color = ArtworkTheme.Muted; break;
                default: status = "NOT CHECKED YET"; color = ArtworkTheme.Dim; break;
            }
            ArtworkTheme.Label(g, status, FD(34 * s), A(color), x + pad, listTop + 38 * s, 34 * s, 0.34f * s);
            float ty = listTop + 90 * s;
            string message = string.IsNullOrEmpty(_updateMessage) ? "Check GitHub for a newer version of OpenSmash. Play is paused while an update installs." : _updateMessage;
            ArtworkTheme.DrawWrapped(g, message, FB(15 * s), A(ArtworkTheme.Muted), x + pad, ty, cwIn - pad * 2, 64 * s);
            if (!string.IsNullOrEmpty(_updateDetail)) ArtworkTheme.DrawWrapped(g, _updateDetail, FB(13 * s), A(ArtworkTheme.Dim), x + pad, ty + 70 * s, cwIn - pad * 2, 54 * s);
            if (_updateLocal.Length > 0 || _updateHead.Length > 0)
            {
                Font vf = FM(12 * s);
                string versions = $"THIS COPY  {(_updateLocal.Length > 0 ? ShortCommit(_updateLocal) : "?")}   \u00B7   GITHUB  {(_updateHead.Length > 0 ? ShortCommit(_updateHead) : "?")}";
                ArtworkTheme.Label(g, versions, vf, A(ArtworkTheme.Dim), x + pad, listTop + ch - 88 * s, 16 * s, 12 * s * 0.08f);
            }
            float bh = 44 * s, by = listTop + ch - bh - 28 * s;
            float wApply = 150 * s, wCheck = 210 * s;
            bool showApply = _updateState == "available" || _updateApplying;
            float xApply = x + cwIn - pad - wApply;
            float xCheck = showApply ? xApply - 12 * s - wCheck : x + (cwIn - wCheck) / 2;
            bool canCheck = !_updateChecking && !_updateApplying;
            ArtworkTheme.ButtonAnimated(g, _updateState == "checking" ? "CHECKING\u2026" : "CHECK FOR UPDATE", xCheck, by, wCheck, bh, s, false, Hov("update:check", canCheck && _hover == "update:check"), canCheck, 12 * s, _alpha, press: Press("update:check"));
            if (canCheck) AddHit("update:check", xCheck, by, wCheck, bh);
            bool canApply = _updateState == "available" && !_updateApplying && !_updateChecking;
            if (showApply)
            {
                ArtworkTheme.ButtonAnimated(g, _updateApplying ? "UPDATING\u2026" : "UPDATE", xApply, by, wApply, bh, s, true, Hov("update:apply", canApply && _hover == "update:apply"), canApply, 13 * s, _alpha, press: Press("update:apply"));
                if (canApply) AddHit("update:apply", xApply, by, wApply, bh);
            }
            PopFade(g, st);
        }

        void PaintMods(Graphics g, int W, int H)
        {
            float s = _s, x = Gutter * s, cw = W - 2 * x;
            PaintSectionHead(g, "02 / MODS", "MODS");

            float cardY = ContentTop * s, cardH = 128 * s, gap = 16 * s;
            int cards = ModActionCount;
            float cardW = (cw - gap * (cards - 1)) / cards;
            PaintCard(g, "act:browse", x, cardY, cardW, cardH, 0, _keys && _sel == 0,
                "GAMEBANANA", ArtworkTheme.Text, ArtworkTheme.Bg, "BROWSE MODS", "Find and install mods from GameBanana.");
            PaintCard(g, "act:setup", x + (cardW + gap), cardY, cardW, cardH, 1, _keys && _sel == 1,
                "PLUGINS", ArtworkTheme.Text, ArtworkTheme.Bg, "PLUGINS", "Community plugins movesets and new characters are built on.");
            // (TRLLZR's mod manager ideas, NOTICE.md: conflicts, updates, shared lists - tools/os_mods.py)
            PaintCard(g, "act:check", x + 2 * (cardW + gap), cardY, cardW, cardH, 2, _keys && _sel == 2,
                "CHECK", ArtworkTheme.Text, ArtworkTheme.Bg, "CONFLICTS & UPDATES", "Files two mods both change, and newer versions on GameBanana.");
            PaintCard(g, "act:share", x + 3 * (cardW + gap), cardY, cardW, cardH, 3, _keys && _sel == 3,
                "SHARE", ArtworkTheme.Text, ArtworkTheme.Bg, "MOD LIST", "Save your mods as a list, or install a friend's list.");

            // List. _modScroll is a pixel offset in unscaled units; rows grow when they carry a description.
            float top = ModListTop;
            float view = ModListView;
            float content = 0;
            foreach (var line in _modLines) content += line.RowH;
            int maxScroll = (int)Math.Max(0, content - view);
            if (_modScroll > maxScroll) _modScroll = maxScroll;
            float sizeR = x + cw - 128 * s, typeX = x + cw - 96 * s;
            {
                var st = PushReveal(g, 2);
                var hf = FM(11 * s);
                float sp = 11 * s * 0.16f;
                int packages = _mods.Select(m => string.IsNullOrEmpty(m.Group) ? "m:" + m.Name : "g:" + m.Group).Distinct().Count();
                ArtworkTheme.Label(g, $"INSTALLED  ({packages})", hf, A(ArtworkTheme.Dim), x, top - 26 * s, 20 * s, sp);
                ArtworkTheme.Label(g, "FILES", hf, A(ArtworkTheme.Dim), 0, top - 26 * s, 20 * s, sp, StringAlignment.Far, sizeR);
                ArtworkTheme.Label(g, "ON", hf, A(ArtworkTheme.Dim), typeX, top - 26 * s, 20 * s, sp);
                HLine(g, x, x + cw, top);
                PopFade(g, st);
            }

            if (_mods.Count == 0 && _modsChecked)
            {
                var st = PushReveal(g, 3);
                var path = TextPath("NO MODS YET.", 46 * s);
                var b = path.GetBounds();
                var ps = g.Save();
                g.TranslateTransform(x, top + 34 * s - b.Top);
                using (var pen = new Pen(A(ArtworkTheme.Dim), Math.Max(1, 1.5f * s))) g.DrawPath(pen, path);
                g.Restore(ps);
                ArtworkTheme.Label(g, "Browse GameBanana above. A download is installed and turned on by itself.", FB(15 * s), A(ArtworkTheme.Muted), x, top + 34 * s + b.Height + 14 * s, 22 * s);
                PopFade(g, st);
            }

            float yAcc = 0;
            int drawn = 0;
            float listBottom = top + view * s;
            int openPick = -1;
            float openPickX = 0, openPickY = 0, openPickW = 0, openPickH = 0;
            var listClip = g.Save();
            g.SetClip(new RectangleF(0, top, W, Math.Max(1, listBottom - top)), CombineMode.Intersect);
            for (int i = 0; i < _modLines.Count; i++)
            {
                var m = _modLines[i];
                float rowUnscaled = m.RowH;
                float topUnscaled = yAcc - _modScroll;
                yAcc += rowUnscaled;
                if (topUnscaled + rowUnscaled < 0) continue;
                if (topUnscaled > view) break;
                int rowIndex = ModActionCount + i;
                string id = $"modrow:{rowIndex}";
                float ry = top + topUnscaled * s;
                float rowH = rowUnscaled * s;
                bool sel = rowIndex == _sel;
                var st = PushReveal(g, 3 + drawn);
                drawn++;
                float e = ArtworkTheme.Ease(Math.Max(Hov(id, _hover == id), sel ? 1 : 0));
                RowWash(g, x, ry, cw, rowH, e, sel ? 0.2f : 0.14f);
                if (sel) using (var b = new SolidBrush(A(ArtworkTheme.Accent))) g.FillRectangle(b, x, ry, 3 * s, rowH);
                float shift = 10 * s * e;
                float indent = m.Child ? 36 * s : 0;
                float thumb = m.Child ? 0 : 46 * s;
                float tx = x + 14 * s + indent + shift;
                if (m.Header)
                {
                    float cy = ry + rowH / 2, ca = 5 * s;
                    using var pen = new Pen(A(ArtworkTheme.Muted), Math.Max(1.4f, 1.6f * s));
                    if (m.Open)
                        g.DrawLines(pen, new[] { new PointF(tx, cy - ca * 0.4f), new PointF(tx + ca, cy + ca * 0.45f), new PointF(tx + ca * 2, cy - ca * 0.4f) });
                    else
                        g.DrawLines(pen, new[] { new PointF(tx, cy - ca), new PointF(tx + ca * 0.8f, cy), new PointF(tx, cy + ca) });
                    tx += 22 * s;
                }
                if (!m.Child)
                {
                    float dw = thumb, dh = thumb;
                    if (!string.IsNullOrEmpty(m.Thumb) && _modThumbs.TryGetValue(m.Thumb, out var pic) && pic.Width > 0 && pic.Height > 0)
                    {
                        float maxW = 96 * s;
                        dh = thumb;
                        dw = dh * pic.Width / (float)pic.Height;
                        if (dw > maxW) { dw = maxW; dh = dw * pic.Height / (float)pic.Width; }
                        g.DrawImage(pic, tx, ry + (rowH - dh) / 2, dw, dh);
                    }
                    else
                    {
                        using var b = new SolidBrush(A(ArtworkTheme.Bg2));
                        g.FillRectangle(b, tx, ry + (rowH - thumb) / 2, thumb, thumb);
                    }
                    tx += dw + 14 * s;
                }
                // Pick rows: … FILES | COLOR ▾ | ON — dropdown sits just left of the toggle.
                float pickW = m.Pick ? 120 * s : 0;
                float pickGap = m.Pick ? 14 * s : 0;
                float filesRight = m.Pick ? typeX - pickW - pickGap - 8 * s : sizeR;
                bool detailed = m.HasChips || !string.IsNullOrEmpty(m.About) || !string.IsNullOrEmpty(m.Meta) || !string.IsNullOrEmpty(m.Sub);
                float textW = Math.Max(40 * s, filesRight - tx - 108 * s);
                if (!detailed)
                    ArtworkTheme.DrawText(g, m.Title, FB(15 * s, sel), A(m.Enabled || m.Mixed ? ArtworkTheme.Text : ArtworkTheme.Dim), tx, ry, textW, rowH);
                else
                {
                    float ty = ry + 8 * s;
                    ArtworkTheme.DrawText(g, m.Title, FB(15 * s, sel), A(m.Enabled || m.Mixed ? ArtworkTheme.Text : ArtworkTheme.Dim), tx, ty, textW, 22 * s, StringAlignment.Near, StringAlignment.Near);
                    ty += 24 * s;
                    if (!string.IsNullOrEmpty(m.Sub))
                    {
                        ArtworkTheme.Label(g, m.Sub, FM(12 * s), A(ArtworkTheme.Dim), tx, ty, 16 * s);
                        ty += 18 * s;
                    }
                    if (!string.IsNullOrEmpty(m.Meta) || m.HasChips)
                    {
                        float bx = tx;
                        float limit = tx + textW;
                        if (!string.IsNullOrEmpty(m.Meta))
                        {
                            ArtworkTheme.Label(g, m.Meta, FM(11 * s), A(ArtworkTheme.Dim), bx, ty, 20 * s, 0.08f * s);
                            bx += ArtworkTheme.TextWidth(g, m.Meta, FM(11 * s), 0.08f * s) + 10 * s;
                        }
                        void Chip(string text, Color bg, Color fg, bool outline = false)
                        {
                            if (bx >= limit - 40 * s) return;
                            bx += Badge(g, text, bx, ty, bg, fg, outline) + 6 * s;
                        }
                        var ink = Color.FromArgb(36, 255, 255, 255);
                        if (m.Online == "unsafe") Chip("NOT ONLINE SAFE", Color.FromArgb(56, 255, 59, 48), ArtworkTheme.Bad);
                        else if (m.Online == "same") Chip("EVERYONE NEEDS IT", Color.FromArgb(48, 255, 194, 26), ArtworkTheme.Warn);
                        else if (m.Online == "caution") Chip("CHECK BEFORE ONLINE", Color.FromArgb(48, 255, 194, 26), ArtworkTheme.Warn);
                        else if (m.Online == "safe") Chip("ONLINE SAFE", Color.FromArgb(40, 61, 255, 138), ArtworkTheme.Ready);
                        if (m.NewSlots) Chip("NEW COSTUME SLOTS", Color.FromArgb(40, 59, 224, 255), ArtworkTheme.Info);
                        if (m.Code == "waiting") Chip("PLUGIN NOT BUILT", Color.FromArgb(56, 255, 59, 48), ArtworkTheme.Bad);
                        else if (m.Code == "built") Chip("PLUGIN BUILT IN", Color.FromArgb(40, 61, 255, 138), ArtworkTheme.Ready);
                        if (m.Added > 0) Chip(m.Added + " NEW FILES", ink, ArtworkTheme.Muted, true);
                        if (m.Merged > 0) Chip(m.Merged + " PATCHES", ink, ArtworkTheme.Muted, true);
                        if (m.KeptShared > 0) Chip(m.KeptShared + " ON ALL COSTUMES", ink, ArtworkTheme.Muted, true);
                        if (m.Skipped > 0) Chip(m.Skipped + " NOT LOADED", Color.FromArgb(56, 255, 59, 48), ArtworkTheme.Bad);
                        ty += 26 * s;
                    }
                    if (!string.IsNullOrEmpty(m.About))
                        ArtworkTheme.DrawWrapped(g, m.About, FB(12 * s), A(ArtworkTheme.Muted), tx, ty, textW, 42 * s);
                }
                if (!m.Header)
                    ArtworkTheme.Label(g, (m.Files + " FILES").ToUpperInvariant(), FM(12 * s), A(ArtworkTheme.Dim), 0, ry, rowH, 0, StringAlignment.Far, filesRight);
                string toggle = m.Mixed ? "MIX" : m.Enabled ? "ON" : "OFF";
                float tw = 64 * s, txx = typeX, tyy = ry + (rowH - 24 * s) / 2;
                if (m.Pick && m.Variants.Length > 0)
                {
                    float ddH = 28 * s, ddX = typeX - pickW - 8 * s, ddY = ry + (rowH - ddH) / 2;
                    string pickKey = "modpick:" + m.GroupKey;
                    bool isOpen = _openDropdownKey == pickKey;
                    string ddId = "modpick:dd:" + i;
                    float fh = ArtworkTheme.Ease(Hov(ddId, _hover == ddId || isOpen));
                    using (var p = ArtworkTheme.Para(ddX, ddY, pickW, ddH, 6 * s))
                    {
                        using (var b = new SolidBrush(A(White(14 + (int)(10 * fh))))) g.FillPath(b, p);
                        using (var pen = new Pen(A(isOpen ? ArtworkTheme.Accent : ArtworkTheme.Mix(ArtworkTheme.Line, White(150), fh)), Math.Max(1, s)))
                            g.DrawPath(pen, p);
                    }
                    int ai = Math.Clamp(m.Active, 0, m.Variants.Length - 1);
                    string label = (m.Variants[ai] ?? "").ToUpperInvariant();
                    var ddClip = g.Save();
                    g.SetClip(new RectangleF(ddX + 10 * s, ddY, Math.Max(1, pickW - 36 * s), ddH), CombineMode.Intersect);
                    ArtworkTheme.Label(g, label, FM(11 * s, true), A(m.Enabled ? ArtworkTheme.Text : ArtworkTheme.Muted), ddX + 12 * s, ddY, ddH, 0);
                    g.Restore(ddClip);
                    float chx = ddX + pickW - 18 * s, chy = ddY + ddH / 2, ca = 4 * s, dir = isOpen ? -1 : 1;
                    using (var pen = new Pen(A(isOpen ? ArtworkTheme.Accent : ArtworkTheme.Mix(ArtworkTheme.Muted, ArtworkTheme.Text, fh)), Math.Max(1.2f, 1.6f * s)))
                        g.DrawLines(pen, new[] { new PointF(chx - ca, chy - ca * 0.5f * dir), new PointF(chx, chy + ca * 0.5f * dir), new PointF(chx + ca, chy - ca * 0.5f * dir) });
                    float ddHitTop = Math.Max(ddY, top), ddHitH = Math.Min(ddY + ddH, listBottom) - ddHitTop;
                    if (ddHitH > 0) AddHit(ddId, ddX, ddHitTop, pickW, ddHitH);
                    if (isOpen) { openPick = i; openPickX = ddX; openPickY = ddY; openPickW = pickW; openPickH = ddH; }
                }
                using (var pth = ArtworkTheme.Para(txx, tyy, tw, 24 * s, 6 * s))
                {
                    if (m.Enabled) using (var b = new SolidBrush(A(ArtworkTheme.Text))) g.FillPath(b, pth);
                    using (var pen = new Pen(A(m.Enabled ? ArtworkTheme.Text : ArtworkTheme.Line), Math.Max(1, s))) g.DrawPath(pen, pth);
                }
                ArtworkTheme.Label(g, toggle, FM(11 * s, true), A(m.Enabled ? ArtworkTheme.Bg : ArtworkTheme.Muted), txx, tyy, 24 * s, 0, StringAlignment.Center, tw);
                HLine(g, x, x + cw, ry + rowH);
                float hitTop = Math.Max(ry, top), hitH = Math.Min(ry + rowH, listBottom) - hitTop;
                if (hitH > 0)
                {
                    float hitRight = m.Pick ? filesRight - 8 * s : typeX - 8 * s;
                    AddHit(id, x, hitTop, Math.Max(8 * s, hitRight - x), hitH);
                    if (m.Header) AddHit("modopen:" + i, x, hitTop, typeX - x - 8 * s, hitH);
                    float toggleTop = Math.Max(tyy, top), toggleH = Math.Min(tyy + 24 * s, listBottom) - toggleTop;
                    if (toggleH > 0) AddHit("modtoggle:" + i, txx, toggleTop, tw + 8 * s, toggleH);
                }
                PopFade(g, st);
            }
            g.Restore(listClip);

            if (openPick >= 0 && openPick < _modLines.Count)
                PaintModPickDropdown(g, H, _modLines[openPick], openPickX, openPickY, openPickW, openPickH);

            if (maxScroll > 0)
            {
                float trackX = x + cw + 16 * s;
                float trackH = view * s;
                float thumbH = Math.Max(28 * s, trackH * trackH / Math.Max(trackH, content * s));
                float thumbY = top + (trackH - thumbH) * _modScroll / maxScroll;
                _modTrackTop = top;
                _modTrackH = trackH;
                _modThumbH = thumbH;
                _modMaxScroll = maxScroll;
                using (var b = new SolidBrush(White(28))) g.FillRectangle(b, trackX, top, 3 * s, trackH);
                using (var b = new SolidBrush(ArtworkTheme.Accent)) g.FillRectangle(b, trackX, thumbY, 3 * s, thumbH);
                AddHit("modscroll", trackX - 10 * s, top, 24 * s, trackH);
            }
            else
                _modMaxScroll = 0;

            // Actions
            {
                var st = PushReveal(g, 5);
                float bx = x, by = H - 104 * s, bh = 38 * s;
                var f = FB(12 * s, true);
                bool busy = Busy;
                foreach (var (key, label) in new[] { ("open", "OPEN FOLDER"), ("add", "ADD MOD"), ("remove", "REMOVE"), ("refresh", "REFRESH"), ("rebuild", "REBUILD PLUGINS") })
                {
                    string id = $"act:{key}";
                    bool enabled = key == "open" || !busy;
                    if (key == "remove" && _sel < ModActionCount) enabled = false;
                    float bw = ArtworkTheme.TextWidth(g, label, f, 1.2f * s) + 48 * s;
                    ArtworkTheme.ButtonAnimated(g, label, bx, by, bw, bh, s, primary: false, Hov(id, enabled && _hover == id), enabled, 12 * s, _alpha, press: Press(id));
                    if (enabled) AddHit(id, bx, by, bw, bh);
                    bx += bw + 10 * s;
                }
                PopFade(g, st);
            }
        }

        void PaintModPickDropdown(Graphics g, int H, ModLine line, float fx, float fy, float fw, float fh)
        {
            float s = _s;
            var vals = line.Variants;
            if (vals == null || vals.Length == 0) return;
            const int MaxRows = 8;
            int count = Math.Min(vals.Length, MaxRows);
            int current = Math.Clamp(line.Active, 0, vals.Length - 1);
            if (_ddFirst < 0) _ddFirst = Math.Max(0, current - MaxRows / 2);
            if (_dropdownHoverIndex >= 0 && _dropdownHoverIndex < _ddFirst) _ddFirst = _dropdownHoverIndex;
            if (_dropdownHoverIndex >= _ddFirst + MaxRows) _ddFirst = _dropdownHoverIndex - MaxRows + 1;
            _ddFirst = Math.Clamp(_ddFirst, 0, Math.Max(0, vals.Length - MaxRows));

            float itemH = 34 * s, listH = count * itemH + 8 * s;
            float ly = fy + fh + 6 * s;
            bool up = ly + listH > H - StatusH * s - 8 * s;
            if (up) ly = fy - 6 * s - listH;
            float e = ArtworkTheme.Ease(Clamp01((float)(Now - _ddAt) / 0.25f));

            var st = g.Save();
            float shown = listH * e;
            g.SetClip(new RectangleF(fx - 2 * s, up ? ly + listH - shown : ly, fw + 4 * s, shown + 2 * s), CombineMode.Intersect);
            using (var p = ArtworkTheme.CutCorner(fx, ly, fw, listH, 14 * s))
            {
                using (var b = new SolidBrush(ArtworkTheme.Bg2)) g.FillPath(b, p);
                using (var pen = new Pen(White(60), Math.Max(1, s))) g.DrawPath(pen, p);
            }
            for (int r = 0; r < count; r++)
            {
                int vi = _ddFirst + r;
                float iy = ly + 4 * s + r * itemH;
                bool hot = _dropdownHoverIndex == vi, sel = vi == current;
                if (hot)
                {
                    using (var b = new SolidBrush(Color.FromArgb(46, ArtworkTheme.Accent))) g.FillRectangle(b, fx + 1, iy, fw - 2, itemH);
                    using (var b = new SolidBrush(ArtworkTheme.Accent)) g.FillRectangle(b, fx + 1, iy, 3 * s, itemH);
                }
                string opt = (vals[vi] ?? "").ToUpperInvariant();
                var optClip = g.Save();
                g.SetClip(new RectangleF(fx + 14 * s, iy, Math.Max(1, fw - 48 * s), itemH), CombineMode.Intersect);
                ArtworkTheme.Label(g, opt, FM(13 * s), hot || sel ? ArtworkTheme.Text : ArtworkTheme.Muted, fx + 20 * s, iy, itemH, 13 * s * 0.06f);
                g.Restore(optClip);
                if (sel)
                    using (var b = new SolidBrush(ArtworkTheme.Accent))
                        g.FillPolygon(b, new[] { new PointF(fx + fw - 28 * s, iy + itemH / 2 - 4 * s), new PointF(fx + fw - 18 * s, iy + itemH / 2 - 4 * s), new PointF(fx + fw - 21 * s, iy + itemH / 2 + 4 * s), new PointF(fx + fw - 31 * s, iy + itemH / 2 + 4 * s) });
                AddHit($"modpick:opt:{vi}", fx, iy, fw, itemH);
            }
            if (vals.Length > MaxRows)
            {
                float trackH = count * itemH, thumbH = trackH * MaxRows / vals.Length;
                float thumbY = ly + 4 * s + (trackH - thumbH) * _ddFirst / (vals.Length - MaxRows);
                using var b = new SolidBrush(ArtworkTheme.Accent);
                g.FillRectangle(b, fx + fw - 5 * s, thumbY, 2 * s, thumbH);
            }
            g.Restore(st);
        }

        void PaintSetupModding(Graphics g, int W, int H)
        {
            float s = _s, x = Gutter * s, cw = W - 2 * x;
            PaintSectionHead(g, "02 / MODS", "PLUGINS");

            {
                var st = PushReveal(g, 0);
                ArtworkTheme.DrawWrapped(g, "The community plugins movesets and new characters are built on. Downloading them here is the same list WindowSmash Mods keeps under Settings.",
                    FB(16 * s), A(ArtworkTheme.Muted), x, ContentTop * s - 4 * s, cw, 52 * s);
                PopFade(g, st);
            }

            var items = _stdPlugins.Where(p => p.Status == "runs").ToArray();
            float listY = ContentTop * s + 62 * s, rowH = 40 * s;
            {
                var st = PushReveal(g, 1);
                HLine(g, x, x + cw, listY);
                for (int i = 0; i < items.Length; i++)
                {
                    float ry = listY + i * rowH;
                    string label = items[i].Name.Replace(".nro", "") + "   " + items[i].Version;
                    bool custom = items[i].Source == "custom";
                    if (custom) Badge(g, "CUSTOM", x, ry + (rowH - 20 * s) / 2, Color.FromArgb(255, 34, 197, 94), ArtworkTheme.Bg);
                    else if (items[i].Fetched) Badge(g, "FETCHED", x, ry + (rowH - 20 * s) / 2, ArtworkTheme.Text, ArtworkTheme.Bg);
                    else Badge(g, "MISSING", x, ry + (rowH - 20 * s) / 2, Color.Empty, ArtworkTheme.Muted, outline: true);
                    ArtworkTheme.Label(g, label, FB(15 * s), A(items[i].Fetched || custom ? ArtworkTheme.Text : ArtworkTheme.Muted), x + 104 * s, ry, rowH);
                    HLine(g, x, x + cw, ry + rowH);
                }
                PopFade(g, st);
            }

            float barY = listY + items.Length * rowH + 34 * s;
            {
                var st = PushReveal(g, 2);
                var f = FM(12 * s);
                ArtworkTheme.Label(g, string.IsNullOrEmpty(_setupStep) ? "PROGRESS" : _setupStep.ToUpperInvariant(), f, A(ArtworkTheme.Muted), x, barY - 24 * s, 18 * s, 1.4f * s);
                ArtworkTheme.Label(g, $"{_setupProgress}%", FM(12 * s, true), A(ArtworkTheme.Text), 0, barY - 24 * s, 18 * s, 1.4f * s, StringAlignment.Far, x + cw);
                DrawBar(g, x, barY, cw, 8 * s, _setupProgress / 100f, ArtworkTheme.Accent);
                PopFade(g, st);
            }

            float logY = barY + 26 * s, logH = H - 124 * s - logY;
            if (logH > 30 * s)
            {
                var st = PushReveal(g, 3);
                using (var b = new SolidBrush(A(ArtworkTheme.Bg2))) g.FillRectangle(b, x, logY, cw, logH);
                using (var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, s))) g.DrawRectangle(pen, x, logY, cw, logH);
                float lh = 16 * s;
                var lines = _setupLog.Split('\n').Where(l => !string.IsNullOrWhiteSpace(l)).ToArray();
                int n = Math.Max(0, (int)((logH - 20 * s) / lh));
                var shown = lines.Skip(Math.Max(0, lines.Length - n)).ToArray();
                if (shown.Length == 0)
                    ArtworkTheme.Label(g, "THE SETUP LOG APPEARS HERE.", FM(11.5f * s), A(ArtworkTheme.Dim), x + 14 * s, logY + 10 * s, lh, 1.2f * s);
                for (int i = 0; i < shown.Length; i++)
                    ArtworkTheme.DrawText(g, shown[i], FM(11.5f * s), A(ArtworkTheme.Dim), x + 14 * s, logY + 10 * s + i * lh, cw - 28 * s, lh);
                PopFade(g, st);
            }

            {
                var st = PushReveal(g, 4);
                float bx = x, by = H - 104 * s, bh = 46 * s;
                var f = FB(13 * s, true);
                string a = "DOWNLOAD MISSING", b = "DOWNLOAD AGAIN", cbtn = "ADD PLUGINS";
                float wa = ArtworkTheme.TextWidth(g, a, f, 1.3f * s) + 52 * s;
                float wb = ArtworkTheme.TextWidth(g, b, f, 1.3f * s) + 52 * s;
                float wc = ArtworkTheme.TextWidth(g, cbtn, f, 1.3f * s) + 52 * s;
                bool busy = _setupModding;
                ArtworkTheme.ButtonAnimated(g, a, bx, by, wa, bh, s, true, Hov("setup_start", _hover == "setup_start"), !busy, 13 * s, _alpha, press: Press("setup_start"));
                if (!busy) AddHit("setup_start", bx, by, wa, bh);
                ArtworkTheme.ButtonAnimated(g, b, bx + wa + 12 * s, by, wb, bh, s, false, Hov("setup_again", _hover == "setup_again"), !busy, 13 * s, _alpha, press: Press("setup_again"));
                if (!busy) AddHit("setup_again", bx + wa + 12 * s, by, wb, bh);
                float xc = bx + wa + 12 * s + wb + 12 * s;
                ArtworkTheme.ButtonAnimated(g, cbtn, xc, by, wc, bh, s, false, Hov("setup_add", _hover == "setup_add"), !busy, 13 * s, _alpha, press: Press("setup_add"));
                if (!busy) AddHit("setup_add", xc, by, wc, bh);
                PopFade(g, st);
            }
        }

        void PaintSharePrompt(Graphics g, int W, int H)
        {
            float s = _s, e = ArtworkTheme.Ease(Clamp01((float)(Now - _sharePromptAt) / 0.4f));
            using (var b = new SolidBrush(Color.FromArgb((int)(200 * e), 5, 5, 5))) g.FillRectangle(b, 0, 0, W, H);
            AddHit("share_prompt:block", 0, 0, W, H);
            var st = PushFade(g, e, 24 * s);
            float mw = 700 * s, mh = 320 * s, mx = (W - mw) / 2, my = (H - mh) / 2;
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
            ArtworkTheme.Label(g, "MOD LIST", FD(32 * s), A(ArtworkTheme.Text), mx + pad, my + 34 * s, 30 * s, 0.32f * s);
            ArtworkTheme.DrawWrapped(g, "Share your installed mods as a list file a friend can install, or install the mods from a list a friend sent you.",
                FB(15 * s), A(ArtworkTheme.Muted), mx + pad, my + 84 * s, mw - pad * 2, 60 * s);
            float bh = 46 * s, by = my + mh - bh - 28 * s, gap = 12 * s;
            float wInstall = 210 * s, wShare = 170 * s, wCancel = 120 * s;
            float xInstall = mx + mw - pad - wInstall;
            float xShare = xInstall - gap - wShare;
            float xCancel = xShare - gap - wCancel;
            ArtworkTheme.ButtonAnimated(g, "CANCEL", xCancel, by, wCancel, bh, s, false, Hov("share_prompt:cancel", _hover == "share_prompt:cancel"), true, 13 * s, _alpha, press: Press("share_prompt:cancel"));
            AddHit("share_prompt:cancel", xCancel, by, wCancel, bh);
            ArtworkTheme.ButtonAnimated(g, "SHARE MODS", xShare, by, wShare, bh, s, false, Hov("share_prompt:share", _hover == "share_prompt:share"), true, 13 * s, _alpha, press: Press("share_prompt:share"));
            AddHit("share_prompt:share", xShare, by, wShare, bh);
            ArtworkTheme.ButtonAnimated(g, "INSTALL SHARED MODS", xInstall, by, wInstall, bh, s, true, Hov("share_prompt:install", _hover == "share_prompt:install"), true, 12 * s, _alpha, press: Press("share_prompt:install"));
            AddHit("share_prompt:install", xInstall, by, wInstall, bh);
            PopFade(g, st);
        }

        void PaintCheckModsPrompt(Graphics g, int W, int H)
        {
            float s = _s, e = ArtworkTheme.Ease(Clamp01((float)(Now - _checkModsPromptAt) / 0.4f));
            using (var b = new SolidBrush(Color.FromArgb((int)(200 * e), 5, 5, 5))) g.FillRectangle(b, 0, 0, W, H);
            AddHit("check_prompt:block", 0, 0, W, H);
            var st = PushFade(g, e, 24 * s);
            float mw = 720 * s, mh = 440 * s, mx = (W - mw) / 2, my = (H - mh) / 2;
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
            ArtworkTheme.Label(g, "CHECK MODS", FD(32 * s), A(ArtworkTheme.Text), mx + pad, my + 34 * s, 30 * s, 0.32f * s);
            ArtworkTheme.DrawWrapped(g, _checkModsText, FB(13.5f * s), A(ArtworkTheme.Muted), mx + pad, my + 80 * s, mw - pad * 2, mh - 180 * s);
            float bh = 46 * s, by = my + mh - bh - 24 * s, gap = 12 * s;
            bool hasUpdates = _checkModsUpdates.Count > 0;
            float wClose = 130 * s, wInstall = 190 * s;
            float xClose, xInstall = 0;
            if (hasUpdates)
            {
                xInstall = mx + mw - pad - wInstall;
                xClose = xInstall - gap - wClose;
            }
            else
            {
                xClose = mx + (mw - wClose) / 2;
            }
            ArtworkTheme.ButtonAnimated(g, "CLOSE", xClose, by, wClose, bh, s, false, Hov("check_prompt:close", _hover == "check_prompt:close"), true, 13 * s, _alpha, press: Press("check_prompt:close"));
            AddHit("check_prompt:close", xClose, by, wClose, bh);
            if (hasUpdates)
            {
                ArtworkTheme.ButtonAnimated(g, "INSTALL UPDATES", xInstall, by, wInstall, bh, s, true, Hov("check_prompt:install", _hover == "check_prompt:install"), true, 12 * s, _alpha, press: Press("check_prompt:install"));
                AddHit("check_prompt:install", xInstall, by, wInstall, bh);
            }
            PopFade(g, st);
        }

        void DrawBar(Graphics g, float x, float y, float w, float h, float frac, Color fill)
        {
            using (var p = ArtworkTheme.Para(x, y, w, h, h * 0.5f))
            using (var b = new SolidBrush(A(White(26))))
                g.FillPath(b, p);
            frac = Clamp01(frac);
            if (frac <= 0) return;
            using (var p = ArtworkTheme.Para(x, y, Math.Max(h, w * frac), h, h * 0.5f))
            using (var b = new SolidBrush(A(fill)))
                g.FillPath(b, p);
        }
}
