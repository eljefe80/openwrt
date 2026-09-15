/* picwdt-keepalive — reproduce the vendor's external-PIC watchdog waveform on
 * P0.6/P0.7 from userspace via /dev/mem, so a non-vendor kernel (our OpenWrt
 * bring-up) doesn't get reset by the hardware PIC watchdog.
 *
 * Reverse-engineered from the stock 2.6.34 kernel's built-in "picwdt" driver
 * (see picwdt-waveform-RE.md). Pin map + protocol, all measured:
 *   P0.7 = kernel gpio 7 = "PIC CLCK" = clock/strobe (push-pull)
 *   P0.6 = kernel gpio 6 = "PIC DATA" = data (push-pull TX, released to input on RX)
 * The driver runs a 100 Hz state machine (one step / 10 ms); CLCK toggles every
 * step (=> 50 Hz strobe), one DATA bit is driven per step (one bit per CLCK edge).
 * A frame = 16-bit sync 0x7FFE, 8-bit sync 0x70, 16 data bytes (a per-ping
 * nonce), then DATA is released and 16 reply bits are clocked in. We repeat the
 * frame back-to-back; that is a superset of the driver's idle keepalive, so the
 * PIC always sees live, well-formed protocol activity.
 *
 * LPC32xx GPIO block @0x40028000:
 *   P0_INP_STATE 0x40, P0_OUTP_SET 0x44, P0_OUTP_CLR 0x48,
 *   P0_DIR_SET 0x50, P0_DIR_CLR 0x54, P0_DIR_STATE 0x58.  P0.6=bit6, P0.7=bit7.
 *
 * usage: picwdt-keepalive [--step-us N] [--idle] [-v]
 *   --step-us N  half-clock period in microseconds (default 10000 = vendor 100 Hz)
 *   --idle       only strobe CLCK + a rolling DATA pattern (state 0), no challenge
 *   -v           log frame/reply activity to stderr
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/mman.h>

/* heartbeat to /dev/kmsg so we can SEE it run+feed on the muted console
 * (shows as a kernel-log line on ttyS0 / in dmesg) */
static int kfd=-1;
static void klog(const char*fmt,...){
    if(kfd<0) return;
    char b[160]; va_list a; va_start(a,fmt);
    int n=vsnprintf(b,sizeof b,fmt,a); va_end(a);
    if(n>0) { ssize_t w=write(kfd,b,(size_t)n); (void)w; }
}

#define GPIO_BASE 0x40028000
#define BIT_DATA  (1u<<6)   /* P0.6 */
#define BIT_CLCK  (1u<<7)   /* P0.7 */

static volatile uint32_t *INP,*OSET,*OCLR,*DSET,*DCLR;

/* --- line primitives (match the driver's push-pull TX / input RX) --- */
static inline void data_out(int v){ *DSET=BIT_DATA; if(v) *OSET=BIT_DATA; else *OCLR=BIT_DATA; }
static inline void data_in(void){ *DCLR=BIT_DATA; }          /* release for reply */
static inline int  data_read(void){ return !!(*INP & BIT_DATA); }
static inline void clk(int v){ if(v) *OSET=BIT_CLCK; else *OCLR=BIT_CLCK; }

/* half-clock delay */
static long STEP_NS = 10000L*1000L;   /* 10 ms default */
static inline void half(void){
    struct timespec ts={ STEP_NS/1000000000L, STEP_NS%1000000000L };
    nanosleep(&ts,NULL);
}

/* one bit-time: drive DATA (or leave released), then a clock edge, then wait.
 * clock phase alternates every call, exactly like the driver's per-step tail. */
static int phase=1;                                   /* first edge rises */
static void step_tx(int bit){ data_out(bit); clk(phase); phase^=1; half(); }
static int  step_rx(void){ int b; clk(phase); phase^=1; half(); b=data_read(); return b; }

/* build the 16-byte challenge frame: 4-byte counter nonce expanded buf[i]=buf[i&3]+i */
static void build_frame(uint8_t f[16], uint32_t nonce){
    f[0]=nonce; f[1]=nonce>>8; f[2]=nonce>>16; f[3]=nonce>>24;
    for(int i=4;i<16;i++) f[i]=f[i&3]+i;
}

/* CRC-16, MSB-first, nibble-wise, poly 0x2341 (matches the driver's table);
 * kept so -v can show the value the PIC's reply is expected to equal (seed 0x5555). */
static uint16_t crc16(const uint8_t *d,int n,uint16_t crc){
    for(int i=0;i<n;i++){
        crc ^= (uint16_t)d[i]<<8;
        for(int b=0;b<8;b++) crc = (crc&0x8000)?(crc<<1)^0x2341:(crc<<1);
    }
    return crc;
}

int main(int argc,char**argv){
    int idle=0, verbose=0;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--step-us")&&i+1<argc) STEP_NS=atol(argv[++i])*1000L;
        else if(!strcmp(argv[i],"--idle")) idle=1;
        else if(!strcmp(argv[i],"-v")) verbose=1;
    }
    kfd=open("/dev/kmsg",O_WRONLY|O_CLOEXEC);
    klog("wb-picwdt: starting step=%ldus %s\n",STEP_NS/1000,idle?"idle":"full");
    int fd=open("/dev/mem",O_RDWR|O_SYNC);
    if(fd<0){perror("/dev/mem"); klog("wb-picwdt: FATAL open /dev/mem failed\n"); return 1;}
    volatile uint8_t*g=mmap(0,0x1000,PROT_READ|PROT_WRITE,MAP_SHARED,fd,GPIO_BASE);
    if(g==MAP_FAILED){perror("mmap"); klog("wb-picwdt: FATAL mmap 0x40028000 failed\n"); return 1;}
    klog("wb-picwdt: mmap ok, feeding P0.6/P0.7\n");
    INP=(void*)(g+0x40); OSET=(void*)(g+0x44); OCLR=(void*)(g+0x48);
    DSET=(void*)(g+0x50); DCLR=(void*)(g+0x54);

    /* init: both lines driven output-low, like the driver's probe */
    *DSET=BIT_CLCK|BIT_DATA; *OCLR=BIT_CLCK|BIT_DATA;
    fprintf(stderr,"picwdt-keepalive: step=%ldus %s\n",STEP_NS/1000,idle?"(idle-strobe)":"(full protocol)");

    uint32_t nonce=0x1a2b3c4d; uint16_t roll=0x7833; uint32_t frame=0;
    for(;;){
        if(idle){
            /* state 0: strobe CLCK + shift a rolling 16-bit pattern on DATA */
            for(int b=15;b>=0;b--) step_tx((roll>>b)&1);
            roll = (uint16_t)((roll<<1) | ((nonce>>15)&1)) & 0x7b7d;
            nonce = nonce*1664525u + 1013904223u;      /* keep it changing */
            if(verbose) klog("wb-picwdt: idle frame=%u roll=%04x\n",frame++,roll);
            continue;
        }
        uint8_t f[16]; build_frame(f,nonce);
        /* state 2: 16-bit sync 0x7FFE, MSB-first */
        for(int b=15;b>=0;b--) step_tx((0x7ffe>>b)&1);
        /* state 3: 8-bit sync 0x70, MSB-first */
        for(int b=7;b>=0;b--)  step_tx((0x70>>b)&1);
        /* state 4: 16 data bytes, each MSB-first */
        for(int i=0;i<16;i++) for(int b=7;b>=0;b--) step_tx((f[i]>>b)&1);
        /* state 5+6: release DATA, clock in 16 reply bits */
        data_in();
        uint16_t reply=0; for(int i=0;i<16;i++) reply=(reply<<1)|step_rx();
        /* state 7: drive DATA low, idle one bit-time, then next frame */
        data_out(0); step_tx(0);
        klog("wb-picwdt: frame=%u nonce=%08x reply=%04x crc5555=%04x\n",
             frame++,nonce,reply,crc16(f,16,0x5555));
        if(verbose) fprintf(stderr,"frame nonce=%08x reply=%04x expect(crc5555)=%04x\n",
                            nonce,reply,crc16(f,16,0x5555));
        nonce = nonce*1664525u + 1013904223u;
    }
    return 0;
}
