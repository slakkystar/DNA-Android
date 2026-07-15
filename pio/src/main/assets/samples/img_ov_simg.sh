if [ ! -d $DNA_PRO/out ];then
 mkdir -p $DNA_PRO/out
fi
for i in $IMG ;do
info=$(dna gettype $DNA_PRO/$i)
if [ "$info" = "ext" ] || [ "$info" = "erofs" ]; then
  echo "> Starting conversion: $i"
    img2simg $DNA_PRO/$i $DNA_PRO/out/$i
elif [ "$info" = "sparse" ]; then
  echo "> Starting conversion: $i"
  simg2img $DNA_PRO/$i $DNA_PRO/out/$i
else
  echo "> Conversion of this format is not supported: $i"
fi
if [[ $silence = 1 ]]; then
  echo "> Deleting: $i"
  rm -rf $DNA_PRO/$i
fi
if [ -f $DNA_PRO/out/$i ];then
  echo "> Conversion completed, file saved to: $DNA_PRO/out"
else
  echo "> Conversion failed, take a screenshot and contact the developer for a fix!!!"
fi
done