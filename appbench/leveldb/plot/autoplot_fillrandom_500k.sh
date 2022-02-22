#!/bin/bash

# create data file for zplot
rm fillrandom_500k.data
touch fillrandom_500k.data
echo "# value ext4dax splitfs devfs macrofs macrofs_slowdown" > fillrandom_500k.data

# extract data from result
declare -a valuearr=("512" "4096")

LINE=2
for VALUE in "${valuearr[@]}"
do
	echo "$VALUE " >> fillrandom_500k.data	

	num="`grep -o '[0-9]*.[0-9]* MB.s' ../result/result-dax-500k/fillrandom-readrandom_1_"$VALUE".txt | sed 's/ MB.s//'1`"
	sed -i -e "$LINE s/$/ $num/" fillrandom_500k.data

	num="`grep -o '[0-9]*.[0-9]* MB.s' ../result/result-splitfs-500k/fillrandom-readrandom_1_"$VALUE".txt | sed 's/ MB.s//'1`"
	sed -i -e "$LINE s/$/ $num/" fillrandom_500k.data

	num="`grep -o '[0-9]*.[0-9]* MB.s' ../result/result-devfs-500k/fillrandom-readrandom_1_"$VALUE".txt | sed 's/ MB.s//'1`"
	sed -i -e "$LINE s/$/ $num/" fillrandom_500k.data

	num="`grep -o '[0-9]*.[0-9]* MB.s' ../result/result-macrofs-500k/fillrandom-readrandom_1_"$VALUE".txt | sed 's/ MB.s//'1`"
	sed -i -e "$LINE s/$/ $num/" fillrandom_500k.data

	num="`grep -o '[0-9]*.[0-9]* MB.s' ../result/result-macrofs-500k-1.2GHz/fillrandom-readrandom_1_"$VALUE".txt | sed 's/ MB.s//'1`"
	sed -i -e "$LINE s/$/ $num/" fillrandom_500k.data

	let LINE=LINE+1

done

# now plot the graph with zplot
python fillrandom_500k.py
