#!/bin/bash

# create data file for zplot
rm readrandom_500k.data
touch readrandom_500k.data
echo "# value ext4dax splitfs devfs macrofs macrofs_slowdown" > readrandom_500k.data

# extract data from result
declare -a valuearr=("512" "4096")

LINE=2
for VALUE in "${valuearr[@]}"
do
	echo "$VALUE " >> readrandom_500k.data	

	num="`grep -o '[0-9]*.[0-9]* micros.op; (' ../result/result-dax-500k/fillrandom-readrandom_1_"$VALUE".txt | sed 's/ micros.op; (//'1`"
	sed -i -e "$LINE s/$/ $num/" readrandom_500k.data

	num="`grep -o '[0-9]*.[0-9]* micros.op; (' ../result/result-splitfs-500k/fillrandom-readrandom_1_"$VALUE".txt | sed 's/ micros.op; (//'1`"
	sed -i -e "$LINE s/$/ $num/" readrandom_500k.data

	num="`grep -o '[0-9]*.[0-9]* micros.op; (' ../result/result-devfs-500k/fillrandom-readrandom_1_"$VALUE".txt | sed 's/ micros.op; (//'1`"
	sed -i -e "$LINE s/$/ $num/" readrandom_500k.data

	num="`grep -o '[0-9]*.[0-9]* micros.op; (' ../result/result-macrofs-500k/fillrandom-readrandom_1_"$VALUE".txt | sed 's/ micros.op; (//'1`"
	sed -i -e "$LINE s/$/ $num/" readrandom_500k.data

	num="`grep -o '[0-9]*.[0-9]* micros.op; (' ../result/result-macrofs-500k-1.2GHz/fillrandom-readrandom_1_"$VALUE".txt | sed 's/ micros.op; (//'1`"
	sed -i -e "$LINE s/$/ $num/" readrandom_500k.data

	let LINE=LINE+1

done

# now plot the graph with zplot
python readrandom_500k.py
