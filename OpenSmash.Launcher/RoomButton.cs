using System.Drawing;
using System.Windows.Forms;
namespace OpenSmash.Launcher;
internal sealed class RoomButton : Button
{
    readonly bool primary;
    bool hover;
    bool selected;
    public float UiScale { get; set; } = 1;
    public bool Selected { get => selected; set { selected = value; Invalidate(); } }
    public RoomButton(string text, string name, bool primary = false)
    {
        Text = text; Name = name; this.primary = primary;
        AccessibleName = text;
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
        FlatStyle = FlatStyle.Flat; BackColor = Theme.Bg;
        MouseEnter += (_, _) => { hover = true; Invalidate(); };
        MouseLeave += (_, _) => { hover = false; Invalidate(); };
        GotFocus += (_, _) => Invalidate(); LostFocus += (_, _) => Invalidate();
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(BackColor);
        Theme.ButtonAnimated(e.Graphics, Text, 0, 3*UiScale, Width-1, Height-4*UiScale, UiScale,
            primary || selected, hover || Focused ? 1 : 0, Enabled, 12*UiScale);
        if (Focused) ControlPaint.DrawFocusRectangle(e.Graphics, Rectangle.Inflate(ClientRectangle,-6,-6), Theme.Text, BackColor);
    }
}
