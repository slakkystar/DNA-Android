project=$(cat $TMPDIR/DNA.ini)
DNA_PRO=$DNA_DIR/$project
if [ ! -d $DNA_PRO/out ];then
 mkdir -p $DNA_PRO/out
fi

for prefix in $IMG; do
    safe_prefix=$(printf "%s" "$prefix" | sed 's/[.[*^$+(){}|]/\\&/g')
    pattern="^${safe_prefix}\.[0-9]\\{1,\\}$"
    find "$DNA_PRO" -maxdepth 1 -type f -name "${prefix}*" -exec basename {} \; |
    grep "$pattern" |
    tr '\n' ' ' |
    {
        read -r files
        cd $DNA_PRO
        echo "> Начинаю объединение файлов в: $prefix"
        simg2img ${files% } $DNA_PRO/out/$prefix
        cd
        if [ -f $DNA_PRO/out/$prefix ];then
            echo "> Объединение завершено, файл находится: $DNA_PRO/out/$prefix"
        else
            echo "DEBUG: > Обработка отдельного префикса '$prefix'" >&2
            echo "DEBUG: > Сгенерированное регулярное выражение '$pattern'" >&2
            echo "DEBUG: > Обработка '${files% }' не удалась, сделайте скриншот и свяжитесь с разработчиком для исправления" >&2
        fi
    }
done