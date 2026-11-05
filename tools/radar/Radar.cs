// UO+ радар для EnhancedMap: зелёные и рыжие с именами, общие на весь альянс.
// Источник данных — скрипт Ориона UOPlusRadar.js (пишет видимых в файл раз в секунду).
// Обмен — радар-сервер radar_server.py (HTTP, общий ключ). Настройки — Radar.ini рядом с EnhancedMap.exe.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Net;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace EnhancedMap.Core
{
    public sealed class RadarContact
    {
        public uint Serial;
        public int X, Y;
        public byte Map;
        public byte Noto; // 2 зелёный, 5 рыжий
        public int Hp = -1; // проценты, -1 неизвестно
        public string Name = "";
        public string Reporter = "";
        public DateTime Seen;
    }

    public static class Radar
    {
        public const int NOTO_GREEN = 2;
        public const int NOTO_ORANGE = 5;

        private static readonly object _lock = new object();
        private static readonly Dictionary<uint, RadarContact> _contacts = new Dictionary<uint, RadarContact>();
        private static volatile bool _dirty;
        private static Thread _thread;

        private static string _server = "";
        private static string _key = "";
        private static string _file = "";
        private static bool _enabled = true;
        private static bool _showGreen = true;
        private static bool _showOrange = true;
        private static int _intervalMs = 1000;
        private static int _ttlMs = 8000;
        private static int _fileStaleMs = 4000;
        private static int _hwndPid = -1;
        private static string _clientDir = "";

        private static readonly Font _font = new Font("Segoe UI", 11, FontStyle.Bold, GraphicsUnit.Pixel);
        private static readonly SolidBrush _green = new SolidBrush(Color.FromArgb(0x40, 0xFF, 0x40));
        private static readonly SolidBrush _orange = new SolidBrush(Color.FromArgb(0xFF, 0x99, 0x20));
        private static readonly SolidBrush _back = new SolidBrush(Color.FromArgb(160, Color.Black));
        private static readonly Pen _outline = new Pen(Color.Black, 1);

        public static string Status { get; private set; } = "выключен";
        public static int LocalCount { get; private set; }

        public static string IniPath => Path.Combine(Path.GetDirectoryName(Assembly.GetEntryAssembly().Location), "Radar.ini");

        public static void Start()
        {
            try
            {
                LoadIni();
            }
            catch (Exception ex)
            {
                Status = "ошибка Radar.ini: " + ex.Message;
                return;
            }

            if (!_enabled)
                return;

            ServicePointManager.Expect100Continue = false;
            _thread = new Thread(Loop) {IsBackground = true, Name = "UOPlusRadar"};
            _thread.Start();
        }

        // Вызывается из таймера окна: есть ли что перерисовать.
        public static bool Tick()
        {
            if (!_dirty)
                return false;
            _dirty = false;
            return true;
        }

        private static void LoadIni()
        {
            if (!File.Exists(IniPath))
            {
                File.WriteAllText(IniPath,
                    "; UO+ радар: зелёные и рыжие на карте для всего альянса\r\n" +
                    "[radar]\r\n" +
                    "enabled=1\r\n" +
                    "; адрес радар-сервера, например http://1.2.3.4:8899/radar (пусто — только свои глаза)\r\n" +
                    "server=\r\n" +
                    "key=\r\n" +
                    "; файл, который пишет скрипт Ориона (пусто — искать UOPlusRadar.txt в папке Ориона)\r\n" +
                    "file=\r\n" +
                    "show_green=1\r\n" +
                    "show_orange=1\r\n", Encoding.UTF8);
            }

            foreach (string raw in File.ReadAllLines(IniPath, Encoding.UTF8))
            {
                string line = raw.Trim();
                if (line.Length == 0 || line[0] == ';' || line[0] == '#' || line[0] == '[')
                    continue;
                int eq = line.IndexOf('=');
                if (eq <= 0)
                    continue;
                string k = line.Substring(0, eq).Trim().ToLowerInvariant();
                string v = line.Substring(eq + 1);
                int cm = v.IndexOf(" ;", StringComparison.Ordinal);
                if (cm >= 0)
                    v = v.Substring(0, cm); // комментарий в конце строки
                v = v.Trim();
                switch (k)
                {
                    case "enabled": _enabled = v != "0"; break;
                    case "server": _server = v; break;
                    case "key": _key = v; break;
                    case "file": _file = v; break;
                    case "show_green": _showGreen = v != "0"; break;
                    case "show_orange": _showOrange = v != "0"; break;
                    case "interval": int.TryParse(v, out _intervalMs); break;
                    case "ttl": int.TryParse(v, out _ttlMs); break;
                }
            }

            if (_intervalMs < 300) _intervalMs = 300;
            if (_ttlMs < 2000) _ttlMs = 2000;
        }

        private static void Loop()
        {
            int backoff = 0;
            DateTime nextServer = DateTime.MinValue;

            while (true)
            {
                try
                {
                    List<RadarContact> local = ReadLocal();
                    DateTime now = DateTime.UtcNow;

                    lock (_lock)
                    {
                        foreach (RadarContact c in local)
                        {
                            _contacts[c.Serial] = c;
                        }
                    }

                    if (_server.Length > 0 && now >= nextServer)
                    {
                        try
                        {
                            List<RadarContact> remote = Exchange(local);
                            DateTime t = DateTime.UtcNow;
                            lock (_lock)
                            {
                                foreach (RadarContact c in remote)
                                {
                                    if (_contacts.TryGetValue(c.Serial, out RadarContact old) && old.Seen >= c.Seen)
                                        continue;
                                    _contacts[c.Serial] = c;
                                }
                            }

                            Status = "сервер: ок, видно " + remote.Count;
                            backoff = 0;
                        }
                        catch (Exception ex)
                        {
                            backoff = Math.Min(backoff == 0 ? 2 : backoff * 2, 30);
                            nextServer = DateTime.UtcNow.AddSeconds(backoff);
                            Status = "сервер недоступен: " + ex.Message;
                        }
                    }
                    else if (_server.Length == 0)
                        Status = "без сервера, вижу " + local.Count;

                    Prune();
                    _dirty = true;
                }
                catch (Exception ex)
                {
                    Status = "ошибка: " + ex.Message;
                }

                Thread.Sleep(_intervalMs);
            }
        }

        private static void Prune()
        {
            DateTime limit = DateTime.UtcNow.AddMilliseconds(-_ttlMs);
            lock (_lock)
            {
                List<uint> old = null;
                foreach (KeyValuePair<uint, RadarContact> kv in _contacts)
                {
                    if (kv.Value.Seen < limit)
                        (old ?? (old = new List<uint>())).Add(kv.Key);
                }

                if (old != null)
                    foreach (uint s in old)
                        _contacts.Remove(s);
            }
        }

        // ---------- файл от Ориона ----------

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

        private static IEnumerable<string> Candidates()
        {
            if (_file.Length > 0)
            {
                yield return _file;
                yield break;
            }

            try
            {
                IntPtr h = UOClientManager.hWnd;
                if (h != IntPtr.Zero)
                {
                    GetWindowThreadProcessId(h, out uint pid);
                    if ((int) pid != _hwndPid)
                    {
                        _hwndPid = (int) pid;
                        _clientDir = Path.GetDirectoryName(Process.GetProcessById((int) pid).MainModule.FileName);
                    }
                }
            }
            catch
            {
                _clientDir = "";
            }

            if (_clientDir.Length > 0)
            {
                yield return Path.Combine(_clientDir, "UOPlusRadar.txt");
                yield return Path.Combine(_clientDir, "OA", "UOPlusRadar.txt");
                yield return Path.Combine(_clientDir, "Scripts", "UOPlusRadar.txt");
            }

            string me = Path.GetDirectoryName(Assembly.GetEntryAssembly().Location);
            yield return Path.Combine(me, "UOPlusRadar.txt");
        }

        private static List<RadarContact> ReadLocal()
        {
            var result = new List<RadarContact>();
            string best = null;
            DateTime bestTime = DateTime.MinValue;

            foreach (string p in Candidates())
            {
                try
                {
                    if (!File.Exists(p))
                        continue;
                    DateTime t = File.GetLastWriteTimeUtc(p);
                    if (t > bestTime)
                    {
                        bestTime = t;
                        best = p;
                    }
                }
                catch
                {
                }
            }

            LocalCount = 0;
            if (best == null || (DateTime.UtcNow - bestTime).TotalMilliseconds > _fileStaleMs)
                return result; // Орион не пишет (скрипт не запущен или клиент закрыт)

            byte[] data;
            try
            {
                using (var fs = new FileStream(best, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (var ms = new MemoryStream())
                {
                    fs.CopyTo(ms);
                    data = ms.ToArray();
                }
            }
            catch
            {
                return result;
            }

            string text = Decode(data);
            if (text.IndexOf("#end", StringComparison.Ordinal) < 0)
                return result; // файл недописан — подождём следующего раза

            DateTime seen = bestTime;
            string reporter = Global.PlayerInstance?.Name ?? "";
            foreach (string raw in text.Split('\n'))
            {
                string line = raw.Trim('\r', ' ', '\t', '﻿');
                if (line.Length == 0 || line[0] == '#')
                    continue;
                RadarContact c = ParseLine(line, false);
                if (c == null)
                    continue;
                c.Seen = seen;
                c.Reporter = reporter;
                result.Add(c);
            }

            LocalCount = result.Count;
            return result;
        }

        private static string Decode(byte[] data)
        {
            try
            {
                return new UTF8Encoding(false, true).GetString(data);
            }
            catch (DecoderFallbackException)
            {
                return Encoding.GetEncoding(1251).GetString(data);
            }
        }

        // serial|x|y|map|noto|hp|name   (из файла)
        // serial|x|y|map|noto|hp|age_ms|reporter|name   (от сервера)
        public static RadarContact ParseLine(string line, bool fromServer)
        {
            string[] f = line.Split(new[] {'|'}, fromServer ? 9 : 7);
            if (f.Length < (fromServer ? 9 : 7))
                return null;
            try
            {
                var c = new RadarContact
                {
                    Serial = ParseUInt(f[0]),
                    X = int.Parse(f[1]),
                    Y = int.Parse(f[2]),
                    Map = byte.Parse(f[3]),
                    Noto = byte.Parse(f[4]),
                    Hp = int.Parse(f[5])
                };
                if (c.Noto != NOTO_GREEN && c.Noto != NOTO_ORANGE)
                    return null;
                if (c.X <= 0 || c.Y <= 0 || c.X > 7168 || c.Y > 4096)
                    return null;
                if (fromServer)
                {
                    int age = int.Parse(f[6]);
                    c.Seen = DateTime.UtcNow.AddMilliseconds(-Math.Max(0, age));
                    c.Reporter = f[7];
                    c.Name = f[8];
                }
                else
                    c.Name = f[6];

                if (c.Name.Length > 40)
                    c.Name = c.Name.Substring(0, 40);
                return c;
            }
            catch
            {
                return null;
            }
        }

        private static uint ParseUInt(string s)
        {
            s = s.Trim();
            if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
                return Convert.ToUInt32(s.Substring(2), 16);
            return uint.Parse(s);
        }

        // ---------- обмен с сервером ----------

        private static List<RadarContact> Exchange(List<RadarContact> local)
        {
            var sb = new StringBuilder();
            sb.Append("#uoplus-radar 1|").Append(Clean(Global.PlayerInstance?.Name ?? "?")).Append('\n');
            foreach (RadarContact c in local)
            {
                sb.Append("0x").Append(c.Serial.ToString("X8")).Append('|')
                    .Append(c.X).Append('|').Append(c.Y).Append('|').Append(c.Map).Append('|')
                    .Append(c.Noto).Append('|').Append(c.Hp).Append('|').Append(Clean(c.Name)).Append('\n');
            }

            byte[] body = Encoding.UTF8.GetBytes(sb.ToString());
            var req = (HttpWebRequest) WebRequest.Create(_server);
            req.Method = "POST";
            req.ContentType = "text/plain; charset=utf-8";
            req.Timeout = 4000;
            req.ReadWriteTimeout = 4000;
            req.KeepAlive = true;
            req.Headers["X-Radar-Key"] = _key;
            req.ContentLength = body.Length;
            using (Stream s = req.GetRequestStream())
                s.Write(body, 0, body.Length);

            var result = new List<RadarContact>();
            using (var resp = (HttpWebResponse) req.GetResponse())
            using (var rd = new StreamReader(resp.GetResponseStream(), Encoding.UTF8))
            {
                string line;
                while ((line = rd.ReadLine()) != null)
                {
                    if (line.Length == 0 || line[0] == '#')
                        continue;
                    RadarContact c = ParseLine(line, true);
                    if (c != null)
                        result.Add(c);
                }
            }

            return result;
        }

        private static string Clean(string s)
        {
            return (s ?? "").Replace('|', '/').Replace('\n', ' ').Replace('\r', ' ');
        }

        // ---------- отрисовка ----------

        public static void Render(Graphics g, int x, int y, int canvasW, int canvasH)
        {
            if (!_enabled)
                return;

            RadarContact[] list;
            lock (_lock)
            {
                list = new RadarContact[_contacts.Count];
                _contacts.Values.CopyTo(list, 0);
            }

            var placed = new List<RectangleF>();
            Array.Sort(list, (a, b) => b.Noto.CompareTo(a.Noto)); // рыжие подписываются первыми
            foreach (RadarContact c in list)
            {
                if (c.Map != Global.Facet)
                    continue;
                if (c.Noto == NOTO_GREEN && !_showGreen || c.Noto == NOTO_ORANGE && !_showOrange)
                    continue;
                // зелёный, который сам сидит на карте альянса, уже нарисован как пользователь
                if (c.Noto == NOTO_GREEN && c.Name.Length > 0 && RenderObjectsManager.GetUser(c.Name) != null)
                    continue;

                int gameX = c.X - (int) Global.X;
                int gameY = c.Y - (int) Global.Y;
                (gameX, gameY) = Geometry.RotatePoint(gameX, gameY, Global.Zoom, 1, Global.Angle);
                int px = gameX + x;
                int py = gameY + y;
                if (px < -50 || py < -20 || px > canvasW + 50 || py > canvasH + 20)
                    continue;

                SolidBrush color = c.Noto == NOTO_ORANGE ? _orange : _green;
                g.FillEllipse(color, px - 4, py - 4, 8, 8);
                g.DrawEllipse(_outline, px - 4, py - 4, 8, 8);

                string label = c.Name.Length > 0 ? c.Name : "?";
                if (c.Hp >= 0 && c.Hp < 100)
                    label += " " + c.Hp + "%";
                SizeF size = g.MeasureString(label, _font);
                float lx = px + 6;
                float ly = py - size.Height / 2;
                if (lx + size.Width > canvasW - 2)
                    lx = px - 6 - size.Width;
                // подписи близко стоящих не налезают друг на друга
                var rect = new RectangleF(lx - 1, ly, size.Width + 1, size.Height);
                for (int guard = 0; guard < 20 && placed.Exists(r => r.IntersectsWith(rect)); guard++)
                    rect.Y += size.Height;
                placed.Add(rect);
                ly = rect.Y;
                if (Math.Abs(ly - (py - size.Height / 2)) > 1)
                    g.DrawLine(color == _orange ? Pens.Orange : Pens.LimeGreen, px, py, lx, ly + size.Height / 2);
                g.FillRectangle(_back, rect);
                g.DrawString(label, _font, color, lx, ly);
            }
        }
    }
}
