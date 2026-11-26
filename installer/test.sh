# installer/test.sh <setup.exe> : installs to a scratch folder, checks, installs over it, removes. No registry, no shortcuts (/test).
E=$(readlink -f "$1"); T="${TMP:-/tmp}/peussok-setup-test"; D="$T/한글 폴더/Peussok"; S=$(dirname "$E")/$(basename "$E" | sed -E 's/Setup-([0-9.]+[0-9]).*/\1/'); export MSYS2_ARG_CONV_EXCL='*'
rm -rf "$T"; mkdir -p "$T"; W=$(cygpath -w "$D"); ok() { [ "$2" = "$3" ] && echo "ok   $1" || { echo "FAIL $1: $2 / $3"; F=1; }; }
"$E" /S /test "/D=$W"; ok install $? 0
n=$(cd "$S"; find . -type f ! -name '*.log' | wc -l); m=$(find "$D" -type f | wc -l); ok "files ($n + list + remover)" $((n+2)) $m
(cd "$S"; find . -type f ! -name '*.log' -exec sha256sum {} +) | sort > "$T/a"; (cd "$D"; find . -type f ! -name installed.txt ! -name Uninstall.exe -exec sha256sum {} +) | sort > "$T/b"; diff -q "$T/a" "$T/b" >/dev/null; ok "same files" $? 0
"$E" /S /test "/D=$W"; ok "install over" $? 0
echo mine > "$D/mine.txt"; printf x > "$D/engine/engines/dlssg/nvngx.log"
"$D/Uninstall.exe" /S /test; sleep 5
ok "own file kept" "$(ls "$D" 2>/dev/null | tr '\n' ' ')" "mine.txt "
"$E" /S /test "/D=$W"; ok "folder with other files refused" $? 1
rm -f "$D/mine.txt"; "$E" /S /test "/D=$W"; "$D/Uninstall.exe" /S /test; sleep 5; [ -e "$D" ]; ok "all gone" $? 1
ls /c/Users/*/AppData/Local/Temp/Peussok-remove-* 2>/dev/null | wc -l | sed 's/^/left in temp: /'
"$E" "/shot=$(cygpath -w "$T/ko.png")" /ko; "$E" "/shot=$(cygpath -w "$T/en.png")" /en
echo "${F:-0} failures"
