// SPDX-License-Identifier: GPL-3.0-only
// Setup of PEUSSOK: for the current user, no administrator rights. Built twice: the setup (with the program inside) and, with UNINSTALLER
// defined, the program that removes it again.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("PEUSSOK Setup")]
[assembly: AssemblyProduct("PEUSSOK")]
[assembly: AssemblyCopyright("Copyright (c) 2026 RXTIGA90")]
[assembly: AssemblyVersion(Info.Version + ".0")]
[assembly: AssemblyFileVersion(Info.Version + ".0")]

static class T
{
    public static bool Korean = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "ko";
    public static string S(string k, string e) { return Korean ? k : e; }
    public static string Name { get { return S("프쏙", "PEUSSOK"); } }
}

static class Work
{
    public const string ListFile = "installed.txt";
    const string UninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\Peussok";
    const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    public static bool Test; // no registry, no shortcuts
    public static string DefaultDir { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"Programs\Peussok"); } }
    public static string DataDir { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Peussok"); } }
    static string Link(Environment.SpecialFolder f) { return Path.Combine(Environment.GetFolderPath(f), T.Name + ".lnk"); }
    static IEnumerable<string> Links()
    {
        foreach (Environment.SpecialFolder f in new[] { Environment.SpecialFolder.Programs, Environment.SpecialFolder.DesktopDirectory })
            foreach (string n in new[] { "프쏙", "PEUSSOK" }) yield return Path.Combine(Environment.GetFolderPath(f), n + ".lnk");
    }
    public static string InstalledDir()
    {
        if (Test) return null;
        using (RegistryKey k = Registry.CurrentUser.OpenSubKey(UninstallKey))
        {
            string d = k == null ? null : k.GetValue("InstallLocation") as string;
            return !string.IsNullOrEmpty(d) && File.Exists(Path.Combine(d, ListFile)) ? d : null;
        }
    }
    // The program running out of this folder.
    public static bool Running(string dir)
    {
        bool found = false;
        string want = Path.Combine(Path.GetFullPath(dir), "Peussok.exe");
        foreach (Process p in Process.GetProcessesByName("Peussok"))
        {
            try { if (string.Equals(p.MainModule.FileName, want, StringComparison.OrdinalIgnoreCase)) found = true; }
            catch (Exception) { }
            p.Dispose();
        }
        return found;
    }
    public static string Inside(string dir, string relative)
    {
        string root = Path.GetFullPath(dir).TrimEnd('\\') + "\\", full = Path.GetFullPath(Path.Combine(root, relative));
        if (!full.StartsWith(root, StringComparison.OrdinalIgnoreCase)) throw new IOException("path: " + relative);
        return full;
    }
    // Removes what the list of an installed copy names, and folders that are then empty. Nothing else in the folder is touched.
    public static void RemoveFiles(string dir)
    {
        string list = Path.Combine(dir, ListFile);
        if (!File.Exists(list)) return;
        List<string> folders = new List<string>();
        foreach (string line in File.ReadAllLines(list, Encoding.UTF8))
        {
            if (line.Length == 0) continue;
            string f = Inside(dir, line);
            if (File.Exists(f)) { File.SetAttributes(f, FileAttributes.Normal); Delete(f); }
            for (string d = Path.GetDirectoryName(f); d.Length > Path.GetFullPath(dir).TrimEnd('\\').Length; d = Path.GetDirectoryName(d)) if (!folders.Contains(d)) folders.Add(d);
        }
        foreach (string log in folders) if (Directory.Exists(log)) foreach (string f in Directory.GetFiles(log, "nvngx*.log")) Delete(f); // written by NVIDIA's library beside its file
        Delete(list);
        folders.Sort(delegate(string a, string b) { return b.Length - a.Length; });
        folders.Add(Path.GetFullPath(dir));
        foreach (string d in folders) if (Directory.Exists(d) && Directory.GetFileSystemEntries(d).Length == 0) try { Directory.Delete(d); } catch (IOException) { }
    }
    static void Delete(string f)
    {
        for (int i = 0; ; i++)
        {
            try { File.Delete(f); return; }
            catch (Exception) { if (i == 20) throw; Thread.Sleep(150); } // still held for a moment
        }
    }
#if !UNINSTALLER
    static Stream Resource(string n) { return Assembly.GetExecutingAssembly().GetManifestResourceStream(n); }
    public static void Install(string dir, bool desktop, Action<int> progress)
    {
        dir = Path.GetFullPath(dir);
        if (Directory.Exists(dir) && Directory.GetFileSystemEntries(dir).Length > 0 && !File.Exists(Path.Combine(dir, ListFile)))
            throw new IOException(T.S("비어 있지 않은 폴더예요. 다른 폴더를 골라 주세요", "The folder is not empty. Choose another one"));
        string old = InstalledDir();
        string fresh = dir.TrimEnd('\\') + "." + Guid.NewGuid().ToString("N").Substring(0, 8); // everything is made and checked beside it first
        try
        {
            Place(dir, fresh, desktop, progress, old);
        }
        finally { if (Directory.Exists(fresh)) try { Directory.Delete(fresh, true); } catch (Exception) { } }
    }
    static void Place(string dir, string fresh, bool desktop, Action<int> progress, string old)
    {
        List<string> names = new List<string>();
        Directory.CreateDirectory(fresh);
        using (Stream s = Resource("payload.zip"))
        using (ZipArchive z = new ZipArchive(s, ZipArchiveMode.Read))
        {
            foreach (ZipArchiveEntry e in z.Entries) if (e.Name.Length > 0) names.Add(e.FullName.Replace('/', '\\'));
            names.Add("Uninstall.exe");
            long total = 0, done = 0;
            foreach (ZipArchiveEntry e in z.Entries) total += e.Length;
            byte[] b = new byte[1 << 18];
            foreach (ZipArchiveEntry e in z.Entries)
            {
                if (e.Name.Length == 0) continue;
                string f = Inside(fresh, e.FullName.Replace('/', '\\'));
                Directory.CreateDirectory(Path.GetDirectoryName(f));
                using (Stream i = e.Open())
                using (FileStream o = new FileStream(f, FileMode.Create, FileAccess.Write))
                    for (int n; (n = i.Read(b, 0, b.Length)) > 0; )
                    {
                        o.Write(b, 0, n);
                        done += n;
                        progress((int)(done * 95 / Math.Max(1, total)));
                    }
            }
        }
        // Every file is compared with the sums made at build time.
        using (StreamReader r = new StreamReader(Resource("payload.sha256"), Encoding.UTF8))
            for (string line; (line = r.ReadLine()) != null; )
            {
                if (line.Length < 66) continue;
                using (SHA256 h = SHA256.Create())
                using (FileStream f = File.OpenRead(Inside(fresh, line.Substring(66))))
                    if (BitConverter.ToString(h.ComputeHash(f)).Replace("-", "").ToLowerInvariant() != line.Substring(0, 64)) throw new IOException(T.S("파일이 손상됐어요: ", "Damaged file: ") + line.Substring(66));
            }
        using (Stream s = Resource("uninstall.exe"))
        using (FileStream o = new FileStream(Path.Combine(fresh, "Uninstall.exe"), FileMode.Create)) s.CopyTo(o);
        // Now the earlier version goes. The list is written first: if this stops half way, the next run knows what is ours.
        RemoveFiles(dir);
        Directory.CreateDirectory(dir);
        File.WriteAllLines(Path.Combine(dir, ListFile), names.ToArray(), new UTF8Encoding(false));
        foreach (string n in names)
        {
            string to = Inside(dir, n);
            Directory.CreateDirectory(Path.GetDirectoryName(to));
            if (File.Exists(to)) Delete(to);
            File.Move(Inside(fresh, n), to);
        }
        if (old != null && !string.Equals(Path.GetFullPath(old).TrimEnd('\\'), dir.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase) && !Running(old)) RemoveFiles(old);
        if (Test) { Shortcut(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(dir)), "프쏙.lnk"), Path.Combine(dir, "Peussok.exe"), dir); progress(100); return; }
        string exe = Path.Combine(dir, "Peussok.exe");
        bool had = false;
        foreach (string l in Links())
            if (File.Exists(l))
            {
                had = had || l.StartsWith(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), StringComparison.OrdinalIgnoreCase);
                File.Delete(l);
            }
        Shortcut(Link(Environment.SpecialFolder.Programs), exe, dir);
        if (desktop || had) Shortcut(Link(Environment.SpecialFolder.DesktopDirectory), exe, dir);
        if (old != null) // started with Windows: from the new folder
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(RunKey, true))
                if (k != null && k.GetValue("Peussok") is string) k.SetValue("Peussok", "\"" + exe + "\"");
        using (RegistryKey k = Registry.CurrentUser.CreateSubKey(UninstallKey))
        {
            k.SetValue("DisplayName", T.Name);
            k.SetValue("DisplayVersion", Info.Version);
            k.SetValue("Publisher", "RXTIGA90");
            k.SetValue("InstallLocation", dir);
            k.SetValue("DisplayIcon", exe);
            k.SetValue("UninstallString", "\"" + Path.Combine(dir, "Uninstall.exe") + "\"");
            k.SetValue("NoModify", 1, RegistryValueKind.DWord);
            k.SetValue("NoRepair", 1, RegistryValueKind.DWord);
            long size = 0;
            foreach (string f in Directory.GetFiles(dir, "*", SearchOption.AllDirectories)) size += new FileInfo(f).Length;
            k.SetValue("EstimatedSize", (int)(size / 1024), RegistryValueKind.DWord);
        }
        progress(100);
    }
    [ComImport, Guid("000214F9-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellLinkW
    {
        void GetPath(IntPtr f, int n, IntPtr d, uint g);
        void GetIDList(out IntPtr p);
        void SetIDList(IntPtr p);
        void GetDescription(IntPtr n, int c);
        void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string n);
        void GetWorkingDirectory(IntPtr d, int c);
        void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string d);
        void GetArguments(IntPtr a, int c);
        void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string a);
        void GetHotkey(out short h);
        void SetHotkey(short h);
        void GetShowCmd(out int s);
        void SetShowCmd(int s);
        void GetIconLocation(IntPtr p, int c, out int i);
        void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string p, int i);
        void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string p, uint r);
        void Resolve(IntPtr w, uint f);
        void SetPath([MarshalAs(UnmanagedType.LPWStr)] string f);
    }
    [ComImport, Guid("00021401-0000-0000-C000-000000000046")]
    class ShellLink { }
    static void Shortcut(string lnk, string exe, string dir)
    {
        IShellLinkW l = (IShellLinkW)new ShellLink();
        l.SetPath(exe);
        l.SetWorkingDirectory(dir);
        ((System.Runtime.InteropServices.ComTypes.IPersistFile)l).Save(lnk, true);
        Marshal.ReleaseComObject(l);
    }
#endif
    public static void Uninstall(string dir, bool settings)
    {
        if (!File.Exists(Path.Combine(dir, ListFile))) throw new IOException(T.S("설치 기록(installed.txt)이 없어요", "installed.txt is missing"));
        RemoveFiles(dir);
        if (Test) return;
        foreach (string l in Links()) if (File.Exists(l)) File.Delete(l);
        using (RegistryKey k = Registry.CurrentUser.OpenSubKey(RunKey, true))
        {
            string v = k == null ? null : k.GetValue("Peussok") as string;
            if (v != null && v.IndexOf(Path.GetFullPath(dir).TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase) >= 0) k.DeleteValue("Peussok", false);
        }
        Registry.CurrentUser.DeleteSubKeyTree(UninstallKey, false);
        if (settings && Directory.Exists(DataDir) && Process.GetProcessesByName("Peussok").Length == 0) Directory.Delete(DataDir, true); // not while another copy uses them
    }
}

class Window : Form
{
    static readonly Color Back = Color.FromArgb(17, 22, 20), Card = Color.FromArgb(27, 35, 32), Line = Color.FromArgb(48, 62, 56), Ink = Color.FromArgb(234, 242, 238), Soft = Color.FromArgb(150, 168, 160), Green = Color.FromArgb(46, 204, 133);
    readonly float k;
    Label text;
    Button go, cancel;
    CheckBox check;
    Bar bar;
    bool finished, busy;
    string dir;
#if !UNINSTALLER
    TextBox box;
    Button pick;
#endif
    class Bar : Control
    {
        public int Value;
        public Bar() { SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true); }
        protected override void OnPaint(PaintEventArgs e)
        {
            using (SolidBrush b = new SolidBrush(Line)) e.Graphics.FillRectangle(b, ClientRectangle);
            using (SolidBrush b = new SolidBrush(Green)) e.Graphics.FillRectangle(b, 0, 0, Width * Value / 100, Height);
        }
    }
    int Px(int v) { return (int)Math.Round(v * k); }
    Button Make(string t, int x, int y, int w, bool strong)
    {
        Button b = new Button();
        b.Text = t;
        b.FlatStyle = FlatStyle.Flat;
        b.FlatAppearance.BorderColor = strong ? Green : Line;
        b.BackColor = strong ? Green : Card;
        b.ForeColor = strong ? Color.FromArgb(8, 24, 16) : Ink;
        b.Font = new Font(Font.FontFamily, 10f, strong ? FontStyle.Bold : FontStyle.Regular);
        b.SetBounds(Px(x), Px(y), Px(w), Px(38));
        Controls.Add(b);
        return b;
    }
    public Window(string target)
    {
        using (Graphics g = CreateGraphics()) k = g.DpiX / 96f;
        dir = target;
        Font = new Font(T.Korean ? "Malgun Gothic" : "Segoe UI", 9.75f);
        AutoScaleMode = AutoScaleMode.None;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Back;
        ForeColor = Ink;
        ClientSize = new Size(Px(520), Px(330));
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { }
        Label title = new Label();
        title.Font = new Font(Font.FontFamily, 20f, FontStyle.Bold);
        title.ForeColor = Ink;
        title.SetBounds(Px(28), Px(22), Px(464), Px(44));
        Controls.Add(title);
        text = new Label();
        text.ForeColor = Soft;
        text.SetBounds(Px(30), Px(70), Px(460), Px(70));
        Controls.Add(text);
        check = new CheckBox();
        check.ForeColor = Ink;
        check.FlatStyle = FlatStyle.Flat;
        check.SetBounds(Px(30), Px(196), Px(460), Px(28));
        Controls.Add(check);
        bar = new Bar();
        bar.SetBounds(Px(30), Px(238), Px(460), Px(6));
        bar.Visible = false;
        Controls.Add(bar);
#if UNINSTALLER
        Text = T.S("프쏙 지우기", "Remove PEUSSOK");
        title.Text = Text;
        text.Text = T.S("프쏙을 이 컴퓨터에서 지워요.\r\n", "Removes PEUSSOK from this computer.\r\n") + dir;
        check.Text = T.S("설정과 기록도 지우기", "Also remove settings and logs");
        go = Make(T.S("지우기", "Remove"), 250, 268, 120, true);
#else
        Text = T.S("프쏙 설치", "PEUSSOK Setup");
        title.Text = T.Name + " " + Info.Version + T.S(" 시험판", " trial");
        text.Text = T.S("캡처카드 화면의 프레임을 늘려 보여 주는 프로그램이에요.\r\n관리자 권한 없이 내 계정에만 설치해요. 캡처카드를 쓰는 다른 프로그램은 꺼 주세요.", "Shows a capture card's picture with frame generation.\r\nInstalled for your account only, without administrator rights. Close other programs that use the card.");
        box = new TextBox();
        box.Text = dir;
        box.BackColor = Card;
        box.ForeColor = Ink;
        box.BorderStyle = BorderStyle.FixedSingle;
        box.SetBounds(Px(30), Px(150), Px(350), Px(28));
        Controls.Add(box);
        pick = Make(T.S("폴더 바꾸기", "Browse"), 390, 145, 100, false);
        pick.Height = Px(32);
        pick.Click += delegate
        {
            using (FolderBrowserDialog d = new FolderBrowserDialog())
                if (d.ShowDialog(this) == DialogResult.OK) box.Text = Path.GetFileName(d.SelectedPath).Equals("Peussok", StringComparison.OrdinalIgnoreCase) ? d.SelectedPath : Path.Combine(d.SelectedPath, "Peussok");
        };
        check.Text = T.S("바탕 화면에 바로 가기 만들기", "Desktop shortcut");
        check.Checked = true;
        go = Make(T.S("설치", "Install"), 250, 268, 120, true);
#endif
        cancel = Make(T.S("닫기", "Close"), 380, 268, 110, false);
        cancel.Click += delegate { Close(); };
        go.Click += delegate { Start(); };
        AcceptButton = go;
        FormClosing += delegate(object o, FormClosingEventArgs e) { if (busy) e.Cancel = true; };
    }
    void Start()
    {
#if !UNINSTALLER
        if (finished)
        {
            Process.Start(new ProcessStartInfo(Path.Combine(dir, "Peussok.exe")) { WorkingDirectory = dir });
            Close();
            return;
        }
        dir = box.Text.Trim();
        bool usable = false;
        try { usable = Path.IsPathRooted(dir) && Path.GetFullPath(dir).Length > 3; } catch (Exception) { }
        if (!usable) { text.Text = T.S("설치할 폴더를 골라 주세요.", "Please choose a folder."); return; }
#endif
        if (finished) { Close(); return; }
        string old = Work.InstalledDir();
        while (Work.Running(dir) || (old != null && Work.Running(old)))
            if (MessageBox.Show(this, T.S("프쏙이 켜져 있어요. 끈 다음 '다시 시도'를 누르세요.", "PEUSSOK is running. Close it and press Retry."), Text, MessageBoxButtons.RetryCancel) != DialogResult.Retry) return;
        busy = true;
        go.Enabled = cancel.Enabled = check.Enabled = false;
        bar.Visible = true;
#if !UNINSTALLER
        box.Enabled = pick.Enabled = false;
#endif
        bool option = check.Checked;
        Thread t = new Thread(delegate()
        {
            Exception bad = null;
            try
            {
#if UNINSTALLER
                Work.Uninstall(dir, option);
#else
                int last = -1;
                Work.Install(dir, option, delegate(int p) { if (p != last) { last = p; BeginInvoke((Action)delegate { bar.Value = p; bar.Invalidate(); }); } });
#endif
            }
            catch (Exception e) { bad = e; }
            BeginInvoke((Action)delegate { Done(bad); });
        });
        t.IsBackground = true;
        t.SetApartmentState(ApartmentState.STA);
        t.Start();
    }
    void Done(Exception bad)
    {
        busy = false;
        cancel.Enabled = go.Enabled = true;
        if (bad != null)
        {
            text.Text = T.S("끝내지 못했어요: ", "Not finished: ") + bad.Message;
            check.Enabled = true;
#if !UNINSTALLER
            box.Enabled = pick.Enabled = true;
#endif
            return;
        }
        finished = true;
        bar.Value = 100;
        bar.Invalidate();
        check.Visible = false;
#if UNINSTALLER
        text.Text = T.S("프쏙을 지웠어요.", "PEUSSOK was removed.");
        go.Visible = false;
#else
        box.Visible = pick.Visible = false;
        text.Text = T.S("설치했어요. 시작 메뉴에서 '프쏙'을 찾을 수 있어요.", "Installed. You find PEUSSOK in the start menu.");
        go.Text = T.S("실행", "Start");
#endif
    }
}

static class Program
{
    [DllImport("user32.dll")]
    static extern bool SetProcessDPIAware();
#if UNINSTALLER
    // The copy in the temporary folder removes itself.
    static void Gone()
    {
        if (Path.GetFileName(Application.ExecutablePath).StartsWith("Peussok-remove-")) Process.Start(new ProcessStartInfo("cmd.exe", "/c timeout /t 2 /nobreak >nul & del \"" + Application.ExecutablePath + "\"") { CreateNoWindow = true, UseShellExecute = false });
    }
#endif
    [STAThread]
    static int Main(string[] a)
    {
        SetProcessDPIAware();
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        string dir = null, from = null;
        bool silent = false, shot = false;
#if UNINSTALLER
        bool settings = false;
#endif
        foreach (string s in a)
        {
            if (s == "/S") silent = true;
            else if (s == "/test") Work.Test = true;
#if UNINSTALLER
            else if (s == "/settings") settings = true;
#endif
            else if (s == "/en") T.Korean = false;
            else if (s == "/ko") T.Korean = true;
            else if (s.StartsWith("/shot=")) { shot = true; from = s.Substring(6); }
            else if (s.StartsWith("/D=")) dir = s.Substring(3);
            else if (s.StartsWith("/from=")) dir = s.Substring(6);
        }
        try
        {
#if UNINSTALLER
            // Runs from a copy in the temporary folder, so that its own file can be removed too.
            string own = Application.ExecutablePath, here = Path.GetDirectoryName(own);
            if (dir == null && File.Exists(Path.Combine(here, Work.ListFile)) && !shot)
            {
                string copy = Path.Combine(Path.GetTempPath(), "Peussok-remove-" + Guid.NewGuid().ToString("N") + ".exe");
                File.Copy(own, copy);
                string args = "\"/from=" + here.TrimEnd('\\') + "\"";
                foreach (string s in a) args += " " + s;
                Process.Start(new ProcessStartInfo(copy, args) { UseShellExecute = false, WorkingDirectory = Path.GetTempPath() });
                return 0;
            }
            if (dir == null) dir = Work.InstalledDir() ?? here;
            if (silent) { if (Work.Running(dir)) return 2; try { Work.Uninstall(dir, settings); } finally { Gone(); } return 0; }
#else
            if (dir == null) dir = Work.InstalledDir() ?? Work.DefaultDir;
            if (silent) { if (Work.Running(dir)) return 2; Work.Install(dir, false, delegate { }); return 0; }
#endif
        }
        catch (Exception e) { Console.Error.WriteLine(e.Message); if (!silent) MessageBox.Show(e.Message); return 1; }
        Window w = new Window(dir);
        if (shot) // a picture of the window for tests, outside the screen
        {
            w.StartPosition = FormStartPosition.Manual;
            w.Location = new Point(-30000, -30000);
            w.ShowInTaskbar = false;
            w.Show();
            Application.DoEvents();
            using (Bitmap b = new Bitmap(w.Width, w.Height)) { w.DrawToBitmap(b, new Rectangle(0, 0, w.Width, w.Height)); b.Save(from); }
            return 0;
        }
        Application.Run(w);
#if UNINSTALLER
        Gone();
#endif
        return 0;
    }
}
