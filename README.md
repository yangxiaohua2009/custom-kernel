# custom-kernel
the kernel in sof_2_8.zip is built from https://github.com/plbossart/sound/tree/fix/sof-es8336-quirk on 2022-Feb-8
unzip the sof_2_8.zip, you should see three deb files. Put them alongside the firmware.deb. Or install them by sudo dpkg -i *.deb

sudo su

chmod +x make.sh

./make.sh
