#ifndef LIBC_DLFCN_H
#define LIBC_DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RTLD_LAZY
#define RTLD_LAZY   0x0001
#endif
#ifndef RTLD_NOW
#define RTLD_NOW    0x0002
#endif
#ifndef RTLD_GLOBAL
#define RTLD_GLOBAL 0x0100
#endif
#ifndef RTLD_LOCAL
#define RTLD_LOCAL  0x0000
#endif

void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
int dlclose(void *handle);
char *dlerror(void);

#ifdef __cplusplus
}
#endif

#endif
