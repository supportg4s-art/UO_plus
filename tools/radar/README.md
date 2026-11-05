# UO+ радар для EnhancedMap

Показывает на карте EnhancedMap зелёных (свои) и рыжих (гильдии, с которыми война) с именами и HP.
Кого видит любой участник альянса, видят все, у кого стоит эта сборка.

Как устроено:

```
Орион + UOPlusRadar.js ──файл UOPlusRadar.txt──> EnhancedMap (UO+) ──HTTP──> radar_server.py
                                                         <── все, кого видел альянс ──
```

- `UOPlusRadar.js` — скрипт Orion Assistant. Раз в секунду пишет видимых зелёных и рыжих в `UOPlusRadar.txt`
  в папке Ориона.
- `Radar.cs` + `enhancedmap.patch` — доработка EnhancedMap (andreakarasho/EnhancedMap, коммит efaa391):
  читает файл, шлёт на радар-сервер, рисует всех. Автообновление EnhancedMap выключено, чтобы оно
  не заменило сборку обычной. Обычный сервер карты альянса работает как раньше.
- `radar_server.py` — радар-сервер, Python 3.7+, только стандартная библиотека.
- `build.sh` — сборка на Linux (mono, xbuild, Roslyn из mono/roslyn-binaries).

## Сервер

```
python3 radar_server.py --port 8899 --key ОБЩИЙ_КЛЮЧ
curl http://127.0.0.1:8899/health      # сколько видно и кто присылает
```

Порт 8899/TCP должен быть открыт снаружи. Точка пропадает через 10 секунд, если её никто не видит.

## Игроку

1. Закрыть EnhancedMap. В его папке заменить `EnhancedMap.exe` на наш (старый лучше сохранить),
   положить рядом `Radar.ini`. Настройки и карты остаются свои.
2. В Орионе: Scripts → загрузить `UOPlusRadar.js` → запустить функцию `Radar`.
   В журнале появится «UO+ радар запущен».
3. Запустить EnhancedMap как обычно.

`Radar.ini`:

```
[radar]
enabled=1
server=http://АДРЕС:8899/radar
key=ОБЩИЙ_КЛЮЧ
file=            ; пусто — искать UOPlusRadar.txt в папке Ориона
show_green=1
show_orange=1
```

Если EnhancedMap не находит файл (Орион пишет его в другое место), путь к файлу указывают в `file=`.
