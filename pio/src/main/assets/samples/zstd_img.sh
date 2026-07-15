if [ ! -d $DNA_PRO/out ];then
 mkdir -p $DNA_PRO/out
fi

for i in $ZST; do
    if [ ! -f "$DNA_PRO/$i" ]; then
        echo "Error: file $DNA_PRO/$i does not exist!"
        continue
    fi
    if expr "$i" : '.*\.zst$' >/dev/null || expr "$i" : '.*\.zstd$' >/dev/null; then
        echo "> Starting decompression: $i"
        line=$(echo "$i" | cut -d"." -f1)
        zstd -d -k -f $DNA_PRO/$i -o $DNA_PRO/out/$line.img
        echo "> Decompression completed, file is located at: $DNA_PRO/out/$line.img"
    fi
    if expr "$i" : '.*\.img$' >/dev/null; then
        echo "> Starting compression: $i "
        zstd -$level -T4 -f $DNA_PRO/$i -o $DNA_PRO/out/$i.zst
        echo "> Compression completed, file is located at: $DNA_PRO/out/$i.zst"
    fi
    if [[ $silence = 1 ]]; then
        echo "> Deleting: $i"
        rm -rf "$DNA_PRO/$i"
    fi
done