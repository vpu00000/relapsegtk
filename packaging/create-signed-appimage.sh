#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
if [[ "${1:-}" == "--help" ]]; then
    cat <<'EOF'
Usage: packaging/create-signed-appimage.sh

Build, sign, and verify an updater-enabled AppImage using packaging/publish.env.
No GitHub release is created and nothing is uploaded.

Required environment:
  GNUPGHOME           GPG home containing the release key
  GITHUB_REPOSITORY   Update repository in owner/repository form

Optional environment:
  SIGNING_KEY         Key User ID; defaults to relapsegtksigkey
  RELEASE_TAG         Version tag; defaults to the exact tag on HEAD
  LINUXDEPLOY         Existing linuxdeploy AppImage to use instead of downloading it

Output:
  Build/Release/RelapseGtk-x86_64.AppImage
  Build/Release/RelapseGtk-x86_64.AppImage.sig
EOF
    exit 0
fi
if (( $# )); then
    echo "Usage: packaging/create-signed-appimage.sh [--help]" >&2
    exit 2
fi
ENV_FILE="${RELAPSEGTK_RELEASE_ENV:-$SCRIPT_DIR/publish.env}"
if [[ ! -f "$ENV_FILE" ]]; then
    echo "Create $SCRIPT_DIR/publish.env from publish.env.example first" >&2
    exit 1
fi
requested_release_tag="${RELEASE_TAG:-}"
set -a
source "$ENV_FILE"
set +a
RELEASE_TAG="${requested_release_tag:-${RELEASE_TAG:-}}"
: "${GNUPGHOME:?Set GNUPGHOME in publish.env}"
: "${GITHUB_REPOSITORY:?Set GITHUB_REPOSITORY in publish.env}"
[[ "$GITHUB_REPOSITORY" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || {
    echo "GITHUB_REPOSITORY must use owner/repository form" >&2
    exit 1
}
SIGNING_KEY="${SIGNING_KEY:-relapsegtksigkey}"
if [[ -z "${RELEASE_TAG:-}" ]]; then
    RELEASE_TAG="$(git -C "$ROOT_DIR" describe --tags --exact-match HEAD)" || {
        echo "Tag HEAD or set RELEASE_TAG for a local build" >&2
        exit 1
    }
fi
VERSION="${RELEASE_TAG#v}"
[[ "$VERSION" =~ ^[A-Za-z0-9._-]+$ ]] || {
    echo "RELEASE_TAG contains unsupported characters" >&2
    exit 1
}
if release_commit="$(git -C "$ROOT_DIR" rev-parse --verify --quiet "refs/tags/$RELEASE_TAG^{commit}")"; then
    if [[ "$release_commit" != "$(git -C "$ROOT_DIR" rev-parse HEAD)" ]]; then
        printf 'RELEASE_TAG %s does not point to HEAD. Remove the stale RELEASE_TAG from %s or check out that tag.\n' \
            "$RELEASE_TAG" "$ENV_FILE" >&2
        exit 1
    fi
fi
export GNUPGHOME
PUBLIC_KEY="$GNUPGHOME/relapsegtk-release.gpg"
BUILD_DIR="$ROOT_DIR/Build/Release"
WORK_DIR="$BUILD_DIR/appimage"
TOOLS_DIR="$WORK_DIR/tools"
APPIMAGE="$BUILD_DIR/RelapseGtk-x86_64.AppImage"
ARCH=x86_64

for tool in cmake git gpg gpgv curl python3 sha256sum pkg-config glib-compile-schemas; do
    command -v "$tool" >/dev/null || { echo "Missing packaging tool: $tool" >&2; exit 1; }
done
mapfile -t keys < <(gpg --list-secret-keys --with-colons "$SIGNING_KEY" 2>/dev/null |
    awk -F: '$1 == "fpr" { print $10 }')
if (( ${#keys[@]} != 1 )); then
    echo "Expected exactly one secret key matching $SIGNING_KEY; run the key-creation commands in packaging/README.md first" >&2
    exit 1
fi
mkdir -p "$BUILD_DIR" "$TOOLS_DIR" "$WORK_DIR/tmp"
gpg --export "$SIGNING_KEY" > "$PUBLIC_KEY.tmp"
mv -- "$PUBLIC_KEY.tmp" "$PUBLIC_KEY"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DRELAPSEGTK_ENABLE_UPDATER=ON \
    "-DRELAPSEGTK_VERSION=$VERSION" \
    "-DRELAPSEGTK_UPDATE_REPOSITORY=$GITHUB_REPOSITORY" \
    "-DRELAPSEGTK_UPDATE_PUBLIC_KEY=$PUBLIC_KEY"
cmake --build "$BUILD_DIR" --parallel "$(nproc)"
export TMPDIR="$WORK_DIR/tmp"

download_asset() {
    local repository="$1" filename="$2"
    local target="$TOOLS_DIR/$filename"
    local -a asset
    if ! (cd "$TOOLS_DIR" && sha256sum --check --status "$filename.sha256" 2>/dev/null); then
        curl --fail --location --retry 3 --silent --show-error \
            "https://api.github.com/repos/$repository/releases/tags/continuous" \
            -o "$TOOLS_DIR/release.json"
        readarray -t asset < <(python3 - "$TOOLS_DIR/release.json" "$filename" <<'PY'
import json, sys
release = json.load(open(sys.argv[1]))
asset = next(a for a in release['assets'] if a['name'] == sys.argv[2])
digest = asset['digest']
if not digest or not digest.startswith('sha256:'):
    raise SystemExit('The release has no SHA-256 digest')
print(asset['browser_download_url'])
print(digest.removeprefix('sha256:'))
PY
        )
        [[ ${#asset[@]} -eq 2 ]] || { echo "Could not find release metadata for $filename" >&2; exit 1; }
        curl --fail --location --retry 3 --silent --show-error "${asset[0]}" -o "$target.part"
        printf '%s  %s\n' "${asset[1]}" "$target.part" | sha256sum --check
        mv -- "$target.part" "$target"
        printf '%s  %s\n' "${asset[1]}" "$filename" > "$target.sha256"
    fi
}
if [[ -z "${LINUXDEPLOY:-}" ]]; then
    download_asset linuxdeploy/linuxdeploy "linuxdeploy-$ARCH.AppImage"
    LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-$ARCH.AppImage"
    chmod +x "$LINUXDEPLOY"
fi
LINUXDEPLOY="$(realpath "$LINUXDEPLOY")"
download_asset AppImage/type2-runtime "runtime-$ARCH"
export LDAI_RUNTIME_FILE="$TOOLS_DIR/runtime-$ARCH"

APPDIR="$(mktemp -d "$WORK_DIR/AppDir.XXXXXX")"
trap 'rm -rf -- "$APPDIR"' EXIT
mkdir -p "$APPDIR/usr/lib/gio/modules" "$APPDIR/usr/share/glib-2.0/schemas" \
    "$APPDIR/usr/share/doc/relapsegtk"
cp "$ROOT_DIR/LICENSE" "$APPDIR/usr/share/doc/relapsegtk/LICENSE"

gtk_prefix="$(pkg-config --variable=prefix gtk4)"
gio_libdir="$(pkg-config --variable=libdir gio-2.0)"
schema_dir="$(pkg-config --variable=schemasdir gio-2.0)"
pixbuf_modules="$(pkg-config --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0)"
pixbuf_query="$(pkg-config --variable=gdk_pixbuf_query_loaders gdk-pixbuf-2.0)"
svg_loader="$pixbuf_modules/libpixbufloader_svg.so"
[[ -f "$svg_loader" && -x "$pixbuf_query" ]] || {
    echo 'Install the librsvg GdkPixbuf loader and gdk-pixbuf-query-loaders to package SVG icons.' >&2
    exit 1
}

shopt -s nullglob
schemas=("$schema_dir"/org.gtk.gtk4.*.gschema.xml)
[[ ${#schemas[@]} -gt 0 ]] || { echo "GTK 4 schemas not found in $schema_dir" >&2; exit 1; }
cp "${schemas[@]}" "$APPDIR/usr/share/glib-2.0/schemas/"
glib-compile-schemas "$APPDIR/usr/share/glib-2.0/schemas"
for directory in gtk-4.0 gtksourceview-5 icons/Adwaita; do
    if [[ -d "$gtk_prefix/share/$directory" ]]; then
        mkdir -p "$APPDIR/usr/share/$(dirname "$directory")"
        cp -a "$gtk_prefix/share/$directory" "$APPDIR/usr/share/$directory"
    fi
done
mkdir -p "$APPDIR/usr/share/icons/hicolor"
cp "$gtk_prefix/share/icons/hicolor/index.theme" "$APPDIR/usr/share/icons/hicolor/"
if [[ -f "$gio_libdir/gio/modules/libdconfsettings.so" ]]; then
    cp -L "$gio_libdir/gio/modules/libdconfsettings.so" "$APPDIR/usr/lib/gio/modules/"
fi

libraries=(--library "$svg_loader")
ldd "$BUILD_DIR/relapsegtk" "$svg_loader" "$pixbuf_query" \
    "$APPDIR/usr/lib/gio/modules/"*.so > "$WORK_DIR/dependencies.txt"
python3 - "$WORK_DIR/dependencies.txt" > "$WORK_DIR/libraries.txt" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
if 'not found' in text:
    raise SystemExit('An application dependency could not be resolved:\n' + text)
print('\n'.join(sorted(set(re.findall(r'=> (/[^\n]+?) \(0x', text)))))
PY
while IFS= read -r library; do
    case "$(basename "$library")" in
        libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libresolv.so.*|ld-linux*.so.*|\
        libGL.so.*|libGLX.so.*|libGLdispatch.so.*|libEGL.so.*|libdrm*.so.*|libgbm.so.*) continue ;;
    esac
    libraries+=(--library "$library")
done < "$WORK_DIR/libraries.txt"
export APPIMAGE_EXTRACT_AND_RUN=1
"$LINUXDEPLOY" --appdir "$APPDIR" \
    --executable "$BUILD_DIR/relapsegtk" --executable "$pixbuf_query" \
    "${libraries[@]}" --deploy-deps-only "$APPDIR/usr/lib/gio/modules" \
    --desktop-file "$ROOT_DIR/packaging/appimage/org.relapse.RelapseGtk.desktop" \
    --icon-file "$ROOT_DIR/resources/icons/relapse.svg" --icon-filename org.relapse.RelapseGtk \
    --custom-apprun "$ROOT_DIR/packaging/appimage/AppRun"
ln -s "usr/share/icons/hicolor/scalable/apps/org.relapse.RelapseGtk.svg" "$APPDIR/.DirIcon"
chmod -R a+rX "$APPDIR"

export LDAI_OUTPUT="$APPIMAGE" ARCH
"$LINUXDEPLOY" --appdir "$APPDIR" --output appimage
gpg --batch --yes --local-user "$SIGNING_KEY" \
    --output "$APPIMAGE.sig" --detach-sign "$APPIMAGE"
gpgv --keyring "$PUBLIC_KEY" "$APPIMAGE.sig" "$APPIMAGE"
printf '\nVersion: %s\nRepository: %s\nAppImage: %s\nSignature: %s.sig\nNothing was published.\n' \
    "$VERSION" "$GITHUB_REPOSITORY" "$APPIMAGE" "$APPIMAGE"
