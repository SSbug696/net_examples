#include <stdio.h> //for printf
#include <string.h> //memset
#include <sys/socket.h>    //for socket ofcourse
#include <sys/types.h>
#include <stdlib.h> //for exit(0);
#include <errno.h> //For errno - the error number
// #include <netinet/udp.h>   //Provides declarations for udp header
#include <netinet/ip.h>    //Provides declarations for ip header
#include <netinet/in_systm.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>
#include <net/if.h>
#include <iostream>
/* 
    96 bit (12 bytes) pseudo header needed for udp header checksum calculation 
*/

using namespace std;


struct pseudo_header
{
    u_int32_t source_address;
    u_int32_t dest_address;
    u_int8_t placeholder;
    u_int8_t protocol;
    u_int16_t udp_length;
};

#ifdef __FAVOR_BSD
struct udphdr
{
  u_int16_t uh_sport;       /* source port */
  u_int16_t uh_dport;       /* destination port */
  u_int16_t uh_ulen;        /* udp length */
  u_int16_t uh_sum;     /* udp checksum */
};

#else

struct udphdr
{
  u_int16_t source;
  u_int16_t dest;
  u_int16_t len;
  u_int16_t check;
};
#endif

/*
    Generic checksum calculation function
*/
unsigned short csum(unsigned short * ptr, int nbytes) 
{
    register long sum;
    unsigned short oddbyte;
    register short answer;
 
    sum=0;
    while(nbytes > 1) {
        sum += *ptr ++;
        nbytes -= 2;
    }
    if (nbytes == 1) {
        oddbyte = 0;
        *((u_char*)&oddbyte)=*(u_char*)ptr;
        sum += oddbyte;
    }
 
    sum = (sum >> 16) + (sum & 0xffff);
    sum = sum + (sum >> 16);
    answer = (short)~sum;
     
    return(answer);
}
 
int main(int argc, char* argv[])
{
    char buffer[10];
    int sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_TCP);
    if (sockfd == -1) {
        printf("Socket failed!!\n");
        return -1;
    }

    // for(int i=0; i < 3; i++) {
    //     int recv_length = recv(sockfd, buffer, 8000, 0);
    //     printf("Got some bytes : %d\n", recv_length);
    // }

    //Create a raw socket of type IPPROTO
    //IPPROTO_RAW
    int s = socket(PF_INET, SOCK_RAW, IPPROTO_RAW);
    struct pseudo_header psh;
    struct sockaddr_in sin;

    if(s == -1)
    {
        perror("Failed to create raw socket");
        exit(1);
    }
     
    // Datagram to represent the packet
    char datagram[4096] , source_ip[32] , *data , *pseudogram;

    data = datagram + sizeof(struct ip) + sizeof(struct udphdr);
    strcpy(data , "ABCDEFGHIJKLMNOPQRSTUVWXYZ");

    struct ip * iph = (struct ip *) &datagram;
    struct udphdr * udph = (struct udphdr *) &datagram + 1;
     
    memset(datagram, 0x0, sizeof(datagram));
    memset(source_ip, 0x0, sizeof(source_ip));
    //95.165.71.232
    strcpy(source_ip , "127.0.0.1");
     
    sin.sin_family = AF_INET;
    sin.sin_port = htons(60598);
    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
     
    iph->ip_hl = 5;
    iph->ip_v = 4;//IPVERSION;
    iph->ip_tos = IPTOS_PREC_ROUTINE;
    iph->ip_len = sizeof (struct ip) + sizeof (struct udphdr) + strlen(data);
    iph->ip_id = (u_short)htonl(54321);
    iph->ip_off = 0x0;
    iph->ip_ttl = MAXTTL;
    iph->ip_p = IPPROTO_UDP;
    iph->ip_src.s_addr = inet_addr(source_ip);
    iph->ip_dst.s_addr = sin.sin_addr.s_addr;
    iph->ip_sum = csum((unsigned short *) iph, iph->ip_len);

    // UDP header
    udph->source = htons(8001);
    udph->dest = htons(60584);
    udph->len = htons(8 + strlen(data));
    udph->check = 0;
     
    // Now the UDP checksum using the pseudo header
    psh.source_address = inet_addr(source_ip);
    psh.dest_address = sin.sin_addr.s_addr;
    psh.placeholder = 0;
    psh.protocol = IPPROTO_UDP;
    psh.udp_length = htons(sizeof(struct udphdr) + strlen(data) );
     
    int psize = sizeof(struct pseudo_header) + sizeof(struct udphdr) + strlen(data);
    pseudogram = (char*)malloc(psize);
     
    memcpy(pseudogram , (char*) &psh , sizeof (struct pseudo_header));
    memcpy(pseudogram + sizeof(struct pseudo_header) , udph , sizeof(struct udphdr) + strlen(data));
     
    udph->check = csum( (unsigned short*) pseudogram , psize);

    while (1){
      //Send the packet
      if (sendto (s, datagram, iph->ip_len ,  0, (struct sockaddr *) &sin, sizeof (sin)) < 0) {
        perror("sendto failed");
      } else {
        printf ("Packet Send. Length : %d \n" , iph->ip_len);
      }
    }
     
    return 0;
}