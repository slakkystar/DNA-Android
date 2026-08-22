stok="$DNA_DRO"
base_src="$START_DIR/samples/hal/charger"

echo "=== Charging HAL Fix ==="

if [ -d "$stok/odm" ]; then
    odm="$stok/odm"
    odm_prefix="/odm"
    contexts_target="$stok/config/odm_file_contexts"
    echo ">> ODM found: $odm"
elif [ -d "$stok/vendor/odm" ]; then
    odm="$stok/vendor/odm"
    odm_prefix="/vendor/odm"
    contexts_target="$stok/config/vendor_file_contexts"
    echo ">> ODM found: $odm"
else
    echo ">> Error: ODM partition not found (neither $stok/odm nor $stok/vendor/odm exist)"
    exit 1
fi

get_prop () {
    key="$1"
    find "$stok" -type f \( -name "build.prop" -o -name "*.prop" \) 2>/dev/null | \
    while read -r f; do
        val=$(grep -m1 "^$key=" "$f" 2>/dev/null | cut -d'=' -f2- | tr -d '\r')
        if [ -n "$val" ]; then
            echo "$val"
            break
        fi
    done
}

soc_manufacturer=$(get_prop "ro.soc.manufacturer")
soc_manufacturer_lc=$(echo "$soc_manufacturer" | tr '[:upper:]' '[:lower:]')
soc=""

if [ -n "$soc_manufacturer" ]; then
    case "$soc_manufacturer_lc" in
        *mediatek*|*mtk*)
            soc="mtk"
            ;;
        *qti*|*qualcomm*)
            soc="qcom"
            ;;
    esac
fi

if [ -z "$soc" ]; then
    board_platform=$(get_prop "ro.board.platform")
    board_platform_lc=$(echo "$board_platform" | tr '[:upper:]' '[:lower:]')
    echo ">> ro.soc.manufacturer not found or unrecognized, checking ro.board.platform ($board_platform)"
    case "$board_platform_lc" in
        *mt[0-9]*)
            soc="mtk"
            ;;
        *)
            soc="qcom"
            ;;
    esac
fi

echo ">> Detected SOC: $soc"

if [ "$soc" = "mtk" ]; then
    release=$(get_prop "ro.vendor.build.version.release")
    [ -z "$release" ] && release=$(get_prop "ro.odm.build.version.release")
    release_num=$(echo "$release" | grep -o -E '^[0-9]+')

    echo ">> Android release detected: $release"

    if [ -n "$release_num" ] && [ "$release_num" -lt 13 ]; then
        src="$base_src/old_mtk"
        echo ">> Using old_mtk HAL (release < 13)"
    else
        src="$base_src/mtk"
        echo ">> Using mtk HAL (release >= 13 or unknown)"
    fi
else
    src="$base_src/qcom"
    echo ">> Using qcom HAL"
fi

if [ ! -d "$src" ]; then
    echo ">> Error: source folder not found: $src"
    exit 1
fi

map="etc/init:644 etc/permissions:644 etc/vintf/manifest:644 framework:644 lib:644 lib64:644 bin/hw:755"

for entry in $map; do
    rel="${entry%%:*}"
    perm="${entry##*:}"

    [ -d "$src/$rel" ] || continue

    mkdir -p "$odm/$rel"

    for file in "$src/$rel"/*; do
        [ -f "$file" ] || continue
        name=$(basename "$file")
        cp -f "$file" "$odm/$rel/$name"
        chmod "$perm" "$odm/$rel/$name"
        echo ">> Copied: $rel/$name (chmod $perm)"
    done
done

mkdir -p "$(dirname "$contexts_target")"
[ -f "$contexts_target" ] || touch "$contexts_target"

if [ -d "$src/bin/hw" ]; then
    for file in "$src/bin/hw"/*; do
        [ -f "$file" ] || continue
        name=$(basename "$file")
        escaped_name=$(echo "$name" | sed 's/\./\\./g')
        context_line="$odm_prefix/bin/hw/$escaped_name u:object_r:hal_allocator_default_exec:s0"

        if grep -qF "$context_line" "$contexts_target" 2>/dev/null; then
            echo ">> Context already present for $name, skipping"
        else
            echo "$context_line" >> "$contexts_target"
            echo ">> Context added: $context_line"
        fi
    done
fi

echo "=== Done ==="
