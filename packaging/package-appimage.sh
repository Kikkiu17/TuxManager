#!/bin/bash
################################################################################
# package-appimage.sh - Build and package Tux Manager as an AppImage
################################################################################

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/config"

PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="$PROJECT_ROOT/packaging/output"
BUILD_DIR="$PROJECT_ROOT/packaging/.appimage-build"
APPDIR="$PROJECT_ROOT/packaging/.AppDir"
DESKTOP_FILE_SRC="$PROJECT_ROOT/packaging/data/io.github.benapetr.TuxManager.desktop"
ICON_FILE_SRC="$PROJECT_ROOT/src/tux_manager_icon.svg"
METADATA_FILE_SRC="$PROJECT_ROOT/packaging/flatpak/io.github.benapetr.TuxManager.metainfo.xml"

QT_BIN_PATH=""
NCPUS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)

while [[ $# -gt 0 ]]; do
    case "$1" in
        --qt)
            if [ $# -lt 2 ] || [ -z "$2" ]; then
                echo "Error: --qt requires a directory path."
                exit 1
            fi
            QT_BIN_PATH="$2"
            shift 2
            ;;
        --version)
            if [ $# -lt 2 ] || [ -z "$2" ]; then
                echo "Error: --version requires a version string."
                exit 1
            fi
            APP_VERSION="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--qt /path/to/qt/bin] [--version x.y.z]"
            exit 1
            ;;
    esac
done

echo "================================="
echo "Building Tux Manager AppImage"
echo "================================="
echo "CPUs: $NCPUS"
echo "Version: $APP_VERSION"
echo ""

if [ -n "$QT_BIN_PATH" ]; then
    if [ ! -d "$QT_BIN_PATH" ]; then
        echo "Error: Specified Qt bin directory does not exist: $QT_BIN_PATH"
        exit 1
    fi
    QT_BIN_PATH="$(cd "$QT_BIN_PATH" && pwd)"
    export PATH="$QT_BIN_PATH:$PATH"
    if [ -x "$QT_BIN_PATH/qmake6" ]; then
        QMAKE_CMD="$QT_BIN_PATH/qmake6"
    elif [ -x "$QT_BIN_PATH/qmake" ]; then
        QMAKE_CMD="$QT_BIN_PATH/qmake"
    else
        echo "Error: qmake not found in specified Qt path: $QT_BIN_PATH"
        exit 1
    fi
else
    if command -v qmake6 >/dev/null 2>&1; then
        QMAKE_CMD="$(command -v qmake6)"
    elif command -v qmake >/dev/null 2>&1; then
        QMAKE_CMD="$(command -v qmake)"
    else
        echo "Error: qmake not found in PATH."
        echo "Install Qt development tools or use --qt to specify Qt bin path."
        exit 1
    fi
fi

QT_HOST_BINS_DIR="$("$QMAKE_CMD" -query QT_HOST_BINS 2>/dev/null || true)"
QT_INSTALL_BINS_DIR="$("$QMAKE_CMD" -query QT_INSTALL_BINS 2>/dev/null || true)"
QT_TOOLS_BIN=""
for candidate_dir in "$QT_BIN_PATH" "$QT_HOST_BINS_DIR" "$QT_INSTALL_BINS_DIR"; do
    if [ -n "$candidate_dir" ] && [ -x "$candidate_dir/lrelease" ]; then
        QT_TOOLS_BIN="$candidate_dir"
        break
    fi
done
if [ -z "$QT_TOOLS_BIN" ]; then
    echo "Error: Qt Linguist lrelease not found for $QMAKE_CMD."
    echo "Install the matching Qt Linguist tools package or include it in --qt."
    exit 1
fi
export PATH="$QT_TOOLS_BIN:$PATH"

if ! command -v linuxdeploy >/dev/null 2>&1; then
    echo "Error: linuxdeploy not found in PATH."
    echo "Install linuxdeploy and re-run."
    exit 1
fi

if ! command -v linuxdeploy-plugin-qt >/dev/null 2>&1; then
    echo "Error: linuxdeploy-plugin-qt not found in PATH."
    echo "Install linuxdeploy-plugin-qt and re-run."
    exit 1
fi

if ! command -v linuxdeploy-plugin-appimage >/dev/null 2>&1; then
    echo "Error: linuxdeploy-plugin-appimage not found in PATH."
    echo "Install linuxdeploy-plugin-appimage and re-run."
    exit 1
fi

if [ ! -f "$DESKTOP_FILE_SRC" ]; then
    echo "Error: Desktop file not found: $DESKTOP_FILE_SRC"
    exit 1
fi

if [ ! -f "$ICON_FILE_SRC" ]; then
    echo "Error: Icon file not found: $ICON_FILE_SRC"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
rm -rf "$BUILD_DIR" "$APPDIR"
mkdir -p "$BUILD_DIR" \
         "$APPDIR/usr/bin" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/scalable/apps" \
         "$APPDIR/usr/share/metainfo"

echo "Step 1: Building application..."
cd "$BUILD_DIR"
"$QMAKE_CMD" "$PROJECT_ROOT/src/TuxManager.pro" "TUX_MANAGER_VERSION=$APP_VERSION"
make -j"$NCPUS"

echo ""
echo "Step 2: Creating AppDir..."
install -Dm755 "$BUILD_DIR/$APP_NAME" "$APPDIR/usr/bin/$APP_NAME"
install -Dm644 "$DESKTOP_FILE_SRC" "$APPDIR/usr/share/applications/io.github.benapetr.TuxManager.desktop"
install -Dm644 "$ICON_FILE_SRC" "$APPDIR/usr/share/icons/hicolor/scalable/apps/tux_manager_icon.svg"
if [ -f "$METADATA_FILE_SRC" ]; then
    install -Dm644 "$METADATA_FILE_SRC" "$APPDIR/usr/share/metainfo/io.github.benapetr.TuxManager.metainfo.xml"
fi

ln -sf usr/share/applications/io.github.benapetr.TuxManager.desktop "$APPDIR/io.github.benapetr.TuxManager.desktop"
ln -sf usr/share/icons/hicolor/scalable/apps/tux_manager_icon.svg "$APPDIR/tux_manager_icon.svg"

echo ""
echo "Step 3: Bundling AppImage..."
export QMAKE
QMAKE="$(command -v "$QMAKE_CMD")"

QT_LIBS_DIR="$("$QMAKE_CMD" -query QT_INSTALL_LIBS 2>/dev/null || true)"
QT_PLUGINS_DIR="$("$QMAKE_CMD" -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
if [ -n "$QT_LIBS_DIR" ] && [ -d "$QT_LIBS_DIR" ]; then
    export LD_LIBRARY_PATH="$QT_LIBS_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
if [ -n "$QT_PLUGINS_DIR" ] && [ -d "$QT_PLUGINS_DIR" ]; then
    export QT_PLUGIN_PATH="$QT_PLUGINS_DIR"
fi

# The application does not link Qt Svg, so linuxdeploy-plugin-qt would not bundle the SVG
# icon engine. Without it QIcon::fromTheme() silently skips every SVG icon of the host icon
# theme (Breeze, Adwaita and most others are SVG only).
case ";${EXTRA_QT_MODULES:-};" in
    *";svg;"*) ;;
    *) export EXTRA_QT_MODULES="${EXTRA_QT_MODULES:+$EXTRA_QT_MODULES;}svg" ;;
esac

# Bundle native Wayland platform plugins alongside XCB when present in the selected Qt
# installation so the AppImage runs natively on Wayland compositors (including Plasma 6).
WAYLAND_PLATFORM_PLUGINS=()
if [ -n "$QT_PLUGINS_DIR" ]; then
    for plugin in libqwayland-egl.so libqwayland-generic.so libqwayland.so; do
        if [ -f "$QT_PLUGINS_DIR/platforms/$plugin" ]; then
            WAYLAND_PLATFORM_PLUGINS+=("$plugin")
        fi
    done
fi
if [ ${#WAYLAND_PLATFORM_PLUGINS[@]} -gt 0 ]; then
    for plugin in "${WAYLAND_PLATFORM_PLUGINS[@]}"; do
        case ";${EXTRA_PLATFORM_PLUGINS:-};" in
            *";$plugin;"*) ;;
            *) export EXTRA_PLATFORM_PLUGINS="${EXTRA_PLATFORM_PLUGINS:+$EXTRA_PLATFORM_PLUGINS;}$plugin" ;;
        esac
    done

    # PlatformPluginsDeployer deploys wayland-decoration-client and wayland-shell-integration
    # for libqwayland*, while waylandcompositor also deploys wayland-graphics-integration-client
    # (needed for libqt-plugin-wayland-egl.so).
    if [ -d "$QT_PLUGINS_DIR/wayland-graphics-integration-client" ]; then
        case ";${EXTRA_QT_MODULES:-};" in
            *";waylandcompositor;"*) ;;
            *) export EXTRA_QT_MODULES="${EXTRA_QT_MODULES:+$EXTRA_QT_MODULES;}waylandcompositor" ;;
        esac
    fi
fi

# The strip binary shipped inside linuxdeploy predates .relr.dyn sections produced by
# current binutils and aborts the build on such libraries.
export NO_STRIP="${NO_STRIP:-true}"

ARCH="$(uname -m)"
OUTPUT_FILENAME="${APP_NAME}-${APP_VERSION}-${ARCH}.AppImage"
OUTPUT_PATH="$OUTPUT_DIR/$OUTPUT_FILENAME"

rm -f "$OUTPUT_PATH" "${OUTPUT_PATH}.zsync" "${OUTPUT_PATH}.sha256" \
      "$PROJECT_ROOT"/*.AppImage "$PROJECT_ROOT"/*.AppImage.zsync

export ARCH
export LINUXDEPLOY_OUTPUT_VERSION="$APP_VERSION"
export LDAI_OUTPUT="$OUTPUT_PATH"
if command -v zsyncmake >/dev/null 2>&1 || [ "${APPIMAGE_EMBED_UPDATE_INFO:-0}" = "1" ] || [ -n "${LDAI_UPDATE_INFORMATION:-}" ]; then
    export LDAI_UPDATE_INFORMATION="${LDAI_UPDATE_INFORMATION:-gh-releases-zsync|${APPIMAGE_UPDATE_OWNER:-Kikkiu17}|${APPIMAGE_UPDATE_REPO:-TuxManager}|latest|${APP_NAME}-*-${ARCH}.AppImage.zsync}"
fi

# Run inside $OUTPUT_DIR so linuxdeploy-plugin-appimage and zsyncmake write both the
# .AppImage and .AppImage.zsync directly in $OUTPUT_DIR without post-build renaming.
cd "$OUTPUT_DIR"

linuxdeploy \
    --appdir "$APPDIR" \
    --desktop-file "$APPDIR/usr/share/applications/io.github.benapetr.TuxManager.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/tux_manager_icon.svg" \
    --plugin qt \
    --output appimage

if [ ! -f "$OUTPUT_PATH" ]; then
    mapfile -t APPIMAGE_FILES < <(find "$OUTPUT_DIR" "$PROJECT_ROOT" -maxdepth 1 -type f -name "*.AppImage" -newer "$BUILD_DIR/$APP_NAME" | sort) || true
    if [ ${#APPIMAGE_FILES[@]} -eq 0 ]; then
        echo "Error: linuxdeploy did not produce an AppImage."
        exit 1
    fi
    mv "${APPIMAGE_FILES[0]}" "$OUTPUT_PATH"
    rm -f "${APPIMAGE_FILES[0]}.zsync"
fi

if [ -n "${LDAI_UPDATE_INFORMATION:-}" ] && command -v zsyncmake >/dev/null 2>&1; then
    if [ ! -f "${OUTPUT_PATH}.zsync" ] && [ -f "$PROJECT_ROOT/${OUTPUT_FILENAME}.zsync" ]; then
        mv "$PROJECT_ROOT/${OUTPUT_FILENAME}.zsync" "${OUTPUT_PATH}.zsync"
    fi
    if [ ! -f "${OUTPUT_PATH}.zsync" ]; then
        (
            cd "$OUTPUT_DIR"
            zsyncmake -u "$OUTPUT_FILENAME" "$OUTPUT_FILENAME"
        )
    fi
fi

(
    cd "$OUTPUT_DIR"
    sha256sum "$OUTPUT_FILENAME" > "${OUTPUT_FILENAME}.sha256"
)

echo ""
echo "================================="
echo "Build complete!"
echo "================================="
echo "AppImage: $OUTPUT_PATH"
if [ -f "${OUTPUT_PATH}.zsync" ]; then
    echo "Zsync:    ${OUTPUT_PATH}.zsync"
fi
echo "SHA256:   ${OUTPUT_PATH}.sha256"
echo ""
echo "To run:"
echo "  chmod +x $OUTPUT_PATH"
echo "  $OUTPUT_PATH"
echo ""
