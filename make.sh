#!/bin/bash

echo install intel firmware
sudo dpkg -i --force-overwrite *.deb

echo copy topology
cp ./tplg/* /lib/firmware/intel/sof-tplg/
cp sof-glk-es8336-ssp0.tplg sof-cml-es8336.tplg
cp sof-glk-es8336-ssp1.tplg sof-apl-es8336.tplg
#echo get dsdt
#cat /sys/firmware/acpi/tables/DSDT > dsdt.dat
#iasl -d dsdt.dat

#echo get gpio
#cat /sys/kernel/debug/gpio>gpio.txt

cp sof-dyndbg.conf.txt /etc/modprobe.d/sof-dyndbg.conf

echo allow selecting kernel from grub
cp grub /etc/default/grub

#chmod +x *.sh
#systemctl enable acpid.service

#cp 3.sh /lib/systemd/system-sleep/

echo setting up hdmi device
cp -r sof-essx8336 /user/share/alsa/ucm2
sed -i '/load-module module-suspend-on-idle/d' /etc/pulse/default.pa

