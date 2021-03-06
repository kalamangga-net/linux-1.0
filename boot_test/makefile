all: clean os run


os:
	#bootsect
	gcc -I../include/ bootsect.S -o bootsect.s -E
	as86 bootsect.s -0 -a -o bootsect.o           
	ld86 -s -o bootsect bootsect.o                
	dd if=bootsect skip=32 bs=1 of=bootsect.bin   
	#setup
	gcc -I../include/ setup.S -o setup.s -E       
	as86 setup.s -0 -a -o setup.o                 
	ld86 -s -o setup setup.o                      
	dd if=setup skip=32 bs=1 of=setup.bin         
	#head
	gcc head.S -I../include/ -m32 -c              
	#main
	gcc -m32 main.c -c -mmanual-endbr -fno-stack-protector
	#combine head and main
	ld -m elf_i386 head.o main.o -o a.out --section-start=.text=0x1000
	#dumpy only necessery sections from head and main
	objcopy -O binary -j.text -j.data -j.rodata -j.bss a.out b.out
	#combine bootsect and setup
	cat bootsect.bin setup.bin > os.bin
	#padd os to 2560 bytes
	dd if=os.bin of=os_pad.bin bs=2560 conv=sync
	#append head and main
	cat b.out >> os_pad.bin
	#padd to 1.44M
	dd if=os_pad.bin of=os_pad.img bs=1M conv=sync

run:
	bochs -f bochsrc -q

clean:
	rm -f a.out
	rm -f b.out
	rm -f bootsect
	rm -f bootsect.bin
	rm -f bootsect.o
	rm -f bootsect.s
	rm -f setup
	rm -f setup.bin
	rm -f setup.o
	rm -f setup.s
	rm -f head.s
	rm -f head.o
	rm -f main.o
	rm -f os.bin
	rm -f os_pad.img
	rm -f os_pad.bin

