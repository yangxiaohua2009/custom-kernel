#!/bin/bash

chip_id=$(sudo ls /sys/bus/i2c/devices/ | grep 8326)
ucm=0 #ucm syntax
if [ ! -d /usr/share/alsa/ucm2 ]
then
ucm=1 #syntax 1
elif [ ! -d /usr/share/alsa/ucm2/conf.d ]
then
ucm=2 #syntax 2
else
ucm=4 #syntax 4
fi

if [ $chip_id ] && [ $ucm = 1 ]
then
echo "8326 ucm" 
elif [ $chip_id ] && [ $ucm = 2 ]
then
echo "8326 ucm2"
cp -r ./ucm2/es8326/sof-essx8336 /usr/share/alsa/ucm2/
elif [ $chip_id ] && [ $ucm = 4 ]
then
echo "8326 ucm latest"
cp -r ./ucm2_latest/sof-essx8336 /usr/share/alsa/ucm2/conf.d
cp -r ./ucm2_latest/sof-essx8336 /usr/share/alsa/ucm2/Intel
cp /usr/share/alsa/ucm2/conf.d/sof-essx8336/HiFi_8326.conf /usr/share/alsa/ucm2/conf.d/sof-essx8336/HiFi.conf
elif [ ! $chip_id ] && [ $ucm = 1 ]
then
echo "8336 ucm" 
cp -r ./ucm/sof-essx8336 /usr/share/alsa/ucm/
elif [ ! $chip_id ] && [ $ucm = 2 ]
then
echo "8336 ucm2"
cp -r ./ucm2/es8336/sof-essx8336 /usr/share/alsa/ucm2/
elif [ ! $chip_id ] && [ $ucm = 4 ]
then
echo "8336 ucm latest"
cp -r ./ucm2_latest/sof-essx8336 /usr/share/alsa/ucm2/conf.d
cp -r ./ucm2_latest/sof-essx8336 /usr/share/alsa/ucm2/Intel
fi
