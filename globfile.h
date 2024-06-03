#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <string.h>
#include <deque>
#include <unistd.h>
#include <stdlib.h>
#include <glob.h>
#include <assert.h>
#include <regex.h>
#include <sys/types.h>
#include "module.h"

using namespace std;

struct HandleMatch {
    HandleMatch(struct module* me_) : me(me_) {}
    virtual const void operator()(const std::string& path,
                                  const std::string& match,
                                  const char* globpath) const=0;
    virtual ~HandleMatch() {} 
    struct module* me;
};

struct CoutMatchHandle : HandleMatch {
    CoutMatchHandle(struct module* me_) : HandleMatch(me_) {}
    const void operator()(const std::string& path,const std::string& match,
                    const char* globpath) const {
       std::string platform;
       std::string version;
       me->parseplatform(path,platform);
       me->parseversion(path,version);
                  
       std::ostringstream r;
       r << path << "|" << globpath+path.size() 
         << "|" << match << "|" << 
         platform << "|" << version << "|" << endl;
       me->cout_output(r);
    }
};

struct Match {
    Match(const char* matchstr_,const char* sp_,const char* ep_,std::string& path_,const char* globpath_) : matchstr(matchstr_),sp(sp_),ep(ep_),path(path_),globpath(globpath_) {}

    virtual void operator()(const HandleMatch&)=0;

    ~Match() {}

    const char* matchstr;
    const char* sp;
    const char* ep;
    std::string path;
    const char* globpath;
};


  // match on a line by line basis 
struct RegexMatch : Match {
    RegexMatch(const char* matchstr_,const char* sp_,const char* ep_,std::string& path_,const char* globpath_) : Match(matchstr_,sp_,ep_,path_,globpath_) {
        assert(regcomp(&regex_,matchstr_,REG_EXTENDED | REG_NEWLINE) == 0);
    }

    virtual ~RegexMatch() {
        regfree(&regex_);
    }

    void operator()(const HandleMatch& handlematch) {
        const char* cp = sp;
        next_match:
        regmatch_t matches = {0};
        if(regexec(&regex_,cp,1,&matches,0) != REG_NOMATCH) {

                 std::string match(cp+(matches.rm_so),
                                   cp+(matches.rm_eo));
                 handlematch(path,match,globpath);
                 cp = cp + matches.rm_eo;
                 if(cp < ep) {
                     goto next_match;
                 }
        }
    }
    regex_t regex_;
};

template <const char* matchstr,const char* globstr,typename T>
void* worker_(const HandleMatch& handlematch) {
  bool done = false;
  while(! done) {
    std::string path = handlematch.me->get_next_item();
    if(path.length() == 0) {
         done = true;
         continue;
    }

    glob_t glob_;
    memset(&glob_,0,sizeof(glob_t));
    std::string globpath = path + globstr;
    if(glob(globpath.c_str(),GLOB_DOOFFS,0,&glob_) == 0) {
        for(int i = 0; i < glob_.gl_pathc;i++) {

	   // use this to accumulate stats about number of opens and failures
           int fd = handlematch.me->openfile(glob_.gl_pathv[i]);
           if(fd >= 0) {
              char* ep = 0;
              char* data = (char*)handlematch.me->filemalloc(fd,&ep); 

              if(data != 0) {
                 T matcher(matchstr,data,ep,path,glob_.gl_pathv[i]);
                 matcher(handlematch);
                 handlematch.me->filefree(data);
              }
              handlematch.me->closefile(fd);
           } else {
               std::ostringstream r;
               r << "Failed opening:  " << path << "/" << glob_.gl_pathv[i] <<
                   endl;
               handlematch.me->cerr_output(r);
           }
         }
         globfree(&glob_);
     }
    } 
    return EXIT_SUCCESS;
}
