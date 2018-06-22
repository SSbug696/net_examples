#include "sys/socket.h"
#include "sys/select.h"
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h> 
#include <stdio.h>
#include "errno.h"
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <poll.h>

#define DEBUG 0
#define BUFFER_SIZE 0x40
#define RW_CHUNK_SIZE 0x20
#define TIME_DIFF 0xf

// Managed struct
struct buffer {
  char * rd;
  char * wr;
  size_t rdc;
  size_t rwc;
  time_t upset_time;
};

void printd(char * buffer, int n) {
  if(DEBUG) {
    printf(buffer, n);
    fflush(stdout);
  }
}

int set_nonblock(int socket) {
  int flags, s;

  flags = fcntl (socket, F_GETFL, 0);
  if(flags == -1) {
    return -1;
  }

  flags |= O_NONBLOCK;
  s = fcntl (socket, F_SETFL, flags);
  if(s == -1) {
    return -1;
  }

  struct so_linger {
    int l_onoff;
    int l_linger;
  } linger;

  linger.l_onoff = 0;
  linger.l_linger = 0;  

  setsockopt(socket, SOL_SOCKET, SO_LINGER | SO_REUSEADDR , &linger, sizeof(linger));
  return 0;
}

void close_descriptor(int sid, struct buffer * b[], fd_set * w, fd_set * r) {
  printd("Close file descriptor #%i \n", 1);
  free(b[sid]->rd);
  b[sid]->upset_time = 0;
  // We don't user write buffer
  //free(b[sid]->wr);
  FD_CLR(sid, r);
  FD_CLR(sid, w);

  close(sid);
  fflush(stdout);
}

int main(int argc, char ** argv) {
  struct timeval tv;
  struct sockaddr_in saddr;
  struct buffer * state_struct[FD_SETSIZE];
  int len_recv = 0, r_len = 0, n_connection = 0;
  // Socket ev timer
  tv.tv_sec = TIME_DIFF;
  tv.tv_usec = 0;

  /* 
    Sets for reading and writing, set_r and 
    set_w is temporary sets in each iteration
  */
  fd_set set_r, set_w, m_set_r, m_set_w;
  size_t max_index = 0;

  size_t s = socket(AF_INET, SOCK_STREAM, 0);
  saddr.sin_family = AF_INET;
  saddr.sin_port = htons(atoi(argv[1]));
  saddr.sin_addr.s_addr = INADDR_ANY;

  if(bind(s, (const struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
    printd("Error of bind socket", 0);
    exit(EXIT_FAILURE);
  }

  if(listen(s, FD_SETSIZE) == -1) {
    printd("Error of listening socket", 0);
    exit(EXIT_FAILURE);
  }

  // Do master socket is nonblocking
  set_nonblock(s);

  printd("Succeed bind and listen socket server on port %i \n", atoi(argv[1]));

  FD_ZERO(&m_set_r);
  FD_ZERO(&m_set_w);
  // For master socket use only reading set
  FD_SET(s, &m_set_r);
  
  // Set max socket index will be equal index of master socket
  // and will be grow when adding clients. Instead you can use FD_SETSIZE
  max_index = s;

  while(1) {
    memcpy(&set_r, &m_set_r, sizeof(m_set_r));
    memcpy(&set_w, &m_set_w, sizeof(m_set_w));

    int rc = select((max_index + 1), &set_r, &set_w, NULL, &tv);
    
    if(rc < 0) {
      printd("Error of select multiplexor", 1);
      exit(EXIT_FAILURE);
    }

    for(int i = 0; i <= max_index; i ++) {
      // Check time window with old awaiting socket
      if(state_struct[i]->upset_time > 0) {
        time_t ctime = clock() / CLOCKS_PER_SEC;
        if(ctime - state_struct[i]->upset_time > TIME_DIFF) {
          close_descriptor(i, state_struct, &m_set_w, &m_set_r);
          continue;
        }
      }

      if(FD_ISSET(i, &set_r)) {        
        if(i == s) {
          n_connection = accept(s, NULL, NULL);
          set_nonblock(n_connection);

          if(n_connection < 0) {
            printd("Error of accepting new connection", 0);
            exit(EXIT_FAILURE);
          }

          // Set number new file descriptor in read/write sets
          FD_SET(n_connection, &m_set_r);
          FD_SET(n_connection, &m_set_w);

          // Initialize state struct for peer
          struct buffer data_connection;
          data_connection.rdc = 0;
          data_connection.rwc = 0;
          data_connection.rd = malloc(BUFFER_SIZE);
          // Don't use write buffer
          //data_connection.wr = malloc(BUFFER_SIZE);

          // Set time window
          data_connection.upset_time = clock() / CLOCKS_PER_SEC;
          state_struct[n_connection] = &data_connection;

          if(max_index < n_connection)
            max_index = n_connection;
        } else {
          char recv_buffer[RW_CHUNK_SIZE];
          len_recv = 0;

          r_len = state_struct[i]->rdc + RW_CHUNK_SIZE;
          // Float chunk size
          if(r_len > BUFFER_SIZE) {
            r_len = (BUFFER_SIZE - state_struct[i]->rdc);
          }

          len_recv = read(i, &recv_buffer, r_len);

          if(!len_recv && r_len) {
            close_descriptor(i, state_struct, &m_set_w, &m_set_r);
          } else if(len_recv == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
              continue;
            } else {
              close_descriptor(i, state_struct, &m_set_w, &m_set_r);
            }

          } else if(len_recv) {
            state_struct[i]->upset_time = clock() / CLOCKS_PER_SEC;
            // Copy data to peer buffer with offset
            strncpy((state_struct[i]->rd + state_struct[i]->rdc), recv_buffer, len_recv);
            state_struct[i]->rdc += len_recv;
            printd(state_struct[i]->rd, 0);
            printd("\n", 0);
          }
        }
      }

      if(FD_ISSET(i, &set_w)) {
        if(state_struct[i]->rdc - state_struct[i]->rwc > 0) {
          if(state_struct[i]->rwc < state_struct[i]->rdc) {
            // Set offset from recv buffer
            int w_len = state_struct[i]->rdc - state_struct[i]->rwc;
            // Align write chunk
            if(w_len > RW_CHUNK_SIZE) {
              w_len = RW_CHUNK_SIZE;
            }

            size_t len_r = write(i, (state_struct[i]->rd + state_struct[i]->rwc), w_len);
            
            if(len_r) {
              state_struct[i]->rwc += w_len;
              printd("Wrote %i bytes \n", state_struct[i]->rwc);
            }

            if(state_struct[i]->rwc == state_struct[i]->rdc) {
              state_struct[i]->rdc = 0;
              state_struct[i]->rwc = 0;
              memset(state_struct[i]->rd, 0, BUFFER_SIZE - 1);
            }
          }
        }
      }
    }
  }

  exit(EXIT_SUCCESS);
}