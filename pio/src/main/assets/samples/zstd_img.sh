if [ ! -d $DNA_PRO/out ];then
 mkdir -p $DNA_PRO/out
fi

for i in $ZST; do
    if [ ! -f "$DNA_PRO/$i" ]; then
        echo "Ошибка: файл $DNA_PRO/$i не существует!"
        continue
    fi
    if expr "$i" : '.*\.zst$' >/dev/null || expr "$i" : '.*\.zstd$' >/dev/null; then
        echo "> Начало распаковки: $i"
        line=$(echo "$i" | cut -d"." -f1)
        zstd -d -k -f $DNA_PRO/$i -o $DNA_PRO/out/$line.img
        echo "> Распаковка завершена, файл находится: $DNA_PRO/out/$line.img"
    fi
    if expr "$i" : '.*\.img$' >/dev/null; then
        echo "> Начало сжатия: $i "
        zstd -$level -T4 -f $DNA_PRO/$i -o $DNA_PRO/out/$i.zst
        echo "> Сжатие завершено, файл находится: $DNA_PRO/out/$i.zst"
    fi
    if [[ $silence = 1 ]]; then
        echo "> Удаление: $i"
        rm -rf "$DNA_PRO/$i"
    fi
done