#!/bin/bash


qxcbs=`locate libqxcb.so`
numOfqxcbs=`locate libqxcb.so | wc -l`
#echo ${qxcbs}




if [ $numOfqxcbs -ne 1 ]; then
	cindex=0
	for xcb in $qxcbs
	do  
		((cindex++))
		echo $cindex $xcb	
	done

	echo ""
	echo   "choose the serial of libqxcb.so :" 
	read serial
fi


cindex=0
for i in $qxcbs
do  
	qcxb=$i
	((cindex++))
	if [ ${cindex} -eq ${serial} ]; then
		break
	fi	
done


echo "libqxcb is:" $qcxb 
echo ""


LibDir=$PWD"/libs"
if [ ! -d "$LibDir" ]; then
    $(mkdir "$LibDir")
fi


Target=$1
lib_array=($(ldd $Target | grep -o "/.*" | grep -o "/.*/[^[:space:]]*"))

echo "extra Qt lib is: $lib_array"



sql="Sql"
needSql=0
for Variable in ${lib_array[@]}
do
    cp "$Variable" "$LibDir"
	if [[ $Variable == *$sql* ]]
	then
		needSql=1
	fi
done

if [ ${needSql} -eq 1 ]; then
	echo "need sql ${needSql}"
fi


echo "copy $1 libs finshed"
echo "========================"

xcb_array=($(ldd $qcxb | grep -o "/.*" | grep -o "/.*/[^[:space:]]*"))
for Variable in ${xcb_array[@]}
do
    cp "$Variable" "$LibDir"
done


echo " "
echo "copy all  libs of '$qcxb'  finshed"
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>"



platforms="$(dirname "$qcxb")"
echo "paltforms dir is : $platforms"


cp -r $platforms  "$PWD"

if  [ "$needSql" -eq 1 ];
then
	sqldrivers="$(dirname "$platforms")/sqldrivers"
	cp -r $sqldrivers "$PWD"
	echo " "
	echo "copy $sqldrivers finshed"
	echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>"
fi

redundancyLib=$PWD/platforms/libs
rm -rf "$redundancyLib"

# last part 

echo "#!/bin/sh


appname=\`basename \$0 | sed s,\.sh\$,,\`
dirname=\`dirname \$0\`


tmp=\"\${dirname#?}\"
if [ \"\${dirname%\$tmp}\" != \"/\" ]; then
dirname=\$PWD/\$dirname
fi

LD_LIBRARY_PATH=\$dirname/libs:\$LD_LIBRARY_PATH

export LD_LIBRARY_PATH
cd \"\$dirname/\"

export QT_LOGGING_RULES=\"*.debug=true\"

\"\$dirname/\$appname\" \"\$@\"
"> $1.sh

chmod +x $1.sh

echo ""
echo "#################################################"
echo " generate '${1}.sh'  start up script finshed"


