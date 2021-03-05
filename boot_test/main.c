/*00000000         *UND*	00000000 stack_start
00000000         *UND*	00000000 _edata
00000000         *UND*	00000000 _end
00000000         *UND*	00000000 x86
00000000         *UND*	00000000 start_kernel
00000000         *UND*	00000000 hard_math
00000000         *UND*	00000000 printk
*/

#include <stdarg.h>

#define STACK_MAGIC	0xdeadbeef
#define PAGE_SIZE 0x400
#define KERNEL_DS	0x18

int hard_math = 0;
int x86 = 0;
char edata, end;

long user_stack [ PAGE_SIZE>>2 ] = { STACK_MAGIC, };

struct {
  long * a;
  short b;
} stack_start = { &user_stack[PAGE_SIZE>>2] , KERNEL_DS };


short* vga_vram = (short*)0xb8000;

int printk(const char *fmt, ...)
{
  //va_list args;
  //va_start(args,fmt);
  //va_end(args);
  return 0;
}


void start_kernel(void){
 //printk();
 const char* fmt = "hello world!\0A";
 short* vram = vga_vram;
 for(;*fmt;fmt++,vram++){
   *vram = 0x0f00|(*fmt);
 }
 for(;;);
}

