# 8BitVita

Basically just X1Vita but with 8bitdo pro 2 vid, pid, & mappings. 

**Compatible 8bitdo controllers**: Lite, Pro, Pro+, Pro 2, & Zero 2.


**Download**: https://github.com/shadowknight1620/8BitVita/releases


**Enable the plugin:**

1. Add 8BitVita.skprx to taiHEN's config (ux0:/tai/config.txt) or (ur0:/tai/config.txt):
	```
	*KERNEL
	ux0:tai/8BitVita.skprx
	```
2. You need to refresh the config.txt by rebooting or through VitaShell.

**Using it for the first time (pairing the controller):**

1. Go to Settings -> Devices -> Bluetooth Devices
2. Turn on the controller in x-input mode, typically ```Start + X``` on most 8bitdo models (Except the Lite and Pro 2).
3. Put the controller into pairing mode by holding the ```Select``` button on most 8bitdo models (Unless it has a dedicated pairing button like the Lite, Pro, Pro+, and Pro 2), the led will blink rapidly or a pattern will begin flashing on the led's.
4. Select the controller name in the bluetooth devices list, the controller will then connect and be paired (You may get a message saying "Do you want to connect?" press ok).

**Using it once paired (see above):**
1. Just turn on the controller in x-input mode and it will connect to the Vita.

**Note**: This plugin is compatible with minivitaTV ;) If you are using multiple controllers the controller ports will be set in the order you connect (if available), so first connection -> port 1 second connection -> port 2 etc.
  

Credit goes to M Ibrahim.
Thanks to ShadowKnight1620 for testing. 

Based on X1Vita & ds4vita
