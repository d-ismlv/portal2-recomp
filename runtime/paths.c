#include "posix.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char game_root[PATH_MAX], linux_root[PATH_MAX], user_root[PATH_MAX];
static _Thread_local char buffers[4][PATH_MAX];
static _Thread_local unsigned next_buffer;
static char *buffer(void) { return buffers[next_buffer++ % 4]; }
static int within(const char *path,const char *root) {
    size_t length=strlen(root);
    return length && !strncmp(path,root,length) && (!path[length] || path[length]=='/');
}
static int absolute_path(const char *path,char *output) {
    char joined[PATH_MAX]; int count;
    if (path[0]=='/') count=snprintf(joined,sizeof(joined),"%s",path);
    else {
        char cwd[PATH_MAX]; if (!getcwd(cwd,sizeof(cwd))) return -1;
        count=snprintf(joined,sizeof(joined),"%s/%s",cwd,path);
    }
    if(count<0 || count>=(int)sizeof(joined)){errno=ENAMETOOLONG;return -1;}
    size_t length=1; output[0]='/'; output[1]=0;
    char *save=NULL;
    for(char *part=strtok_r(joined,"/",&save);part;part=strtok_r(NULL,"/",&save)) {
        if(!strcmp(part,"."))continue;
        if(!strcmp(part,"..")){
            if(length>1){while(length>1 && output[length-1]!='/')--length;if(length>1)--length;output[length]=0;}
            continue;
        }
        size_t add=strlen(part);
        if(length+(length>1)+add>=PATH_MAX){errno=ENAMETOOLONG;return -1;}
        if(length>1)output[length++]='/';
        memcpy(output+length,part,add+1); length+=add;
    }
    // Resolve existing prefixes too: /tmp is /private/tmp on macOS, including
    // when the requested file has not been created yet.
    char prefix[PATH_MAX], resolved[PATH_MAX]; strcpy(prefix,output);
    size_t cut=strlen(prefix);
    while(!realpath(prefix,resolved)) {
        if(cut<=1) return -1;
        while(cut>1 && prefix[cut-1]!='/')--cut;
        if(cut>1)--cut;
        prefix[cut]=0;
    }
    const char *suffix=output+cut;
    char combined[PATH_MAX];
    int total=snprintf(combined,sizeof(combined),"%s%s",resolved,suffix);
    if(total<0 || total>=PATH_MAX){errno=ENAMETOOLONG;return -1;}
    strcpy(output,combined);
    return 0;
}
int rt_paths_init(const char *game,const char *linux,const char *user) {
    if(!realpath(game,game_root) || !realpath(linux,linux_root))return -1;
    if(absolute_path(user,user_root))return -1;
    if(mkdir(user_root,0755) && errno!=EEXIST)return -1;
    return 0;
}
const char *rt_read_path(const char *path) {
    if(!game_root[0])return path;
    char canonical[PATH_MAX]; if(absolute_path(path,canonical))return NULL;
    if(!within(canonical,game_root))return path;
    const char *relative=canonical+strlen(game_root); while(*relative=='/')++relative;
    char *result=buffer();
    int count=snprintf(result,PATH_MAX,"%s/%s",user_root,relative);
    if(count<0 || count>=PATH_MAX){errno=ENAMETOOLONG;return NULL;}
    struct stat info;
    if(!stat(result,&info) && (!S_ISDIR(info.st_mode) || access(canonical,F_OK)))return result;
    // Supprimé par l'invité : la copie utilisateur absente donne ENOENT.
    if(lstat(result,&info) && rt_path_is_whiteout(relative)==1)return result;
    count=snprintf(result,PATH_MAX,"%s/%s",linux_root,relative);
    if(count<0 || count>=PATH_MAX){errno=ENAMETOOLONG;return NULL;}
    if(!stat(result,&info) && (!S_ISDIR(info.st_mode) || access(canonical,F_OK)))return result;
    const char *base=strrchr(relative,'/');
    if(base && strstr(relative,"bin/")) {
        const char *directory=!strncmp(relative,"portal2/",8)?"portal2/bin/linux32":"bin/linux32";
        count=snprintf(result,PATH_MAX,"%s/%s/%s",linux_root,directory,base+1);
        if(count>=0 && count<PATH_MAX && !stat(result,&info))return result;
    }
    return path;
}
#define WHITEOUTS RT_WHITEOUTS
static _Thread_local char parent_buffer[PATH_MAX];
static const char *relative_parent(const char *relative,size_t length) {
    if(length>=PATH_MAX)return NULL;
    memcpy(parent_buffer,relative,length);parent_buffer[length]=0;return parent_buffer;
}
// Répertoire présent dans une source originale et non supprimé par l'invité.
static int lower_directory(const char *relative) {
    if(rt_path_is_whiteout(relative)==1)return 0;
    char lower[2][PATH_MAX];int count=rt_path_lower(relative,lower);
    struct stat info;
    for(int i=0;i<count;++i)if(!stat(lower[i],&info) && S_ISDIR(info.st_mode))return 1;
    return 0;
}
int rt_path_relative(const char *path,char relative[PATH_MAX]) {
    if(!game_root[0])return 0;
    char canonical[PATH_MAX]; if(absolute_path(path,canonical))return -1;
    if(!within(canonical,game_root))return 0;
    const char *r=canonical+strlen(game_root); while(*r=='/')++r;
    strcpy(relative,r); return 1;
}
static int join(char out[PATH_MAX],const char *root,const char *relative) {
    int count=*relative?snprintf(out,PATH_MAX,"%s/%s",root,relative):snprintf(out,PATH_MAX,"%s",root);
    if(count<0 || count>=PATH_MAX){errno=ENAMETOOLONG;return -1;}
    return 0;
}
int rt_path_overlay(const char *relative,char out[PATH_MAX]) { return join(out,user_root,relative); }
int rt_path_lower(const char *relative,char out[2][PATH_MAX]) {
    int count=0; struct stat info;
    if(!join(out[count],linux_root,relative) && !lstat(out[count],&info))++count;
    if(!join(out[count],game_root,relative) && !lstat(out[count],&info))++count;
    return count;
}
static int whiteout_marker(const char *relative,char out[PATH_MAX]) {
    int count=*relative?snprintf(out,PATH_MAX,"%s/" WHITEOUTS "/%s/.wh",user_root,relative):-1;
    if(count<0 || count>=PATH_MAX){errno=*relative?ENAMETOOLONG:EINVAL;return -1;}
    return 0;
}
int rt_path_is_whiteout(const char *relative) {
    char marker[PATH_MAX]; if(whiteout_marker(relative,marker))return -1;
    return !access(marker,F_OK);
}
int rt_path_whiteout(const char *relative) {
    char marker[PATH_MAX]; if(whiteout_marker(relative,marker))return -1;
    for(char *p=marker+strlen(user_root)+1;*p;++p)if(*p=='/'){
        *p=0; int error=mkdir(marker,0755); *p='/'; if(error && errno!=EEXIST)return -1;
    }
    FILE *file=fopen(marker,"wb"); if(!file)return -1;
    return fclose(file)?-1:0;
}
int rt_path_clear_whiteout(const char *relative) {
    char marker[PATH_MAX]; if(whiteout_marker(relative,marker))return -1;
    return unlink(marker) && errno!=ENOENT?-1:0;
}
int rt_path_hidden_name(const char *relative_dir,const char *name) {
    if(!*relative_dir && !strcmp(name,WHITEOUTS))return 1;
    char relative[PATH_MAX];
    int count=*relative_dir?snprintf(relative,PATH_MAX,"%s/%s",relative_dir,name):snprintf(relative,PATH_MAX,"%s",name);
    if(count<0 || count>=PATH_MAX)return 0;
    return rt_path_is_whiteout(relative)==1;
}
int rt_guest_realpath(const char *path,char out[PATH_MAX]) {
    if(!*path){errno=ENOENT;return -1;}
    if(absolute_path(path,out))return -1;
    const char *resolved=rt_read_path(out); struct stat info;
    if(!resolved || stat(resolved,&info))return -1;
    return 0;
}
const char *rt_write_path(const char *path,int preserve_existing) {
    if(!game_root[0])return path;
    char canonical[PATH_MAX]; if(absolute_path(path,canonical))return NULL;
    if(!within(canonical,game_root))return path;
    const char *relative=canonical+strlen(game_root); while(*relative=='/')++relative;
    char *result=buffer(); int count=snprintf(result,PATH_MAX,"%s/%s",user_root,relative);
    if(count<0 || count>=PATH_MAX){errno=ENAMETOOLONG;return NULL;}
    // Ne recrée dans la copie utilisateur que les répertoires parents visibles
    // dans la vue fusionnée : sinon ENOENT, comme sous Linux.
    for(char *p=result+strlen(user_root)+1;*p;++p)if(*p=='/'){
        *p=0;
        struct stat info;
        if(stat(result,&info)) {
            const char *parent=relative_parent(relative,(size_t)(p-(result+strlen(user_root)+1)));
            if(!parent || !lower_directory(parent)){*p='/';errno=ENOENT;return NULL;}
            if(mkdir(result,0755) && errno!=EEXIST){*p='/';return NULL;}
        } else if(!S_ISDIR(info.st_mode)){*p='/';errno=ENOTDIR;return NULL;}
        *p='/';
    }
    if(preserve_existing && access(result,F_OK)){
        const char *existing=rt_read_path(canonical);
        FILE *input=existing?fopen(existing,"rb"):NULL;
        if(input){
            FILE *output=fopen(result,"wb");
            if(!output){fclose(input);return NULL;}
            char block[65536];size_t size;int failed=0;
            while((size=fread(block,1,sizeof(block),input)))if(fwrite(block,1,size,output)!=size){failed=1;break;}
            if(ferror(input))failed=1;
            fclose(input);if(fclose(output))failed=1;
            if(failed)return NULL;
        }
    }
    return result;
}
const char *rt_program_path(void) {
    char *result=buffer();snprintf(result,PATH_MAX,"%s/portal2_linux",game_root);return result;
}
