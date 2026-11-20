#!/system/bin/sh

project=$(cat "$TMPDIR/DNA.ini")
DNA_PRO="$DNA_DIR/$project"
mkdir -p "$DNA_PRO/out"

for prefix in $IMG; do
    safe_prefix=$(printf "%s" "$prefix" | sed 's/[.[*^$+(){}|]/\\&/g')
    pattern="^\\./${safe_prefix}\\.[0-9]+$"

    cd "$DNA_PRO" || exit 1
    files_arr=$(find . -maxdepth 1 -type f -name "${prefix}.*" | grep -E "$pattern" | sort -V)

    echo "Merging: $prefix"

    [ -z "$files_arr" ] && { echo "Error: No parts found for $prefix"; continue; }

    merged_parts=""
    for f in $files_arr; do
        merged_parts="$merged_parts $f"
    done

    simg2img $merged_parts "$DNA_PRO/out/$prefix"

    if [ -f "$DNA_PRO/out/$prefix" ]; then
        echo "Merge complete: $DNA_PRO/out/$prefix"
    else
        echo "Error: Merge failed for $prefix"
    fi
done
