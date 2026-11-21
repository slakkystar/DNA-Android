val=$1
case ${val} in
dir)
ls -F $DNA_DRO | sed 's/\/$//g' | grep -v "config"
;;
zip1)
ls $DNA_DIR | grep -v "/$" | grep \."zip"$
;;
img1)
ls $DNA_DIR | grep -v "/$" | grep \."img"$
;;
br)
ls $DNA_PRO | grep -v "/$" | grep \."br"$
;;
dat)
ls $DNA_PRO | grep -v "/$" | grep \."new.dat"$
;;
img)
for i in $(ls $DNA_PRO | grep -v "/$" | grep \."img"$)
do
    if [ -f $DNA_PRO/$i ];then
        info=$(dna gettype $DNA_PRO/$i)
        echo "$i|$i ($info)"
    fi
done
;;
split_sparse)
{
    find "$DNA_PRO" -maxdepth 1 -type f -exec basename {} \;
    if [ -d "$DNA_PRO/out" ]; then
        find "$DNA_PRO/out" -maxdepth 1 -type f -exec basename {} \;
    fi
} |
    sed -n 's/\(.*\)\.[0-9]\{1,\}$/\1/p; t; d' |
    sort | uniq -c |
    while IFS= read -r line; do
        line=${line#"${line%%[! ]*}"}
        count=${line%% *}
        prefix=${line#* }
        printf "%s|%s Total %s files\n" "$prefix" "$prefix" "$count"
    done
;;
vbmeta)
    for i in $(ls $DNA_PRO | grep -E "^vbmeta.*\.img$"); do
    if [ -f "$DNA_PRO/$i" ]; then
        echo "$i|$i (vbmeta)"
    fi
    done
;;
zst_img)
ls $DNA_PRO | grep -v "/$" | grep -E "\.(zst|zstd|img)$"
;;
u_zst_img)
ls $DNA_PRO | grep -v "/$" | grep -E "\.(zst|zstd)$"
;;
*)
ls $DNA_PRO | grep -v "/$" | grep \."$1"$
;;
esac
