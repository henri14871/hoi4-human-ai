// HumanAI.exe - settings window for Human AI.
//
// version.dll starts it when Hearts of Iron IV launches ("--pid <game pid>").
// It edits human_ai.ini next to hoi4.exe (the dll re-reads it within a second),
// shows the dll's state from human_ai_status.ini, and closes when the game exits.
// It never touches the game process itself.
//
// Built with the C# compiler that ships with Windows (.NET Framework 4.x), so it is
// written in C# 5. See native/build.bat.
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

static class Program {
    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

    [STAThread]
    static int Main(string[] args) {
        bool created;
        using (var single = new Mutex(true, @"Local\HumanAI_settings_window", out created)) {
            if (!created) return 0;  // already open

            SetProcessDPIAware();
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);

            Process game = FindGame(args);
            if (game == null) {
                MessageBox.Show("Hearts of Iron IV isn't running.\n\nHuman AI opens this window by itself when you start the game.",
                                "Human AI", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return 1;
            }
            string dir;
            try { dir = Path.GetDirectoryName(game.MainModule.FileName); }
            catch (Exception) { dir = AppDomain.CurrentDomain.BaseDirectory; }

            Application.Run(new SettingsForm(game, dir));
            return 0;
        }
    }

    static Process FindGame(string[] args) {
        for (int i = 0; i + 1 < args.Length; ++i) {
            int pid;
            if (args[i] == "--pid" && int.TryParse(args[i + 1], out pid)) {
                try {
                    Process p = Process.GetProcessById(pid);
                    if (!p.HasExited) return p;
                } catch (ArgumentException) { }
            }
        }
        Process[] found = Process.GetProcessesByName("hoi4");
        return found.Length > 0 ? found[0] : null;
    }
}

static class Ini {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern int GetPrivateProfileInt(string section, string key, int def, string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern int GetPrivateProfileString(string section, string key, string def, StringBuilder ret, int size, string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern bool WritePrivateProfileString(string section, string key, string value, string path);

    public static int Int(string path, string section, string key, int def) {
        return GetPrivateProfileInt(section, key, def, path);
    }
    public static string Str(string path, string section, string key, string def) {
        var sb = new StringBuilder(256);
        GetPrivateProfileString(section, key, def, sb, sb.Capacity, path);
        return sb.ToString();
    }
    public static void Set(string path, string section, string key, string value) {
        WritePrivateProfileString(section, key, value, path);
    }
}

class SettingsForm : Form {
    // key in human_ai.ini, label, description. Keys must match kCategoryKeys in native/src/hai.cpp.
    static readonly string[][] Government = {
        new[] { "foreign", "Diplomacy", "Guarantees, factions, wars, lend-lease" },
        new[] { "political", "Politics", "National focus, laws, advisors, scientists" },
        new[] { "interior", "Economy", "Trade, garrisons, equipment and ship designs" },
    };
    static readonly string[][] Forces = {
        new[] { "land", "Land forces", "Fronts, armies, army groups, invasions" },
        new[] { "air", "Air force", "Air bases and air missions" },
        new[] { "navy", "Navy", "Naval objectives and fleets" },
        new[] { "volunteers", "Volunteers", "Volunteer forces sent abroad" },
    };
    static readonly string[][] Intelligence = {
        new[] { "cryptology", "Cryptology", "Code breaking" },
    };

    static readonly Color CBg = Color.FromArgb(23, 25, 22);
    static readonly Color CPanel = Color.FromArgb(34, 36, 30);
    static readonly Color CText = Color.FromArgb(235, 230, 214);
    static readonly Color CMuted = Color.FromArgb(148, 145, 132);
    static readonly Color CBrass = Color.FromArgb(199, 168, 97);
    static readonly Color CAiOn = Color.FromArgb(140, 120, 64);
    static readonly Color CBad = Color.FromArgb(214, 110, 96);
    static readonly Color CGood = Color.FromArgb(150, 190, 110);

    readonly Process game;
    readonly string ini, statusIni;
    readonly System.Collections.Generic.Dictionary<string, Button> toggles =
        new System.Collections.Generic.Dictionary<string, Button>();
    readonly CheckBox enabledBox, topBox;
    readonly Label statusLabel, packLabel;
    readonly ComboBox packBox;
    readonly string packsDir;
    readonly System.Windows.Forms.Timer timer;
    DateTime iniTime;
    bool loading;

    const string NoPack = "None (vanilla AI)";

    public SettingsForm(Process game, string dir) {
        this.game = game;
        ini = Path.Combine(dir, "human_ai.ini");
        statusIni = Path.Combine(dir, "human_ai_status.ini");
        packsDir = Path.Combine(dir, "human_ai_packs");

        Text = "Human AI";
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        BackColor = CBg;
        ForeColor = CText;
        Font = new Font("Segoe UI", 9.75f);
        ClientSize = new Size(380, 10);
        StartPosition = FormStartPosition.Manual;

        int y = 14;
        var title = MakeLabel("Human AI", 16, y, new Font("Segoe UI Semibold", 15f), CBrass);
        Controls.Add(title);
        y += 34;
        statusLabel = MakeLabel("", 16, y, Font, CMuted);
        statusLabel.AutoSize = false;
        statusLabel.Size = new Size(348, 20);
        Controls.Add(statusLabel);
        y += 28;

        enabledBox = new CheckBox { Text = "Enabled", Location = new Point(16, y), AutoSize = true, ForeColor = CText };
        enabledBox.CheckedChanged += delegate { if (!loading) Save("enabled", enabledBox.Checked); };
        Controls.Add(enabledBox);
        y += 32;

        y = AddGroup("Government", Government, y);
        y = AddGroup("Armed forces", Forces, y);
        y = AddGroup("Intelligence", Intelligence, y);

        var allAi = MakeButton("All to AI", 16, y, 170);
        allAi.Click += delegate { SetAll(true); };
        var allMe = MakeButton("All to me", 194, y, 170);
        allMe.Click += delegate { SetAll(false); };
        Controls.Add(allAi);
        Controls.Add(allMe);
        y += 46;

        // Optional AI pack: any AI mod's folder in human_ai_packs. Only its AI defines are used.
        Controls.Add(MakeLabel("AI PACK (OPTIONAL)", 16, y, new Font("Segoe UI Semibold", 8.5f), CBrass));
        y += 22;
        packBox = new ComboBox { Location = new Point(16, y), Width = 262, DropDownStyle = ComboBoxStyle.DropDownList,
                                 BackColor = CPanel, ForeColor = CText, FlatStyle = FlatStyle.Flat };
        packBox.SelectedIndexChanged += delegate {
            if (loading) return;
            string pick = (string)packBox.SelectedItem;
            Ini.Set(ini, "human_ai", "ai_pack", pick == NoPack ? "" : pick);
            iniTime = File.Exists(ini) ? File.GetLastWriteTimeUtc(ini) : DateTime.MinValue;
        };
        packBox.DropDown += delegate { FillPacks(); };
        Controls.Add(packBox);
        var open = MakeButton("Folder", 286, y - 1, 78);
        open.Click += delegate {
            Directory.CreateDirectory(packsDir);
            Process.Start("explorer.exe", "\"" + packsDir + "\"");
        };
        Controls.Add(open);
        y += 34;
        packLabel = MakeLabel("Drop an AI mod's folder into human_ai_packs to use it.", 16, y,
                              new Font("Segoe UI", 8.5f), CMuted);
        packLabel.AutoSize = false;
        packLabel.Size = new Size(348, 34);
        Controls.Add(packLabel);
        y += 40;

        topBox = new CheckBox { Text = "Keep on top of the game", Location = new Point(16, y), AutoSize = true, ForeColor = CMuted };
        topBox.CheckedChanged += delegate {
            TopMost = topBox.Checked;
            if (!loading) Ini.Set(ini, "app", "keep_on_top", topBox.Checked ? "1" : "0");
        };
        Controls.Add(topBox);
        y += 30;
        var foot = MakeLabel("Closes by itself when the game exits. With the Human AI\n" +
                             "mod enabled, each save's in-game choices take over on load.",
                             16, y, new Font("Segoe UI", 8.5f), CMuted);
        Controls.Add(foot);
        y += 40;

        ClientSize = new Size(380, y);
        Location = SavedLocation();

        LoadFromIni();
        timer = new System.Windows.Forms.Timer { Interval = 1000 };
        timer.Tick += delegate { Tick(); };
        timer.Start();
        Tick();
    }

    // Opened by the game: show without pulling focus away from it.
    protected override bool ShowWithoutActivation { get { return true; } }

    protected override CreateParams CreateParams {
        get {
            CreateParams cp = base.CreateParams;
            if (Ini.Int(ini, "app", "keep_on_top", 1) != 0) cp.ExStyle |= 0x8;  // WS_EX_TOPMOST
            return cp;
        }
    }

    protected override void OnFormClosed(FormClosedEventArgs e) {
        Ini.Set(ini, "app", "x", Location.X.ToString());
        Ini.Set(ini, "app", "y", Location.Y.ToString());
        base.OnFormClosed(e);
    }

    Point SavedLocation() {
        Rectangle area = Screen.PrimaryScreen.WorkingArea;
        int x = Ini.Int(ini, "app", "x", area.Left + 24);
        int y = Ini.Int(ini, "app", "y", area.Bottom - Height - 24);
        var p = new Point(x, y);
        foreach (Screen s in Screen.AllScreens)
            if (s.WorkingArea.Contains(p)) return p;
        return new Point(area.Left + 24, area.Top + 24);
    }

    int AddGroup(string name, string[][] rows, int y) {
        Controls.Add(MakeLabel(name.ToUpperInvariant(), 16, y, new Font("Segoe UI Semibold", 8.5f), CBrass));
        y += 22;
        foreach (string[] r in rows) {
            var row = new Panel { Location = new Point(16, y), Size = new Size(348, 46), BackColor = CPanel };
            row.Controls.Add(MakeLabel(r[1], 10, 4, new Font("Segoe UI Semibold", 9.75f), CText));
            row.Controls.Add(MakeLabel(r[2], 10, 24, new Font("Segoe UI", 8.25f), CMuted));
            var b = MakeButton("You", 262, 8, 76);
            string key = r[0];
            b.Click += delegate { Save(key, !IsAi(b)); };
            row.Controls.Add(b);
            toggles[key] = b;
            Controls.Add(row);
            y += 50;
        }
        return y + 10;
    }

    static Label MakeLabel(string text, int x, int y, Font font, Color color) {
        return new Label { Text = text, Location = new Point(x, y), AutoSize = true, Font = font, ForeColor = color, BackColor = Color.Transparent };
    }

    static Button MakeButton(string text, int x, int y, int w) {
        var b = new Button { Text = text, Location = new Point(x, y), Size = new Size(w, 30), FlatStyle = FlatStyle.Flat,
                             BackColor = Color.FromArgb(48, 50, 42), ForeColor = CText, Cursor = Cursors.Hand };
        b.FlatAppearance.BorderColor = Color.FromArgb(90, 84, 60);
        return b;
    }

    static bool IsAi(Button b) { return b.Text == "AI"; }

    static void ShowState(Button b, bool ai) {
        b.Text = ai ? "AI" : "You";
        b.BackColor = ai ? CAiOn : Color.FromArgb(48, 50, 42);
        b.FlatAppearance.BorderColor = ai ? CBrass : Color.FromArgb(90, 84, 60);
    }

    void Save(string key, bool on) {
        Ini.Set(ini, "human_ai", key, on ? "1" : "0");
        Button b;
        if (toggles.TryGetValue(key, out b)) ShowState(b, on);
        iniTime = File.Exists(ini) ? File.GetLastWriteTimeUtc(ini) : DateTime.MinValue;
    }

    void SetAll(bool ai) {
        foreach (string key in toggles.Keys) Ini.Set(ini, "human_ai", key, ai ? "1" : "0");
        LoadFromIni();
    }

    void FillPacks() {
        bool was = loading;
        loading = true;
        string current = Ini.Str(ini, "human_ai", "ai_pack", "");
        packBox.Items.Clear();
        packBox.Items.Add(NoPack);
        if (Directory.Exists(packsDir))
            foreach (string d in Directory.GetDirectories(packsDir)) packBox.Items.Add(Path.GetFileName(d));
        if (current.Length > 0 && !packBox.Items.Contains(current)) packBox.Items.Add(current);
        packBox.SelectedItem = current.Length > 0 ? current : NoPack;
        loading = was;
    }

    void LoadFromIni() {
        FillPacks();
        loading = true;
        enabledBox.Checked = Ini.Int(ini, "human_ai", "enabled", 1) != 0;
        foreach (var kv in toggles) ShowState(kv.Value, Ini.Int(ini, "human_ai", kv.Key, 0) != 0);
        topBox.Checked = Ini.Int(ini, "app", "keep_on_top", 1) != 0;
        TopMost = topBox.Checked;
        loading = false;
        iniTime = File.Exists(ini) ? File.GetLastWriteTimeUtc(ini) : DateTime.MinValue;
    }

    void Tick() {
        bool gone;
        try { gone = game.HasExited; } catch (Exception) { gone = true; }
        if (gone) { Close(); return; }

        // Edited by hand while open: pick it up.
        DateTime t = File.Exists(ini) ? File.GetLastWriteTimeUtc(ini) : DateTime.MinValue;
        if (t != iniTime) LoadFromIni();

        string pick = Ini.Str(ini, "human_ai", "ai_pack", "");
        string appliedPack = Ini.Str(statusIni, "pack", "name", "");
        if (pick.Length == 0) {
            packLabel.Text = "Vanilla AI. Drop an AI mod's folder into human_ai_packs to use it.";
        } else if (appliedPack == pick) {
            packLabel.Text = string.Format("{0}: {1} AI settings applied, {2} gameplay settings ignored.",
                pick, Ini.Int(statusIni, "pack", "applied", 0), Ini.Int(statusIni, "pack", "ignored", 0));
        } else {
            packLabel.Text = pick + ": applies when a game is loaded.";
        }

        string state = Ini.Str(statusIni, "status", "state", "");
        bool ours = Ini.Int(statusIni, "status", "pid", 0) == game.Id;
        if (!ours) {
            statusLabel.Text = "Waiting for the game to load Human AI...";
            statusLabel.ForeColor = CMuted;
        } else if (state == "active") {
            bool engine = Ini.Int(statusIni, "status", "engine_flag", 0) != 0;
            statusLabel.Text = engine ? "Active: the AI is running the areas marked AI." : "Active: you control everything.";
            statusLabel.ForeColor = CGood;
        } else if (state == "unsupported") {
            statusLabel.Text = "This game version isn't supported yet. The game is vanilla.";
            statusLabel.ForeColor = CBad;
        } else if (state == "stood_down") {
            statusLabel.Text = "Stood down after an internal check. See human_ai.log.";
            statusLabel.ForeColor = CBad;
        }
    }
}
