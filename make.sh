#!/bin/bash

echo install intel firmware
sudo dpkg -i --force-overwrite *.deb

echo copy topology
cp ./tplg/* /lib/firmware/intel/sof-tplg/
cp /lib/firmware/intel/sof-tplg/sof-glk-es8336-ssp0.tplg /lib/firmware/intel/sof-tplg/sof-tgl-es8336-ssp0.tplg
cp jsl.ri  /lib/firmware/intel/sof/
#echo get dsdt
#cat /sys/firmware/acpi/tables/DSDT > dsdt.dat
#iasl -d dsdt.dat

#echo get gpio
#cat /sys/kernel/debug/gpio>gpio.txt

cp sof-dyndbg.conf.txt /etc/modprobe.d/sof-dyndbg.conf

echo allow selecting kernel from grub
#cp grub /etc/default/grub

#chmod +x *.sh
#systemctl enable acpid.service

#cp 3.sh /lib/systemd/system-sleep/

echo setting up hdmi device
cp -r sof-essx8336 /usr/share/alsa/ucm2
#sed -i '/load-module module-suspend-on-idle/d' /etc/pulse/default.pa

