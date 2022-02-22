#!/bin/bash  
  
for i in {0..9}  
do  
  sudo cpufreq-set -c $i -d 2.2GHz -u 2.2GHz -g performance
done  

for i in {10..39}  
do  
  sudo cpufreq-set -c $i -d 2.2GHz -u 2.2GHz -g performance
done
