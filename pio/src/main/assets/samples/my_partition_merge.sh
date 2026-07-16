systemdir="$DNA_DRO/system"
configdir="$DNA_DRO/config"
dynamic_fs_dir="$DNA_DRO/dynamic_fs"
target_fs="$configdir/system_fs_config"
target_contexts="$configdir/system_file_contexts"

rm -rf $DNA_DRO/dynamic_fs
mkdir -p $DNA_DRO/dynamic_fs

for partition in $(ls $DNA_DRO | grep my_);do
  [ ! -d $systemdir ] && continue && echo "system.img не распакован, распакуйте его и продолжите"
  [ ! -d $DNA_DRO/$partition ] && continue && echo "$partition.img не распакован, распакуйте его и продолжите"
  echo "> Объединение раздела ${partition}"
  if [ -d $DNA_DRO/$partition ];then
    rm -rf $systemdir/$partition
    rm -rf $DNA_DRO/$partition/lost+found
    mv $DNA_DRO/$partition $systemdir/
  fi
  
  if [ -f $configdir/${partition}_file_contexts ];then
    mv $configdir/${partition}_file_contexts $dynamic_fs_dir/
  fi

  if [ -f $configdir/${partition}_fs_config ];then
    mv $configdir/${partition}_fs_config $dynamic_fs_dir/
    rm -rf $configdir/${partition}_info
  fi
  for i in $(ls $dynamic_fs_dir | grep "${partition}_file_contexts$");do
    if [ -e $dynamic_fs_dir/$i ];then
      contexts_header=$(grep -n -o -E "^/ u:" $dynamic_fs_dir/$i | head -n 1 | grep -o -E "^[0-9]+")
      [ -n "$contexts_header" ] && sed -i "${contexts_header}d" $dynamic_fs_dir/$i
      sed -i '1d' $dynamic_fs_dir/$i
      sed -i '/\?/d' $dynamic_fs_dir/$i
      sed -i 's/^/&\/system/g' $dynamic_fs_dir/$i
      sed -i "/system\/${partition} /d" $target_contexts
      echo "/system/${partition} u:object_r:system_file:s0" >> $dynamic_fs_dir/$i
      cat $dynamic_fs_dir/$i >> $dynamic_fs_dir/${partition}_merge_contexts
      cat $dynamic_fs_dir/${partition}_merge_contexts >> $target_contexts
    fi
  done

  for i in $(ls $dynamic_fs_dir | grep "${partition}_fs_config$");do
    if [ -e $dynamic_fs_dir/$i ];then
      config_header=$(grep -n -o -E "^/ 0" $dynamic_fs_dir/$i | head -n 1 | grep -o -E "^[0-9]+")
      [ -n "$config_header" ] && sed -i "${config_header}d" $dynamic_fs_dir/$i
      sed -i '1d' $dynamic_fs_dir/$i
      sed -i 's/^/&system\//g' $dynamic_fs_dir/$i
      sed -i '1d' $dynamic_fs_dir/$i
      sed -i "/system\/${partition} /d" $target_fs
      echo "system/${partition} 0 0 0755" >> $dynamic_fs_dir/$i
      cat $dynamic_fs_dir/$i >> $dynamic_fs_dir/${partition}_merge_fs_config
      cat $dynamic_fs_dir/${partition}_merge_fs_config >> $target_fs
    fi
  done
  echo "Объединение раздела завершено"
      echo "import /${partition}/build.prop" >> $systemdir/system/build.prop
done
rm -rf $dynamic_fs_dir