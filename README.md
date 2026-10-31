# UO+ — клиент для шарда Age of Power

UO+ — дополнение к старому клиенту Ultima Online 3.0 (`zlib1.dll`) для шарда [Age of Power](https://aop-online.net/):
своя сетка контейнеров, карта мира с радаром, окно лута, горячие клавиши и команды в стиле Injection, наборы
`arm`/`dress`, встроенные скрипты.

## Обновления

Лаунчер `UOPlus_Play.exe` (исходник в [`launcher/`](launcher)) при запуске читает [`manifest.json`](manifest.json) из этого репозитория: в нём номер версии и список файлов
с размером и контрольной суммой SHA-256. Лаунчер сравнивает их с файлами у игрока и скачивает только то, что
изменилось. Сами файлы последней версии лежат в папке [`files/`](files).

- `path` — куда положить файл относительно папки клиента (где `Client+inj_multi_*.exe`). Если в `uoplus.ini`
  в разделе `[files]` файл перенаправлен (например `verdata.mul=...`), лаунчер обновляет тот, что игра реально читает;
- `only_if_ours` — обновлять, только если там уже UO+ (так `zlib1.dll` Injection-а не тронется);
- `url` — откуда скачать;
- `sha256` — проверка, что файл скачан целиком и не подменён.

## Инструменты

- `tools/verdata_gumps.py` — разобрать verdata/patchdata на картинки гампов (PNG) и собрать обратно.
- `tools/uoplus_grid_gen.py` — собрать verdata UO+ с картинками сетки.

В репозитории нет и не должно быть логинов, паролей, `injection.xml`, `uo.cfg` и других личных настроек.

## Лаунчер

`UOPlus_Play.exe` в папке клиента: проверяет обновления, докачивает изменённое (прошлые файлы — рядом с `.old`),
кнопка «Играть» запускает клиент в размере окна из `uoplus.ini` (`[launch] size`). `UOPlus_Play.exe /play` — сразу играть
без окна. Журнал — `uoplus_launcher.log`. Сборка: `i686-w64-mingw32-windres play.rc -O coff -o play_res.o` и
`i686-w64-mingw32-g++ -std=c++17 -O2 -municode -mwindows -static -o UOPlus_Play.exe play.cpp play_res.o -lwinhttp -lbcrypt -lcomctl32 -lshell32 -ladvapi32 -lgdi32 -luser32`.

## Новая версия

1. Положить новые файлы в `files/`, 2. пересчитать `manifest.json` (размер, sha256) и поднять `version`, 3. запушить.
