#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <string.h>
#include <deque>
#include <set>
#include <map>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "module.h"

using namespace std;

static deque< string > paths;
static pthread_mutex_t pathmtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t coutmtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cerrmtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t numfilesmtx = PTHREAD_MUTEX_INITIALIZER;
static unsigned int numfiles_opened_success;
static unsigned int numfiles_opened_fail;
static pthread_mutex_t memorymtx = PTHREAD_MUTEX_INITIALIZER;
static set<char*> regularmemory;
static map<void*,size_t> mmapmemory;
static uint64_t bytes_scanned;

// 0-1k,1k-4k,4k-16k,16k-64k,64k-256k,>256k
static unsigned int file_sz_histogram[6];

static bool
parseline(const std::string& path,const std::string& filesuffix,
          const std::string& parseprefix,std::string& outstring) {
   std::string filename(path);
   filename.append(filesuffix);
   std::ifstream file(filename.c_str());
   while(file.good()) {
       std::string line;
       getline(file,line);
       if(line.substr(0,parseprefix.size()) == parseprefix) {
           outstring.append(line.substr(parseprefix.size()));
           return true;
       }
   }
   return false;
}

static bool
parseplatform(const std::string& path,std::string& platform) {
   return parseline(path,"/PLATFORM","platform=",platform);
}

static bool
parseversion(const std::string& path,std::string& version) {
   return parseline(path,"/VERSION","Version: ",version);
}

static string
get_next_item(void) {
    string path;
    pthread_mutex_lock(&pathmtx);
    if(! paths.empty()) {
      path = paths.front();
      paths.pop_front();
    }
    pthread_mutex_unlock(&pathmtx);
    return path;
}

static void
synchronize_stream(std::ostream& outstream,pthread_mutex_t* mtx,
                   const std::ostringstream& data) {
    pthread_mutex_lock(mtx);
    outstream << data.str();
    pthread_mutex_unlock(mtx);
}

static void
cerr_output(const std::ostringstream& data) {
   synchronize_stream(std::cerr,&cerrmtx,data);
}

static void
cout_output(const std::ostringstream& data) {
   synchronize_stream(std::cout,&coutmtx,data);
}

static int
openfile(const char* filename) {
    pthread_mutex_lock(&numfilesmtx);
    int fd = open(filename,O_RDONLY);
    fd >= 0 ? numfiles_opened_success++ : numfiles_opened_fail++;
    pthread_mutex_unlock(&numfilesmtx);
    return fd;
}

static void
update_file_sz_histogram(size_t sz) {
   if(sz <= (1 << 10))
      (*file_sz_histogram)++;
   else if(sz <= (1 << 12))
      (*(file_sz_histogram + 1))++;
   else if(sz <= (1 << 14))
      (*(file_sz_histogram + 2))++;
   else if(sz <= (1 << 16))
      (*(file_sz_histogram + 3))++;
   else if(sz <= (1 << 18))
      (*(file_sz_histogram + 4))++;
   else
      (*(file_sz_histogram + 5))++;
}

static void*
filemalloc(int fd,char** ep) {
     struct stat sb;
     if(fstat(fd,&sb) != -1 and sb.st_size != 0) {
          char* data = (char*)mmap(0,sb.st_size,PROT_READ,MAP_SHARED,fd,0);
          char* copy_ = 0;
          if(data != (char*)MAP_FAILED) {
              if((sb.st_size % getpagesize()) == 0) {
                  copy_ = new char[sb.st_size + 1];
                  copy(data,data + sb.st_size,copy_);
                  *(copy_ + sb.st_size) = 0;
                  munmap(data,sb.st_size);
                  *ep = copy_+sb.st_size;
                  pthread_mutex_lock(&memorymtx);
                  regularmemory.insert(copy_);
                  update_file_sz_histogram(sb.st_size);
                  bytes_scanned += sb.st_size;
                  pthread_mutex_unlock(&memorymtx);
                  return copy_;
              }
              else {
                  pthread_mutex_lock(&memorymtx);
                  mmapmemory[data] = sb.st_size;
                  *ep = data+sb.st_size;
                  bytes_scanned += sb.st_size;
                  update_file_sz_histogram(sb.st_size);
                  pthread_mutex_unlock(&memorymtx);
                  return data;
              }
          }
     }
     return 0;
}

static void
filefree(void* memory) {
    pthread_mutex_lock(&memorymtx);

    map<void*,size_t>::iterator iter = mmapmemory.find(memory);
    if(iter == mmapmemory.end()) {
        set<char*>::iterator iter2 = regularmemory.find((char*)memory);
        if(iter2 != regularmemory.end()) {
            delete[] (char*)memory;
            regularmemory.erase(iter2);
        }        
    }
    else {
        munmap(iter->first,iter->second);
        mmapmemory.erase(iter);
    }
    pthread_mutex_unlock(&memorymtx);
}

static int
closefile(int fd) {
    return close(fd);
}

static int
usage(const char* me) {
    cerr << me << " -f workermodule [-t numthreads default: _NPROCESSORS_ONLN * 2] [-s stacksize_in_kb default:  10240]" << endl;
    return(EXIT_FAILURE);
}

int main(int argc,char* argv[]) {

   int ch;
   long numthreads = sysconf(_SC_NPROCESSORS_ONLN) * 2;
   size_t stacksize = 0;
   void* module = 0;
   void* (*worker)(void*) = 0;

   while((ch = getopt(argc,argv,"t:s:f:")) != -1) {
       switch(ch) {
       case 't':
         {
           stringstream r(optarg);
           r >> numthreads;
         }
           break;
       case 's':
         {
           stringstream r(optarg);
           r >> stacksize;
           stacksize <<= 10;  // deal w/overflow?
         }
         break;
       case 'f':
         {
             if((module = dlopen(optarg,RTLD_NOW)) == 0) {
                 cerr << "Failed loading(" << optarg << "):  " << dlerror() << endl;
                 return EXIT_FAILURE;  
             }
             if((worker = (void* (*)(void*))dlsym(module,"worker")) == 0) {
                 cerr << "Failed finding:  " << dlerror() << endl;
                 return EXIT_FAILURE;  
             }
             break;       
         }
       default:
           return usage(argv[0]);
       } 
   }  
   
   if(module == 0)
       return usage(argv[0]);

   pthread_t threads[numthreads];
   pthread_attr_t attrs[numthreads];
     
   memset(threads,0,numthreads*sizeof(pthread_t));
   memset(attrs,0,numthreads*sizeof(pthread_attr_t)); 

   while(cin.good()) {
       string line;
       getline(cin,line);
       paths.push_back(line);
   }

  for(int i = 0;i < numthreads;i++) {
       struct module me = {0};
       me.identifier = i;
       me.get_next_item = get_next_item;
       me.parseplatform = parseplatform;
       me.parseversion = parseversion;
       me.cerr_output = cerr_output;
       me.cout_output = cout_output;
       me.openfile = openfile;
       me.filemalloc = filemalloc;
       me.filefree = filefree;
       me.closefile = closefile;

       if(stacksize != 0)pthread_attr_setstacksize(attrs+i,stacksize);
       pthread_create(threads+i,attrs+i,worker,&me);
  }
  
  struct timeval begin = {0};
  struct timeval end = {0};
  gettimeofday(&begin,0);
  for(int i = 0;i < numthreads;i++)
       pthread_join(*(threads+i),NULL);
  gettimeofday(&end,0);

  uint64_t microseconds = ((end.tv_sec - begin.tv_sec) * 1000000L);
  microseconds += end.tv_usec;
  microseconds -= begin.tv_usec;
  float seconds = microseconds/1000000.0;

  cerr.precision(2); 
  cerr.setf(std::ios::fixed,std::ios::floatfield); 
  cerr << "Total runtime(s): " << seconds << endl;
  cerr << "Successful file opens: " << numfiles_opened_success << 
      " Per second: " << (int)(numfiles_opened_success/seconds) << endl;
  cerr << "Failed file opens: " << numfiles_opened_fail << endl;
  cerr << "Bytes scanned: " << bytes_scanned << endl;
  cerr << "Bytes scanned/s: " << (int)(bytes_scanned/seconds) << endl;
  cerr << "Average file size: " << (bytes_scanned/numfiles_opened_success) << endl;
  cerr << "File histogram follows:" << endl;
  cerr << "     <=" << (1 << 10) << " bytes:  " << *file_sz_histogram << endl;
  cerr << "     <=" << (1 << 12) << " bytes:  " << *(file_sz_histogram+1) << endl;
  cerr << "    <=" << (1 << 14) << " bytes:  " << *(file_sz_histogram+2) << endl;
  cerr << "    <=" << (1 << 16) << " bytes:  " << *(file_sz_histogram+3) << endl;
  cerr << "   <=" << (1 << 18) << " bytes:  " << *(file_sz_histogram+4) << endl;
  cerr << "    >" << (1 << 18) << " bytes:  " << *(file_sz_histogram+5) << endl;

  void (*finish)(void) =  (void (*)(void))dlsym(module,"finish");
  if(finish != 0)finish();
  dlclose(module);
  return EXIT_SUCCESS;
}
