#include "sys/socket.h"
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h> 
#include <stdio.h>
#include "errno.h"
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

#define DEBUG 0
#define MAX_SOCKETS 0xFF
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

void kill_ev(int sig) {
  printd("processes is killed %i \n", getpid());
  kill(getpid(), SIGKILL);
}

int set_nonblock(int socket) {
  int flags, s;

  flags = fcntl(socket, F_GETFL, 0);
  if(flags == -1) {
    return -1;
  }

  flags |= O_NONBLOCK;
  s = fcntl(socket, F_SETFL, flags);
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

// Clear read-write buffer and close socket fd
void close_descriptor(int sid, int sock_id, int socket_list[], struct buffer * b[]) {
  printd("Close file descriptor #%i \n", 1);
  free(b[sid]->rd);
  //free(b[sid]->wr);
  
  close(sock_id);
  socket_list[sid] = 0;
  fflush(stdout);
}

int main(int argc, char ** argv) {
  struct sockaddr_in saddr;
  int active_sockets[MAX_SOCKETS];
  struct buffer * state_struct[MAX_SOCKETS];
  int len_recv = 0, len_r = 0, n_connection = 0;
  size_t count_sockets = 0;
  size_t free_socket_index = 0;
  
  memset(active_sockets, 0, MAX_SOCKETS);

  size_t s = socket(AF_INET, SOCK_STREAM, 0);
  saddr.sin_family = AF_INET;
  saddr.sin_port = htons(atoi(argv[1]));
  saddr.sin_addr.s_addr = INADDR_ANY;

  if(bind(s, (const struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
    perror("Error of bind socket");
    exit(1);
  }

  socklen_t len = sizeof(saddr);
  if(listen(s, MAX_SOCKETS) == -1) {
    perror("Error of listening socket");
    exit(1);
  }

  printd("Succeed bind and listen socket server on port %i \n", atoi(argv[1]));

  // Set to nonblocking mode acceptible socket
  set_nonblock(s);

  if(signal(SIGTERM, &kill_ev) == SIG_ERR || signal(SIGINT, &kill_ev) == SIG_ERR) {
    printd("Error of signal handler", 0);
    exit(EXIT_FAILURE);
  }

  while(1) {    
    n_connection = accept(s, (struct sockaddr*)&saddr, &len);
    if(n_connection >= 0) {    
      set_nonblock(n_connection);
      
      // Find free slot
      while(active_sockets[free_socket_index] != 0 && free_socket_index < MAX_SOCKETS) {
        free_socket_index ++;
      }

      if(active_sockets[free_socket_index] != 0) {
        // Not free slots
        close(n_connection);
        continue;
      }

      // Set new connection to empty slot
      active_sockets[free_socket_index] = n_connection;
      
      // Initialize state struct for peer
      struct buffer data_connection;
      data_connection.rdc = 0;
      data_connection.rwc = 0;
      data_connection.rd = malloc(BUFFER_SIZE);
      data_connection.upset_time = clock() / CLOCKS_PER_SEC;
      state_struct[free_socket_index] = &data_connection;

      free_socket_index = 0;
      count_sockets ++;
    }

    for(int i = 0; i < count_sockets; i ++) {
      int td = active_sockets[i];

      if(td == 0) 
        continue;

      char recv_buffer[RW_CHUNK_SIZE];

      time_t ctime = clock() / CLOCKS_PER_SEC;
      if(ctime - state_struct[i]->upset_time > TIME_DIFF) {
        close_descriptor(i, td, active_sockets, state_struct);
        continue;
      }

      int r_len = state_struct[i]->rdc + RW_CHUNK_SIZE;
      // Floating chunks to prevent overflow
      if(r_len > BUFFER_SIZE) {
        r_len = (BUFFER_SIZE - state_struct[i]->rdc);
      }

      len_recv = read(td, &recv_buffer, r_len);

      if(!len_recv && r_len) {
        close_descriptor(i, td, active_sockets, state_struct);
      } else if(len_recv == -1) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        } else {
          close_descriptor(i, td, active_sockets, state_struct);
        }

      } else if(len_recv) {
        state_struct[i]->upset_time = ctime;
        // Copy data to peer buffer with offset
        strncpy((state_struct[i]->rd + state_struct[i]->rdc), recv_buffer, len_recv);
        state_struct[i]->rdc += len_recv;
        printd(state_struct[i]->rd, 0);
        printd("\n", 0);
      }

      if(state_struct[i]->rdc - state_struct[i]->rwc > 0) {
        if(state_struct[i]->rwc < state_struct[i]->rdc) {
          // Set offset for reading buffer data
          int w_len = state_struct[i]->rdc - state_struct[i]->rwc;
          // Align write chunk
          if(w_len > RW_CHUNK_SIZE) {
            w_len = RW_CHUNK_SIZE;
          }

          len_r = write(td, (state_struct[i]->rd + state_struct[i]->rwc), w_len);
          
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

  exit(EXIT_SUCCESS);
}