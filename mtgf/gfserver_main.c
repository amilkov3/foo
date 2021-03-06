
#include "gfserver-student.h"

#define USAGE                                                                 \
"usage:\n"                                                                    \
"  gfserver_main [options]\n"                                                 \
"options:\n"                                                                  \
"  -t [nthreads]       Number of threads (Default: 5)\n"                      \
"  -p [listen_port]    Listen port (Default: 19121)\n"                         \
"  -m [content_file]   Content file mapping keys to content files\n"          \
"  -h                  Show this help message.\n"                             \

/* OPTIONS DESCRIPTOR ====================================================== */
static struct option gLongOptions[] = {
  {"port",          required_argument,      NULL,           'p'},
  {"nthreads",      required_argument,      NULL,           't'},
  {"content",       required_argument,      NULL,           'm'},
  {"help",          no_argument,            NULL,           'h'},
  {NULL,            0,                      NULL,             0}
};


extern gfh_error_t gfs_handler(gfcontext_t **ctx, const char *path, void* arg);

static void _sig_handler(int signo){
  if ((SIGINT == signo) || (SIGTERM == signo)) {
    exit(signo);
  }
}

static void Pthread_mutex_lock(pthread_mutex_t *mutex) {
  int rc = pthread_mutex_lock(mutex);
  assert(rc == 0);
}

static void Pthread_mutex_unlock(pthread_mutex_t *mutex) {
  int rc = pthread_mutex_unlock(mutex);
  assert(rc == 0);
}

static void Pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex){
  int rc = pthread_cond_wait(cond, mutex);
  assert(rc == 0);
}

static void* work(void *arg) {
  queue_item *item;
  int fd, fstatus; //read_size;
  struct stat fs;
  int BUFSIZE = 1024;
  char resp_buffer[BUFSIZE];
  work_arg *warg; 
  gfcontext_t *ctx;
  
  warg = (work_arg *) arg;
  
  while (1) {

    /* Wait until work exists + signal */
    Pthread_mutex_lock(warg->queue_lock);
    while (steque_isempty(warg->work_queue)) {
      Pthread_cond_wait(warg->cons_cond, warg->queue_lock);
    }
    item = (queue_item *) steque_pop(warg->work_queue);
    Pthread_mutex_unlock(warg->queue_lock);

    ctx = item->ctx;

    /* Initialize file descriptor */
    if ((fd = content_get(item->path)) < 0) {
      gfs_sendheader(&ctx, GF_FILE_NOT_FOUND, 0);
      continue;
    }

    /* Set file status */
    if ((fstatus = fstat(fd, &fs)) < 0) {
      gfs_sendheader(&ctx, GF_ERROR, 0);
      close(fd);
      continue;
    }

    pthread_t t = pthread_self();
    printf("Size of file: %ld with path %s from thread %lu\n", fs.st_size, item->path, t);

    /* Send header */
    gfs_sendheader(&ctx, GF_OK, fs.st_size);

    /* Ensure zerod-out */
    memset(resp_buffer, 0, BUFSIZE);

    int bytes_read = 0;
	  int bytes_sent = 0;
    int bytes_sent_curr;
    while ((bytes_read = pread(fd, resp_buffer, BUFSIZE, bytes_sent)) > 0) {
      /* Send bytes and add to total */
      bytes_sent_curr = gfs_send(&ctx, resp_buffer, bytes_read);
      bytes_sent += bytes_sent_curr;

      printf("Read %d bytes and wrote %d bytes to client. Total sent: %d with path %s of size %ld from thread %lu\n", 
        bytes_read, bytes_sent_curr, bytes_sent, item->path, fs.st_size, t);

      /* Zero out */
      memset(resp_buffer, 0, BUFSIZE);
    }

    printf("Ended up sending %d bytes to client with path %s of size %ld from thread %lu\n", 
       bytes_sent, item->path, fs.st_size, t);

    free(item);
  }


  return 0;
}

/* Main ========================================================= */
int main(int argc, char **argv) {
  int option_char = 0;
  unsigned short port = 19121;
  char *content_map = "content.txt";
  gfserver_t *gfs = NULL;
  int nthreads = 5;

  setbuf(stdout, NULL);

  if (SIG_ERR == signal(SIGINT, _sig_handler)){
    fprintf(stderr,"Can't catch SIGINT...exiting.\n");
    exit(EXIT_FAILURE);
  }

  if (SIG_ERR == signal(SIGTERM, _sig_handler)){
    fprintf(stderr,"Can't catch SIGTERM...exiting.\n");
    exit(EXIT_FAILURE);
  }

  // Parse and set command line arguments
  while ((option_char = getopt_long(argc, argv, "t:rhm:p:", gLongOptions, NULL)) != -1) {
    switch (option_char) {
      case 't': // nthreads
        nthreads = atoi(optarg);
        break;
      case 'p': // listen-port
        port = atoi(optarg);
        break;
      case 'h': // help
        fprintf(stdout, "%s", USAGE);
        exit(0);
        break;       
      default:
        fprintf(stderr, "%s", USAGE);
        exit(1);
      case 'm': // file-path
        content_map = optarg;
        break;                                          
    }
  }

  /* not useful, but it ensures the initial code builds without warnings */
  if (nthreads < 1) {
    nthreads = 1;
  }

  content_init(content_map);

  steque_t* work_queue = (steque_t *) malloc(sizeof(steque_t));
  steque_init(work_queue);

  pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;

  pthread_cond_t cons_cond = PTHREAD_COND_INITIALIZER;
  pthread_cond_t main_cond = PTHREAD_COND_INITIALIZER;

  work_arg warg = {
    .work_queue = work_queue,
    .queue_lock = &queue_lock,
    .cons_cond = &cons_cond,
    .main_cond = &main_cond,
    .port = port,
  };

  /* Initialize thread management */
  pthread_t pool[nthreads];
  for (int i = 0; i < nthreads; i++) {
    if (pthread_create(&pool[i], NULL, work, &warg) != 0){
      fprintf(stderr, "Error creating thread # %d", i);
      exit(1);
    }
  }

  /*Initializing server*/
  gfs = gfserver_create();

  /*Setting options*/
  gfserver_set_port(&gfs, port);
  gfserver_set_maxpending(&gfs, 16);
  gfserver_set_handler(&gfs, gfs_handler);
  gfserver_set_handlerarg(&gfs, (void *) &warg); // doesn't have to be NULL!

  /*Loops forever*/
  gfserver_serve(&gfs);

  /* Clean up */
  free(pool);
  steque_destroy(work_queue);
  free(gfs);
  content_destroy();

}
