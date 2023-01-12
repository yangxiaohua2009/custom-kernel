#!/bin/bash

echo install kernel
#sudo dpkg -i *.deb
sudo dpkg -i *.deb

echo copy topology
tar -xvf data.tar
cp sof-jsl-es8336-ssp1.tplg ./lib/firmware/intel/sof-tplg/
cp -r ./lib/firmware/intel/sof-tplg/ /lib/firmware/intel/
cp -r ./lib/firmware/intel/sof/ /lib/firmware/intel/
cp ./tplg/* /lib/firmware/intel/sof-tplg/

#cp es8336.conf /etc/modprobe.d/

#cp -r ./ucm/sof-essx8336 /usr/share/alsa/ucm2/conf.d
#cp -r ./ucm/sof-essx8336 /usr/share/alsa/ucm2/Intel
sudo chmod +x ./ucm/ucm.sh
cd ./ucm/
sudo ./ucm.sh

sed -i '/load-module module-suspend-on-idle/d' /etc/pulse/default.pa
echo 'load-module module-suspend-on-idle timeout=0' >> /etc/pulse/default.pa
