#!/bin/sh
# rebuilds vendor/win/prefix, the mingw sysroot for the windows client. gitignored, so this
# script is the only record of it. run from anywhere.
set -e
cd "$(dirname "$0")"
rm -rf prefix && mkdir prefix

SDL=3.4.16
curl -sL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-devel-$SDL-mingw.tar.gz" \
  | tar xz --strip-components=2 -C prefix "SDL3-$SDL/x86_64-w64-mingw32"

M=https://mirror.msys2.org/mingw/mingw64
mkdir -p /tmp/mdb.$$
curl -sL "$M/mingw64.db" | zstd -dc | tar -x -C /tmp/mdb.$$
for want in freetype brotli bzip2 libpng zlib harfbuzz graphite2; do
  d=$(grep -rl -x "mingw-w64-x86_64-$want" /tmp/mdb.$$ --include=desc | while read -r f; do
        [ "$(sed -n '/%NAME%/{n;p}' "$f")" = "mingw-w64-x86_64-$want" ] && echo "$f"; done | head -1)
  f=$(sed -n '/%FILENAME%/{n;p}' "$d")
  curl -sL "$M/$f" | zstd -dc | tar -x --strip-components=1 -C prefix mingw64/include mingw64/lib mingw64/bin 2>/dev/null
done
rm -rf /tmp/mdb.$$

sed -i "s|^prefix=/mingw64|prefix=$PWD/prefix|" prefix/lib/pkgconfig/*.pc
# harfbuzz's glib dep is unresolvable here and we link the freetype dll, so private deps never apply
sed -i 's/^Requires.private:.*/Requires.private:/' prefix/lib/pkgconfig/freetype2.pc
rm -f prefix/lib/libfreetype.a

PKG_CONFIG_LIBDIR=$PWD/prefix/lib/pkgconfig pkg-config --print-errors --exists sdl3 freetype2 && echo "vendor/win ok"
