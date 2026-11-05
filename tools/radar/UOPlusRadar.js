// UO+ радар для EnhancedMap (Orion Assistant 2.0).
// Раз в секунду записывает видимых зелёных и рыжих (имя, координаты, HP) в файл UOPlusRadar.txt.
// Доработанный EnhancedMap читает этот файл, рисует их на карте и делится ими с альянсом.
// Запуск: вкладка Scripts в Орионе → загрузить этот файл → запустить функцию Radar.

var RADAR_FILE = 'UOPlusRadar.txt'; // относительно папки Ориона; EnhancedMap ищет его там сам
var RADAR_RANGE = 24;               // дальше клиент всё равно не видит
var RADAR_DELAY = 1000;             // мс между записями

function RadarCollect(noto, code, me, out, asked) {
	var list = Orion.FindType('0xFFFF', '0xFFFF', 'ground', 'live', RADAR_RANGE, noto);
	if (!list)
		return;
	for (var i = 0; i < list.length; i++) {
		var s = list[i];
		if (s == me)
			continue;
		var o = Orion.FindObject(s);
		if (!o)
			continue;
		var name = o.Name();
		if (!name) {
			if (!asked[s]) {
				asked[s] = 1;
				Orion.RequestName(s, 50);
				name = o.Name();
			}
			if (!name)
				name = '';
		}
		var hp = -1;
		var mx = o.MaxHits();
		if (mx > 0)
			hp = Math.round(o.Hits() * 100 / mx);
		name = String(name).replace(/\|/g, '/');
		out.push(s + '|' + o.X() + '|' + o.Y() + '|' + o.Map() + '|' + code + '|' + hp + '|' + name);
	}
}

function Radar() {
	var asked = {};
	var me = Player.Serial();
	var f = Orion.NewFile();
	Orion.Print('0x3F', 'UO+ радар запущен, файл: ' + RADAR_FILE);
	while (true) {
		var out = [];
		RadarCollect('green', 2, me, out, asked);
		RadarCollect('orange', 5, me, out, asked);

		f.Remove(RADAR_FILE);
		if (f.Open(RADAR_FILE)) {
			f.WriteLine('#uoplus-radar 1');
			for (var i = 0; i < out.length; i++)
				f.WriteLine(out[i]);
			f.WriteLine('#end');
			f.Close();
		}
		Orion.Wait(RADAR_DELAY);
	}
}
