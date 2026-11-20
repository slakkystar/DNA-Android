#!/system/bin/sh

project=$(cat "$TMPDIR/DNA.ini")
DNA_PRO="$DNA_DIR/$project"
mkdir -p "$DNA_PRO/out"

CHUNK_SPLIT="$START_DIR/tools/chunk_split"

for prefix in $IMG; do
    input_file="$DNA_PRO/$prefix"
    output_dir="$DNA_PRO/out"

    if [ ! -f "$input_file" ]; then
        echo "ERROR: File not found → $input_file"
        continue
    fi

    size_bytes=$(stat -c %s "$input_file")
    size_gb=$(echo "scale=2; $size_bytes / (1024 * 1024 * 1024)" | bc)
    size_mb=$(echo "$size_gb * 1024" | bc | awk '{printf "%.0f", $1}')

    if [ "$parts" -lt 4 ] || [ "$parts" -gt 25 ]; then
        echo "ERROR: Invalid part count ($parts). Must be between 4 and 25."
        continue
    fi

    echo "> Detected Size: ${size_mb}MB (${size_gb} GB)"
    echo "> Splitting into $parts parts..."

    cp "$input_file" "$output_dir/" || {
        echo "ERROR: Copy failed → $input_file"
        continue
    }

    cd "$output_dir" || continue

    chunk_size_mb=$(( (size_mb + parts - 1) / parts ))

    "$CHUNK_SPLIT" -C "${chunk_size_mb}M" -s .%d "$prefix" || {
        echo "ERROR: Split failed → $prefix"
        rm -f "$prefix"
        continue
    }

    rm -f "$prefix"

    i=0
    for f in $(ls "$prefix".* 2>/dev/null | sort -V); do
        new_name="$prefix.$i"
        mv "$f" "$new_name"
        echo "> Created: $new_name"
        i=$((i + 1))
    done

    echo "> Total Parts Created: $i"
    echo
done
