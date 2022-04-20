# custom-kernel
the kernel in sof.zip is built from https://github.com/plbossart/sound/tree/fix/sof-es8336-quirk
unzip the sof.zip, you should see three deb files. Put them alongside the firmware.deb. Or install them by sudo dpkg -i *.deb

`sudo su`

`chmod +x make.sh`

`./make.sh`


Common errors:

1. missing topologies: unzip everything in es8336-topologies-2.tar.gz and copy *.tplg to /lib/firmware/intel/sof-tplg/
2. i915 errors: install lastest sof firmware from sof website
3. wifi/display errors: compile your own kernel with the correct config in /boot/ and the lastest source code (currently 5.18). Select SND_SOC_INTEL_SOF_ES8336_MACH using menuconfig.
4. ucm errors: if `alsaucm reload` shows errors:
4.1. you are using ucm2: `sudo cp -r sof-essx8336 /usr/share/alsa/ucm2/`

4.2. you are using ucm: 
manually open the kcontrols using

		amixer cset name='Speaker Switch' on 
    
		amixer cset name='Headphone Playback Volume' 3,3
    
		amixer cset name='Right Headphone Mixer Right DAC Switch' on
    
		amixer cset name='Left Headphone Mixer Left DAC Switch' on 
    
		amixer cset name='DAC Playback Volume' 999,999 
    
		amixer cset name='Headphone Mixer Volume' 999,999 
    
