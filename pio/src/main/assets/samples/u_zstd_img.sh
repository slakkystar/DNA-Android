if [ ! -d "$DNA_PRO/out" ]; then
    mkdir -p "$DNA_PRO/out"
fi

if [ -z "$DNA_ZSTD_THREADS" ] && command -v dna >/dev/null 2>&1; then
    DNA_ZSTD_THREADS=$(dna perf --shell 2>/dev/null | sed -n 's/^DNA_ZSTD_THREADS=\([0-9][0-9]*\)$/\1/p' | head -n 1)
fi
threads=${DNA_ZSTD_THREADS:-}
case "$threads" in ''|*[!0-9]*) threads=$(nproc 2>/dev/null || echo 1); [ "$threads" -gt 4 ] && threads=4 ;; esac
[ "$threads" -lt 1 ] && threads=1

for i in $ZST; do
    input="$DNA_PRO/$i"
    if [ ! -f "$input" ]; then
        echo "File error $input: file not found!"
        continue
    fi

    if expr "$i" : '.*\.zst$' >/dev/null || expr "$i" : '.*\.zstd$' >/dev/null; then
        echo "> Extracting: $i"
        line=$(echo "$i" | cut -d"." -f1)
        zstd -d -T"$threads" -k -f "$input" -o "$DNA_PRO/out/$line.img"
        echo "> Extraction complete, file located at: $DNA_PRO/out/$line.img"
        if [[ $silence = 1 ]]; then
            echo "> Deleting: $i"
            rm -rf "$input"
        fi
    fi
done