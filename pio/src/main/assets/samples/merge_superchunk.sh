project=$(cat $TMPDIR/DNA.ini)
DNA_PRO=$DNA_DIR/$project
if [ ! -d $DNA_PRO/out ];then
 mkdir -p $DNA_PRO/out
fi

for prefix in $IMG; do
    safe_prefix=$(printf "%s" "$prefix" | sed 's/[.[*^$+(){}|]/\\&/g')
    pattern="^${safe_prefix}\.[0-9]\{1,\}$"
    find "$DNA_PRO" -maxdepth 1 -type f -name "${prefix}*" -exec basename {} \; |
    grep "$pattern" |
    tr '\n' ' ' |
    {
        read -r files
        cd $DNA_PRO
        echo "> Starting file merge into: $prefix"
        simg2img ${files% } $DNA_PRO/out/$prefix
        cd
        if [ -f $DNA_PRO/out/$prefix ];then
            echo "> Merge completed, file is located at: $DNA_PRO/out/$prefix"
        else
            echo "DEBUG: > Processing single prefix '$prefix'" >&2
            echo "DEBUG: > Generated regular expression '$pattern'" >&2
            echo "DEBUG: > Processing '${files% }' failed, take a screenshot and contact the developer for a fix" >&2
        fi
    }
done