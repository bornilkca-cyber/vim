#!/bin/sh
# build-deb.sh --- build a vim-copilot Debian package
#
# Invoked by "make deb" from the top of the source tree.  Everything it needs
# is derived from the source tree and the running system; nothing is hardcoded.
#
# usage: debian-copilot/build-deb.sh [output-directory]

set -e

top=$(cd "$(dirname "$0")/.." && pwd)
outdir=${1:-$top}
srcdir=$top/src
metadir=$top/debian-copilot
stage=$srcdir/deb-root

die() { echo "build-deb.sh: $*" >&2; exit 1; }

# --- Derive the version ----------------------------------------------------
# The Debian version is MAJOR.MINOR.PATCHLEVEL+CODENAME, where PATCHLEVEL is
# the highest patch in version.c (what ":version" reports as "Included
# patches: 1-N"), not VIM_VERSION_BUILD.
major=$(sed -n 's/^#define VIM_VERSION_MAJOR[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1/p' "$srcdir/version.h")
minor=$(sed -n 's/^#define VIM_VERSION_MINOR[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1/p' "$srcdir/version.h")
patch=$(sed -n '/^static int included_patches\[\]/,/^};/p' "$srcdir/version.c" \
	| sed -n 's/^[[:space:]]*\([0-9]\{1,\}\),.*/\1/p' | head -1)
[ -n "$major" ] && [ -n "$minor" ] && [ -n "$patch" ] || die "cannot derive version"
upstream_version="$major.$minor.$patch"

# The OS tag comes from the build host, so the package name records what it
# was built against (glibc and ncurses sonames are not portable across them).
if [ -r /etc/os-release ]; then
	# shellcheck disable=SC1091
	. /etc/os-release
	os=${VERSION_CODENAME:-${ID:-unknown}}
else
	os=unknown
fi
version="$upstream_version+$os"

arch=$(dpkg --print-architecture)
maintainer=${DEBEMAIL:-"vim-copilot <root@$(hostname)>"}

echo "==> vim-copilot $version ($arch)"

# --- Check the tree is configured the way the package needs ----------------
mk=$srcdir/auto/config.mk
[ -r "$mk" ] || die "src/auto/config.mk missing; run 'make deb-configure' first"
cfg_name=$(sed -n 's/^VIMNAME[[:space:]]*=[[:space:]]*//p' "$mk")
cfg_prefix=$(sed -n 's/^prefix[[:space:]]*=[[:space:]]*//p' "$mk")
[ "$cfg_name" = "vim-copilot" ] || die "configured VIMNAME is '$cfg_name'; run 'make deb-configure'"
[ "$cfg_prefix" = "/usr" ] || die "configured prefix is '$cfg_prefix'; run 'make deb-configure'"

# --- Build and stage -------------------------------------------------------
# "installvim" deliberately replaces "install": it skips installtools (which
# would install a hardcoded /usr/bin/xxd, owned by the xxd package) and
# install-icons (hardcoded vim.desktop/gvim.png, owned by vim-common and
# vim-gui-common).
echo "==> building"
( cd "$srcdir" && make )
echo "==> staging"
rm -rf "$stage"
( cd "$srcdir" && make installvim DESTDIR="$stage" >/dev/null )

[ -x "$stage/usr/bin/vim-copilot" ] || die "staging produced no /usr/bin/vim-copilot"

# --- Dependencies ----------------------------------------------------------
# dpkg-shlibdeps (from dpkg-dev) is preferred.  Without it, resolve each ELF's
# NEEDED sonames to owning packages via dpkg -S.
echo "==> resolving dependencies"
elves=$(find "$stage" -type f -perm -u+x -exec sh -c 'head -c4 "$1" | grep -q ELF' _ {} \; -print)
depends=
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
	mkdir -p "$stage/debian"
	: > "$stage/debian/control"
	# shellcheck disable=SC2086
	( cd "$stage" && dpkg-shlibdeps -O --ignore-missing-info $elves ) \
		> "$stage/.shlibdeps" 2>/dev/null || true
	depends=$(sed -n 's/^shlibs:Depends=//p' "$stage/.shlibdeps")
	rm -rf "$stage/debian" "$stage/.shlibdeps"
fi
if [ -z "$depends" ]; then
	echo "    dpkg-shlibdeps unavailable, resolving sonames with dpkg -S"
	depends=$(for f in $elves; do
			ldd "$f" 2>/dev/null | sed -n 's/^[[:space:]]*[^ ]* => \([^ ]*\).*/\1/p'
		done | sort -u | while read -r lib; do
			[ -e "$lib" ] || continue
			dpkg -S "$(readlink -f "$lib")" 2>/dev/null | cut -d: -f1
		done | sort -u | paste -sd, - | sed 's/,/, /g')
fi
[ -n "$depends" ] || die "could not determine dependencies"
echo "    Depends: $depends"

# --- Control file ----------------------------------------------------------
installed_size=$(du -sk "$stage" | cut -f1)
mkdir -p "$stage/DEBIAN"
sed -e "s|@VERSION@|$version|" \
    -e "s|@UPSTREAM_VERSION@|$upstream_version|" \
    -e "s|@ARCH@|$arch|" \
    -e "s|@MAINTAINER@|$maintainer|" \
    -e "s|@INSTALLED_SIZE@|$installed_size|" \
    -e "s|@DEPENDS@|$depends|" \
    "$metadir/control.in" > "$stage/DEBIAN/control"

docdir=$stage/usr/share/doc/vim-copilot
mkdir -p "$docdir"
cp "$metadir/copyright" "$docdir/copyright"
[ -r "$top/README.md" ] && cp "$top/README.md" "$docdir/README.md"
printf 'vim-copilot (%s) %s; urgency=low\n\n  * Vim %s with native GitHub Copilot support.\n\n -- %s  %s\n' \
	"$version" "$os" "$upstream_version" "$maintainer" "$(date -R)" \
	| gzip -9n > "$docdir/changelog.gz"

# md5sums must not include the control area itself.
( cd "$stage" && find . -type f ! -path './DEBIAN/*' -printf '%P\0' \
	| xargs -0 md5sum > DEBIAN/md5sums )

# --- Build the archive -----------------------------------------------------
deb="$outdir/vim-copilot_${version}_${arch}.deb"
echo "==> packing (this takes a moment, the language server is large)"
dpkg-deb --root-owner-group -Zxz -z6 --build "$stage" "$deb" >/dev/null

echo "==> $deb"
ls -lh "$deb" | awk '{print "    size: " $5}'
