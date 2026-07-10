#本脚本由　by Han | 情非得已c，编写
#应用于搞机助手上
name(){
jian="$TEMP_DIR/by_name.log"
if [[ ! -f $jian ]]; then
        BLOCKDEV=`which blockdev`
        find /dev/block -mindepth 1 -type l | while read o; do
            [[ -d "$o" ]] && continue
            c=`basename "$o"`
            echo ${b[@]} | grep -q "$c" && continue
            echo $c
        done | sort -u | while read Row; do
            BLOCK=`find /dev/block -name $Row | head -n 1`
            if [[ $BLOCK == *uuid/* || $BLOCK == *mapper/com.* || $BLOCK == */sd* ]]; then continue; fi
            if [[ -n $BLOCKDEV ]]; then   
                size=`blockdev --getsize64 $BLOCK`
                if [[ $size -ge 1073741824 ]]; then
                    File_Type=`awk "BEGIN{print $size/1073741824}"`G
                elif [[ $size -ge 1048576 ]]; then
                    File_Type=`awk "BEGIN{print $size/1048576}"`MB
                elif [[ $size -ge 1024 ]]; then
                    File_Type=`awk "BEGIN{print $size/1024}"`kb
                elif [[ $size -le 1024 ]]; then
                    File_Type=${size}b
                fi
                    echo "$BLOCK|$Row 「大小：$File_Type」" >>$jian
            else
                echo "$BLOCK|$Row" >>$jian
            fi
        done
fi
cat $jian
}

extract(){
Extract=$DNA_DIR/image
IFS=$'\n'
[[ ! -d "$Extract" ]] && mkdir -p "$Extract"

for i in $IMG; do
    e=${i##*/}
    File="$Extract/${e}.img"
    if [[ ! -L $i ]];then
        echo "！未找到 $e 分区，无法提取"
    else
        echo "- 正在提取 $e 分区"
        dd if="$i" of="$File"
        echo "- 已将 $e 分区提取至：$File"
    fi
done
}

flash(){
IFS=$'\n'
e=${IMG##*/}
echo "- 当前选择的分区: $e"
echo "- 刷入文件路径：$Brush_in"
if [[ ! -L "$IMG" ]];then
    echo "！未找到 $e 分区，无法刷入"
else
    if [[ -f "$Brush_in" ]]; then
        echo "- 正在刷入 $e 分区"
        dd if="$Brush_in" of="$IMG"
        if [[ $CQ = 1 ]]; then
         echo "正在重启至 Recovery 模式，倒计时……"
         for i in $(seq 4 -1 1); do
            echo $i
            sleep 1
         done
         reboot recovery
         fi
         if [[ $CQ1 = 1 ]]; then
          echo "正在重启系统，倒计时……"
          for i in $(seq 4 -1 1); do
            echo $i
            sleep 1
          done
          reboot
         fi
    else
        echo "！未找到刷入文件 $Brush_in，无法写入到 $e 分区"
    fi
    echo "- 完成"
    sleep 2
fi
}
$1