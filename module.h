#ifndef _MODULE_H_
#define _MODULE_H_

#include <string>

struct module {
    int identifier;
    std::string (*get_next_item)(void);
    bool (*parseplatform)(const std::string& path,std::string& platform);
    bool (*parseversion)(const std::string& path,std::string& version);
    void (*cerr_output)(const std::ostringstream& data);
    void (*cout_output)(const std::ostringstream& data);

    // use this when opening files so threadhelper can accumulate
    // statistics on number of files opened to help system-wide profiling
    int (*openfile)(const char*);
    void* (*filemalloc)(int fd,char** ep);
    void (*filefree)(void* memory);
    int (*closefile)(int fd);
};

#endif
