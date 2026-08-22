
# Check if $DNA_DRO is set
if [ -z "$DNA_DRO" ]; then
    echo "[-] Error: \$DNA_DRO variable is not set!"
    exit 1
fi

# Check if $type_select variable is set
if [ -z "$type_select" ]; then
    echo "[-] Error: \$type_select variable is not set!"
    echo "Usage example: type_select=\"odm\" $0"
    echo "Usage example: type_select=\"product system_ext\" $0"
    echo "Usage example: type_select=\"all\" $0"
    echo "Available partitions: product, system_ext, odm"
    exit 1
fi

CONFIG_DIR="$DNA_DRO/config"

get_dir_size_bytes() {
    local target_dir="$1"
    du -sb "$target_dir" | awk '{print $1}'
}

# Main function to process partitions
process_partition() {
    local part="$1"         # Partition name (product, system_ext, odm)
    local parent_part="$2"  # Parent partition (system or vendor)
    local src_path=""

    echo "[*] Processing partition: $part (Parent: $parent_part)"

    # 1. Locate original source path
    if [ "$part" = "odm" ]; then
        if [ -d "$DNA_DRO/vendor/odm" ] && [ ! -L "$DNA_DRO/vendor/odm" ]; then
            src_path="$DNA_DRO/vendor/odm"
        else
            echo "[-] Partition odm not found in vendor/odm (or already processed)."
            return 1
        fi
    else
        if [ -L "$DNA_DRO/system/system/$part" ]; then
            src_path="$DNA_DRO/system/$part"
        elif [ -d "$DNA_DRO/system/system/$part" ] && [ ! -L "$DNA_DRO/system/system/$part" ]; then
            src_path="$DNA_DRO/system/system/$part"
        else
            echo "[-] Partition $part not found in system/ or system/system/"
            return 1
        fi
    fi

    echo "[+] Original $part found at: $src_path"

    # 2. Create target directory in root with 755 permissions
    local target_dir="$DNA_DRO/$part"
    mkdir -p "$target_dir"
    chmod 755 "$target_dir"

    # 3. Move contents to external directory
    if [ -d "$src_path" ]; then
        mv "$src_path"/* "$src_path"/.* "$target_dir"/ 2>/dev/null
    fi

    # 3.1 Special condition for odm: create symlink vendor/odm -> /odm
    if [ "$part" = "odm" ]; then
        rm -rf "$src_path"
        ln -s /odm "$src_path"
        echo "[+] Created symlink vendor/odm -> /odm"
    fi

    # 4. Extract and filter file_contexts
    local base_fc="$CONFIG_DIR/${parent_part}_file_contexts"
    local part_fc="$CONFIG_DIR/${part}_file_contexts"
    
    if [ -f "$base_fc" ]; then
        grep -a -E "^/${parent_part}/${part}(/|[[:space:]]|$)" "$base_fc" | sed -E "s|^/${parent_part}/${part}|/${part}|g" > "$part_fc"
        grep -a -v -E "^/${parent_part}/${part}(/|[[:space:]]|$)" "$base_fc" > "$base_fc.tmp" && mv "$base_fc.tmp" "$base_fc"
        echo "[+] Created $part_fc ($(wc -l < "$part_fc") lines) and cleaned ${parent_part}_file_contexts"
    fi

    # 5. Extract and filter fs_config
    local base_fs="$CONFIG_DIR/${parent_part}_fs_config"
    local part_fs="$CONFIG_DIR/${part}_fs_config"

    if [ -f "$base_fs" ]; then
        grep -a -E "^(/?${parent_part}/)?${part}(/|[[:space:]]|$)" "$base_fs" | sed -E "s|^(/?${parent_part}/)?${part}|${part}|g" > "$part_fs"
        grep -a -v -E "^(/?${parent_part}/)?${part}(/|[[:space:]]|$)" "$base_fs" > "$base_fs.tmp" && mv "$base_fs.tmp" "$base_fs"
        echo "[+] Created $part_fs ($(wc -l < "$part_fs") lines) and cleaned ${parent_part}_fs_config"
    fi

    # 6. Calculate exact size in bytes and save to info file
    local part_info="$CONFIG_DIR/${part}_info"
    local size_bytes=$(get_dir_size_bytes "$target_dir")
    
    echo "$size_bytes" > "$part_info"
    echo "[+] Saved size ($size_bytes bytes) to $part_info"
}

# Helper dispatch function
run_partition() {
    local target="$1"
    case "$target" in
        product)
            process_partition "product" "system"
            ;;
        system_ext)
            process_partition "system_ext" "system"
            ;;
        odm)
            process_partition "odm" "vendor"
            ;;
        *)
            echo "[-] Unknown partition: $target"
            ;;
    esac
}

# Process partitions based on $type_select
if [ "$type_select" = "all" ]; then
    run_partition "product"
    echo "--------------------------------------"
    run_partition "system_ext"
    echo "--------------------------------------"
    run_partition "odm"
else
    for item in $type_select; do
        run_partition "$item"
        echo "--------------------------------------"
    done
fi

echo "[+] Operation completed successfully!"
