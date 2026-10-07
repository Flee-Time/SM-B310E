int open(const char *name,int flags,...) {
    if(!name)return -1;
    int access=flags&O_ACCMODE;
    if(access!=O_RDONLY && access!=O_WRONLY && access!=O_RDWR)return -1;
    FILE *file=NULL;
    if(flags&O_APPEND)file=fopen(name,access==O_RDWR?"a+b":"ab");
    else if(flags&O_TRUNC)file=fopen(name,access==O_RDWR?"w+b":"wb");
    else {
        file=fopen(name,access==O_RDONLY?"rb":"r+b");
        if(!file && (flags&O_CREAT))file=fopen(name,access==O_RDWR?"w+b":"wb");
    }
    return file?file2fd(file):-1;
}
