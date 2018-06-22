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
#include <poll.h>

#define DEBUG 0
#define MAX_SOCKETS 0xFFFF
// r/w buffers
#define BUFFER_SIZE 0xff
#define RW_CHUNK_SIZE 0x40
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
void close_descriptor(int fd_index, struct pollfd descriptors[], struct buffer * b[]) {  
  close(descriptors[fd_index].fd);
  // This can be and free of write buffer, but we use only bufferization of read 
  free(b[fd_index]->rd);
  descriptors[fd_index].revents = 0;
  b[fd_index] = 0;
  descriptors[fd_index].fd = 0;  
}

int main(int argc, char ** argv) {
  struct sockaddr_in saddr;
  int len_recv = 0, r_len = 0;
  
  // Managed struct with buffers and transmit counters
  struct buffer * state_struct[MAX_SOCKETS];
  struct pollfd descriptors[MAX_SOCKETS];
  // The upper limit of active sockets
  size_t count_sockets = 0;
  // Current file descriptor accepting connection
  size_t FD = 0;
  time_t ctime;

  size_t s = socket(AF_INET, SOCK_STREAM, 0);
  saddr.sin_family = AF_INET;
  saddr.sin_port = htons(atoi(argv[1]));
  saddr.sin_addr.s_addr = INADDR_ANY;

  memset(descriptors, 0, sizeof(descriptors));
  memset(state_struct, 0, sizeof(state_struct));
  
  if(bind(s, (const struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
    perror("Error of bind socket");
    exit(EXIT_FAILURE);
  }

  socklen_t len = sizeof(saddr);
  if(listen(s, MAX_SOCKETS) == -1) {
    perror("Error of listening socket");
    exit(EXIT_FAILURE);
  }

  printd("Succeed bind and listen socket server on port %i \n", atoi(argv[1]));
  // Set to nonblocking mode acceptible socket
  set_nonblock(s);

  count_sockets = s;
  descriptors[0].fd = s;
  // Only listen, the possibility of writing does not interest
  descriptors[0].events = POLLIN;

  while(1) {    
    int ev = poll(descriptors, count_sockets, 0);

    if(ev < 0) {
      printd("Error of POLL multiplexor", 0);
      exit(EXIT_FAILURE);
    }

    size_t cc = count_sockets;

    for(int i = 0; i < cc; i ++) {
      
      if(descriptors[i].revents == 0)
        continue;

      // Check time window with old awaiting socket     
      ctime = clock() / CLOCKS_PER_SEC;
      if(descriptors[i].fd != s && descriptors[i].fd > 0) {
        if(ctime - state_struct[i]->upset_time > TIME_DIFF ) {
          close_descriptor(i, descriptors, state_struct);
          continue;
        }
      }
      
      // If this socket have new active event of record
      if(descriptors[i].revents & POLLIN) {
        if(descriptors[i].fd == s) {
          int nfd = accept(s, NULL, NULL);

          if(nfd < 0) {
            printd("Error of accepting new sockets", 0);
            exit(EXIT_FAILURE);
          } 

          int free_socket_index = 0;
          // Find free slot in struct array, solution for fragmentation
          while(descriptors[free_socket_index].fd != 0 && free_socket_index < MAX_SOCKETS) {
            free_socket_index ++;
          }

          if(descriptors[free_socket_index].fd != 0) {
            // Not free slots
            close(nfd);
            continue;
          }

          set_nonblock(nfd);

          // Initialize state struct for peer
          struct buffer data_connection;
          data_connection.rdc = 0;
          data_connection.rwc = 0;
          data_connection.rd = malloc(BUFFER_SIZE);
          data_connection.upset_time = clock() / CLOCKS_PER_SEC;

          state_struct[free_socket_index] = &data_connection;
          
          descriptors[free_socket_index].fd = nfd;
          descriptors[free_socket_index].events = POLLIN | POLLOUT;
          descriptors[free_socket_index].revents = 0;
          
          // Push upper limit sockets count
          if(free_socket_index > count_sockets) {
            count_sockets = free_socket_index;
          }
        } else {
          char recv_buffer[RW_CHUNK_SIZE];
          len_recv = 0;
          r_len = RW_CHUNK_SIZE;

          // Float chunk size
          if(state_struct[i]->rdc + r_len > BUFFER_SIZE) {
            r_len = (BUFFER_SIZE - state_struct[i]->rdc);
          }

          len_recv = read(descriptors[i].fd, &recv_buffer, r_len);

          if(len_recv == 0 && r_len > 0) {
            close_descriptor(i, descriptors, state_struct);
          } else if(len_recv == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
              continue;
            } else {
              close_descriptor(i, descriptors, state_struct);
            }
          } else if(len_recv) {
            state_struct[i]->upset_time = ctime;
            // Copy data to peer buffer with offset
            strncpy((state_struct[i]->rd + state_struct[i]->rdc), recv_buffer, len_recv);
            state_struct[i]->rdc += len_recv;
            printd("\n", 0);
          }
        }
      }

      if(descriptors[i].revents & POLLOUT && descriptors[i].fd != 0) {
        FD = i;
        if(state_struct[FD]->rdc - state_struct[FD]->rwc > 0) {
          if(state_struct[FD]->rwc < state_struct[FD]->rdc) {
            // Set offset from recv buffer
            int w_len = state_struct[FD]->rdc - state_struct[FD]->rwc;
            // Align write chunk
            if(w_len > RW_CHUNK_SIZE) {
              w_len = RW_CHUNK_SIZE;
            }

            size_t len_r = write(descriptors[FD].fd, (state_struct[FD]->rd + state_struct[FD]->rwc), w_len);
            
            if(len_r) {
              state_struct[FD]->rwc += w_len;
              printd("Wrote %i bytes \n", state_struct[FD]->rwc);
            }

            if(state_struct[FD]->rwc == state_struct[FD]->rdc) {
              state_struct[FD]->rdc = 0;
              state_struct[FD]->rwc = 0;
              memset(state_struct[FD]->rd, 0, BUFFER_SIZE - 1);
            }
          }
        }
      }
    }
  }

  exit(EXIT_SUCCESS);
}