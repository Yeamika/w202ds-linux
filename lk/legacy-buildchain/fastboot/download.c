/* SPDX-License-Identifier: MIT
 * Standalone raw download: never invokes the native sparse streaming writer.
 */
#include <stdint.h>
void lk_debug_download_raw(const char *arg, void *unused, uint64_t size)
{
    volatile uint64_t *const image=(void *)0xb5122048;
    void (*const reply)(const char *,const char *)=(void *)0xb500e11c;
    int (*const tx)(const void *,unsigned)=(void *)0xb5081864;
    int (*const rx)(void *,unsigned)=(void *)0xb5081638;
    char response[12]={'D','A','T','A'};
    uint64_t bytes=0;
    (void)unused;(void)size;
    image[2]=0;
    for(unsigned i=0;i<8;++i) {
        char c=arg[i];unsigned v=c>='0'&&c<='9'?c-'0':
            c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;
        if(v==16) {reply("FAIL","invalid download size");return;}
        bytes=bytes*16+v;response[4+i]="0123456789abcdef"[v];
    }
    if(arg[8] || !bytes || bytes>image[1] || bytes>0x10000000) {
        reply("FAIL","download exceeds native RAM buffer");return;
    }
    if(tx(response,sizeof(response))!=(int)sizeof(response) ||
       rx((void *)(uintptr_t)image[0],bytes)!=(int)bytes) {
        reply("FAIL","incomplete RAM download");return;
    }
    image[2]=bytes;
    reply("OKAY","");
}
