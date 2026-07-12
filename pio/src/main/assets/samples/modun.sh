if [ ! -e $START_DIR/module ];then
  mkdir -p $START_DIR/module
  chmod -R 755 $START_DIR/module
  chown -R $APP_USER_ID:$APP_USER_ID $START_DIR/module
fi
cat <<Dna
<?xml version="1.0" encoding="utf-8"?>
<group>
    <action reload="true" auto-off="true">
        <title>Удалить плагин</title>
        <set>samples/project.sh sub</set>
        <param name="sub" title="Выберите плагин:" options-sh="ls -F $START_DIR/module | sed 's/\/$//g'" desc="Поиск файлов в пути плагинов" required="true" multiple="true"/>
    </action>
</group>
<group title="Список плагинов">
Dna

for var in $(find $START_DIR/module/ -name index.xml);do
cat $var
done

cat <<Dna
</group>
Dna