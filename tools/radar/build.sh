#!/bin/sh
# Сборка EnhancedMap с UO+ радаром на Linux (mono + Roslyn из github.com/mono/roslyn-binaries).
# Результат: out/EnhancedMap.exe (+ Aga.Controls.dll, System.ValueTuple.dll).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
W=${WORK:-$HERE/.work}
mkdir -p "$W" && cd "$W"
[ -d EnhancedMap ] || git clone -q https://github.com/andreakarasho/EnhancedMap.git
[ -d roslyn-binaries ] || git clone -q --depth 1 https://github.com/mono/roslyn-binaries.git
cd EnhancedMap
git checkout -q -f efaa391 && git clean -qfd
sed -i '1s/^\xEF\xBB\xBF//' Core/RenderObjectsManager.cs GUI/MainWindow.cs EnhancedMap.csproj Main.cs
patch -p1 --binary < "$HERE/enhancedmap.patch"
cp "$HERE/Radar.cs" Core/Radar.cs
mkdir -p ../Output ../packages/System.ValueTuple.4.4.0/lib/net461
cp libs/Aga.Controls.dll ../Output/
cp libs/System.ValueTuple.dll ../packages/System.ValueTuple.4.4.0/lib/net461/
R="$W/roslyn-binaries/Microsoft.Net.Compilers/3.9.0"
xbuild /nologo /v:quiet /p:Configuration=Release /p:CscToolPath="$R" /p:CscToolExe=csc.exe EnhancedMap.csproj
mkdir -p "$HERE/out"
cp ../Release/EnhancedMap.exe ../Release/Aga.Controls.dll ../Release/System.ValueTuple.dll "$HERE/out/"
echo "Готово: $HERE/out/EnhancedMap.exe"
