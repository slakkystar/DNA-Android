stok="$DNA_TMP/DNA_stok"
src="$START_DIR/samples/hal/cryptoeng"
enable_prop="ro.oemports10t.cryptoeng=true"

echo "=== Cryptoeng HAL ==="

# We define the root of odm: priority to regular odm, otherwise vendor/odm
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

# Copy the entire structure as is
echo "> Step: Copy files"

find "$src" -mindepth 1 -type d | while read -r d; do
    rel="${d#"$src"/}"
    mkdir -p "$odm/$rel"
done

find "$src" -type f | while read -r f; do
    rel="${f#"$src"/}"
    mkdir -p "$odm/$(dirname "$rel")"
    cp -f "$f" "$odm/$rel"
    echo ">> Copied: $rel"
done

# Permissions: all folders -> 755, all files -> 644, except bin/hw/* -> 755
echo "> Step: Set permissions"

find "$src" -mindepth 1 -type d | while read -r d; do
    rel="${d#"$src"/}"
    chmod 755 "$odm/$rel"
done

find "$src" -type f | while read -r f; do
    rel="${f#"$src"/}"
    case "$rel" in
        bin/hw/*)
            chmod 755 "$odm/$rel"
            ;;
        *)
            chmod 644 "$odm/$rel"
            ;;
    esac
done

echo ">> Permissions set"

# Turn on HAL prop
[ -f "$odm/build.prop" ] || touch "$odm/build.prop"

if grep -qF "$enable_prop" "$odm/build.prop" 2>/dev/null; then
    echo ">> Prop already present in odm/build.prop, skipping"
else
    echo "" >> "$odm/build.prop"
    echo "# --- cryptoeng HAL enable ---" >> "$odm/build.prop"
    echo "$enable_prop" >> "$odm/build.prop"
    chmod 644 "$odm/build.prop"
    echo ">> Prop added to odm/build.prop: $enable_prop"
fi

# Automatically create a selinux context for each file in bin/hw
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
