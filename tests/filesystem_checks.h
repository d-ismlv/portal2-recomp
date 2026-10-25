#include <stdlib.h>
#include <sys/stat.h>
#include <limits.h>

static void put_fixture(const char *path,const char *contents) {
    FILE *file=fopen(path,"wb"); assert(file);
    assert(fwrite(contents,1,strlen(contents),file)==strlen(contents));
    assert(!fclose(file));
}
static void check_filesystem(void) {
    char root[]="/tmp/portal2-path-test-XXXXXX", path[PATH_MAX];
    assert(mkdtemp(root));
    char game[PATH_MAX],linux[PATH_MAX],user[PATH_MAX];
    snprintf(game,sizeof(game),"%s/game",root); assert(!mkdir(game,0755));
    snprintf(linux,sizeof(linux),"%s/linux",root); assert(!mkdir(linux,0755));
    snprintf(user,sizeof(user),"%s/user",root);
    snprintf(path,sizeof(path),"%s/config.cfg",game); put_fixture(path,"original");
    char source[PATH_MAX]; strcpy(source,path);
    snprintf(path,sizeof(path),"%s/launcher.so",linux); put_fixture(path,"ELF");
    assert(!rt_paths_init(game,linux,user));
    char canonical[PATH_MAX]; assert(realpath(game,canonical));
    assert(!strcmp(rt_read_path(game),game)); // overlay root cannot hide assets
    snprintf(path,sizeof(path),"%s/launcher.so",game);
    const char *read=rt_read_path(path); assert(strstr(read,"/linux/launcher.so"));
    const char *write=rt_write_path(source,1); assert(write && strstr(write,"/user/config.cfg"));
    FILE *file=fopen(write,"rb"); assert(file); char contents[16]={0};
    assert(fread(contents,1,sizeof(contents),file)==8); fclose(file); assert(!strcmp(contents,"original"));
    put_fixture(write,"changed"); assert(!strcmp(rt_read_path(source),write));
    file=fopen(source,"rb"); assert(file); memset(contents,0,sizeof(contents));
    assert(fread(contents,1,sizeof(contents),file)==8); fclose(file); assert(!strcmp(contents,"original"));
    CPU c; rt_cpu_reset(&c); c.esp=0x20010000;
    strcpy(GPTR(0x20009000),source);
    W32(c.esp+4,3);W32(c.esp+8,0x20009000);W32(c.esp+12,0x2000a000);
    assert(rt_stat_shim(&c,"__xstat64") && !c.eax);
    assert(R64(0x2000a000+44)==7 && (R32(0x2000a000+16)&0170000)==0100000);
    assert(R32(0x2000a000+52)>0 && R64(0x2000a000+88)>0);
    W32(c.esp+4,3);assert(rt_stat_shim(&c,"__xstat") && !c.eax);
    assert(R32(0x2000a000+44)==7);
    strcpy(GPTR(0x20009100),"rb");
    W32(c.esp+4,0x20009000);W32(c.esp+8,0x20009100);
    assert(rt_libc_shim(&c,"fopen") && c.eax);uint32_t stream=c.eax;
    W32(c.esp+4,stream);W32(c.esp+8,(uint32_t)-2);W32(c.esp+12,SEEK_END);
    assert(rt_libc_shim(&c,"fseek") && !c.eax);
    assert(rt_libc_shim(&c,"ftell") && c.eax==5);
    W32(c.esp+8,5);W32(c.esp+12,1);W32(c.esp+16,SEEK_SET);
    assert(rt_libc_shim(&c,"fseeko64") && !c.eax);
    assert(rt_libc_shim(&c,"ftello64") && c.eax==5 && c.edx==1);
    assert(rt_libc_shim(&c,"ftell") && c.eax==UINT32_MAX);
    assert(rt_libc_shim(&c,"__errno_location") && R32(c.eax)==75);
    assert(rt_libc_shim(&c,"rewind"));
    assert(rt_libc_shim(&c,"ftell") && !c.eax);
    for(unsigned i=0;i<7;++i)assert(rt_libc_shim(&c,"getc") && c.eax==(uint32_t)(unsigned char)"changed"[i]);
    assert(rt_libc_shim(&c,"getc") && c.eax==UINT32_MAX);
    assert(rt_libc_shim(&c,"feof") && c.eax);
    assert(rt_libc_shim(&c,"ferror") && !c.eax);
    assert(rt_libc_shim(&c,"fclose") && !c.eax);
    strcpy(GPTR(0x20009100),"aa/bb/cc");
    W32(c.esp+4,0x20009100);W32(c.esp+8,'/');
    assert(rt_libc_shim(&c,"strrchr") && c.eax==0x20009105);
    W32(c.esp+4,0x20009200);W32(c.esp+8,0x20009100);
    assert(rt_shim(&c,"stpcpy") && c.eax==0x20009208 && !strcmp(GPTR(0x20009200),"aa/bb/cc"));
    assert(!unlink(write)); assert(!rmdir(user));
    assert(!unlink(source));assert(!rmdir(game));
    snprintf(path,sizeof(path),"%s/launcher.so",linux);assert(!unlink(path));
    assert(!rmdir(linux));assert(!rmdir(root));
}
