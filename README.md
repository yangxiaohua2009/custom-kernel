# custom-kernel
the kernel in sof.zip is built from https://github.com/plbossart/sound/tree/fix/sof-es8336-quirk
unzip the sof.zip, you should see three deb files. Put them alongside the firmware.deb. Or install them by sudo dpkg -i *.deb

sudo su

chmod +x make.sh

./make.sh
