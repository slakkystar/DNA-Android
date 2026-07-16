if [ ! -d "$DNA_PRO/out" ]; then
    mkdir -p "$DNA_PRO/out"
fi

for i in $ZST; do
    input="$DNA_PRO/$i"
    if [ ! -f "$input" ]; then
        echo "File error $input: file not found!"
        continue
    fi

    if expr "$i" : '.*\.zst$' >/dev/null || expr "$i" : '.*\.zstd$' >/dev/null; then
        echo "> Extracting: $i"
        line=$(echo "$i" | cut -d"." -f1)
        zstd -d -k -f "$input" -o "$DNA_PRO/out/$line.img"
        echo "> Extraction complete, file located at: $DNA_PRO/out/$line.img"
        if [[ $silence = 1 ]]; then
            echo "> Deleting: $i"
            rm -rf "$input"
        fi
    fi
done