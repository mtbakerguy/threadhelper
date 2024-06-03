#include "globfile.h"

using namespace std;

extern const char match_[] = "oom-killer";
extern const char glob_[]  = "/var/log/kern.log";

extern "C" {

void* worker(void* arg) {
  return worker_<match_,glob_,RegexMatch>(CoutMatchHandle((struct module*)arg));
}
};
