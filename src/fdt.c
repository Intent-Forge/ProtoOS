#include "platform.h"

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static int eq(const char *a, const char *b) {
    while (*a && *a==*b) { ++a; ++b; }
    return *a==*b;
}
static int prefix(const char *a, const char *b) {
    while (*b) if (*a++!=*b++) return 0;
    return 1;
}
static size_t bounded_len(const unsigned char *p,size_t n) {
    size_t i=0; while(i<n && p[i]) ++i; return i;
}
static int range(uint32_t off,uint32_t len,uint32_t total) {
    return off<=total && len<=total-off;
}
int parse_dtb(const void *blob, size_t available, struct boot_config *out) {
    const unsigned char *b=blob;
    if (!b || !out || available<40 || be32(b)!=0xd00dfeed) return -1;
    uint32_t total=be32(b+4), so=be32(b+8), no=be32(b+12);
    uint32_t ns=be32(b+32), ss=be32(b+36);
    if (total>available || total<40 || be32(b+20)<17 || be32(b+24)>17 ||
        !range(so,ss,total) || !range(no,ns,total) || so<40 || no<40 ||
        (so&3) || (so<no+ns && no<so+ss)) return -2;
    const unsigned char *p=b+so, *end=p+ss, *names=b+no;
    unsigned depth=0; unsigned kind[16]={0};
    unsigned char uart_reg[16]={0}; unsigned uart_len=0; int uart_ok=0;
    int found_uart=0,found_cpus=0,root_closed=0;
    unsigned root_cells=0,soc_cells=0; int soc_identity=0;
    out->uart=0; out->timebase_hz=0; out->bootargs[0]=0;
    out->initrd_start=0; out->initrd_end=0; out->initrd_found=0;
    while ((size_t)(end-p)>=4) {
        uint32_t token=be32(p); p+=4;
        if (token==1) {
            if (depth>=16 || root_closed) return -3;
            size_t n=bounded_len(p,(size_t)(end-p));
            if (n==(size_t)(end-p)) return -3;
            size_t skip=(n+1+3)&~(size_t)3;
            if (skip>(size_t)(end-p)) return -3;
            const char *name=(const char *)p;
            unsigned k=0;
            if(depth==0 && n!=0) return -3;
            if(depth==1 && eq(name,"chosen")) k=1;
            if(depth==1 && eq(name,"cpus")) k=2;
            if(depth==1 && eq(name,"soc")) k=3;
            if(depth==2 && kind[1]==3 && prefix(name,"serial@")) {
                k=4; uart_len=0; uart_ok=0;
            }
            kind[depth++]=k; p+=skip;
        } else if (token==2) {
            if (!depth) return -4;
            if (kind[depth-1]==4 && uart_ok && uart_len==16) {
                if (found_uart || be32(uart_reg)!=0 || be32(uart_reg+8)!=0 ||
                    be32(uart_reg+12)<8) return -4;
                out->uart=be32(uart_reg+4); found_uart=1;
            }
            --depth; if(!depth) root_closed=1;
        } else if (token==3) {
            if (!depth || (size_t)(end-p)<8) return -5;
            uint32_t len=be32(p), off=be32(p+4); p+=8;
            if (off>=ns || bounded_len(names+off,ns-off)==ns-off ||
                len>(size_t)(end-p)) return -5;
            size_t skip=((size_t)len+3)&~(size_t)3;
            if (skip>(size_t)(end-p)) return -5;
            const char *name=(const char *)(names+off);
            unsigned k=kind[depth-1];
            if(depth==1 || k==3) {
                unsigned bit=eq(name,"#address-cells")?1: eq(name,"#size-cells")?2:0;
                if(bit) {
                    if(len!=4 || be32(p)!=2) return -6;
                    if(depth==1) root_cells|=bit; else soc_cells|=bit;
                }
                if(k==3 && eq(name,"ranges")) {
                    if(len) return -6;
                    soc_identity=1;
                }
            }
            if(k==1 && eq(name,"bootargs")) {
                size_t n=bounded_len(p,len);
                if(n==len || n>=sizeof(out->bootargs)) return -6;
                for(size_t i=0;i<=n;++i) out->bootargs[i]=(char)p[i];
            }
            /* v1.4: program archive loaded by the boot loader (QEMU -initrd).
             * QEMU 8 writes one cell, QEMU 10 two; anything else is malformed. */
            if(k==1 && (eq(name,"linux,initrd-start") || eq(name,"linux,initrd-end"))) {
                uint32_t bit=eq(name,"linux,initrd-start")?1u:2u;
                if((len!=4 && len!=8) || (out->initrd_found&bit)) return -6;
                uint64_t v=be32(p);
                if(len==8) v=(v<<32)|be32(p+4);
                if(bit==1u) out->initrd_start=v; else out->initrd_end=v;
                out->initrd_found|=bit;
            }
            if(k==2 && eq(name,"timebase-frequency")) {
                if(len!=4 || !be32(p) || found_cpus) return -6;
                out->timebase_hz=be32(p); found_cpus=1;
            }
            if(k==4 && eq(name,"reg")) {
                if(len!=16) return -6;
                for(unsigned i=0;i<16;++i) uart_reg[i]=p[i];
                uart_len=len;
            }
            if(k==4 && eq(name,"compatible")) {
                size_t i=0;
                while(i<len) {
                    size_t n=bounded_len(p+i,len-i);
                    if(n==len-i) return -6;
                    if(eq((const char *)(p+i),"ns16550a")) uart_ok=1;
                    i+=n+1;
                }
            }
            p+=skip;
        } else if(token==4) {
            /* FDT_NOP */
        } else if(token==9) {
            return (!depth && root_closed && found_uart && found_cpus &&
                    root_cells==3 && soc_cells==3 && soc_identity) ? 0 : -7;
        } else return -8;
    }
    return -9;
}
