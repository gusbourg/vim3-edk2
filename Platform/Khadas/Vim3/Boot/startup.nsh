@echo -off

if exist fs0:\VIM3_LINUX.EFI then
  initrd fs0:\VIM3_INITRD
  fs0:\VIM3_LINUX.EFI root=UUID=e92a34bb-3bd5-457c-8816-6200a6e21fbf rootwait rootfstype=ext4 ro console=tty1 console=ttyAML0,115200 earlycon=meson,0xff803000 ignore_loglevel loglevel=8 consoleblank=0 coherent_pool=2M cgroup_enable=memory
endif

if exist fs1:\VIM3_LINUX.EFI then
  initrd fs1:\VIM3_INITRD
  fs1:\VIM3_LINUX.EFI root=UUID=e92a34bb-3bd5-457c-8816-6200a6e21fbf rootwait rootfstype=ext4 ro console=tty1 console=ttyAML0,115200 earlycon=meson,0xff803000 ignore_loglevel loglevel=8 consoleblank=0 coherent_pool=2M cgroup_enable=memory
endif

echo VIM3 Linux EFI payload not found on fs0: or fs1:
