#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "packet.h"

char buffer[MSS_SIZE]; /* buffer to store any received packets */

int clientlen; /* byte size of client's address */
int sockfd; /* socket */
int optval; /* flag value for setsockopt */
int portno; /* port to listen on */

int FINAL_SEND = 50; /* number of times to send off the ack for last packet */
int WINDOW_SIZE = 10; /* receiver window */

volatile int needed_pkt = 0; /* int to ensure that we don't allow for out of order packets*/
volatile int stop = 0;

struct sockaddr_in serveraddr; /* server's addr */
struct sockaddr_in clientaddr; /* client addr */
struct timeval tp; 

FILE *fp; /* pointer for output file */

sigset_t sigmask;  

tcp_packet *recvpkt;
tcp_packet *sndpkt;
tcp_packet** rcv_window[10]; /* buffer for out of sequence packets */  





int main(int argc, char **argv) {
    VLOG(DEBUG, "VALUE OF DATA_SIZE! %lu",  DATA_SIZE);
    /*
     * set up the receiver buffer
     */
    // int windex = 0;
    // if (rcv_window)
    // {
    //   for (windex = 0; windex < 10; i++)
    //   {
    //     a[i] = malloc(sizeof *a[i] * M);
    //   }
    // }

    /* 
     * check command line arguments 
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

    /* 
     * socket: create the parent socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /* 
     * bind: associate the parent socket with a port 
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");


    /* 
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    clientlen = sizeof(clientaddr);
    while (1) {
        /*
         * recvfrom: receive a udp datagram from a client
         */
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }
        recvpkt = (tcp_packet *) buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);

        if ( recvpkt->hdr.data_size == 0) { /* if it was empty packet, close the program */
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = -1;
            sndpkt->hdr.ctr_flags = 0;/* type (0) ack - closure response */
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            VLOG(INFO, "End Of File has been reached. Sent closure ack (0)");
            fclose(fp);
            free(sndpkt);
            break;
        }

        printf("JUST RECEIVED PACKET %d with flags %d \n", recvpkt->hdr.seqno, recvpkt->hdr.ctr_flags);

        /* 
         * sendto: ack back to the client 
         */

        /* 
         * if the received packet was as expected, 
         * write to output file and send an ack. If 
         * the packet doesn't arrive, just wait for it
         */
        if( recvpkt->hdr.seqno == needed_pkt )
        {

            gettimeofday(&tp, NULL);
            VLOG(DEBUG, " %lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);

            /* write the packet*/
            fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
            fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);
            
            /*
             * send an ack for the packet you have recieved and written 
             */
            sndpkt = make_packet(0);
            needed_pkt = recvpkt->hdr.seqno + recvpkt->hdr.data_size; /* update the number of the expected packet */
            sndpkt ->hdr.ctr_flags = 1; /* type (1) ack  - send the next one naturally */
            sndpkt->hdr.ackno = needed_pkt;
            
            printf("after receipt needed_pkt has a value %d\n", needed_pkt );

	        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
	                (struct sockaddr *) &clientaddr, clientlen) < 0) {
	            error("ERROR in sendto");
	        }
	        printf("sending ack (1) number %d\n", needed_pkt );  

            
            /* 
             * if the packet you receieved was the last packet, 
             * exit the program
             */
             if ( recvpkt->hdr.ctr_flags == -2) {  
                sndpkt = make_packet(0);
                sndpkt->hdr.ackno = -1;
                sndpkt->hdr.ctr_flags = 3; /* type (3)/1 - last packet at  the very end */
                /*
                 * if you receive the last packet at the very end, send the ack for it a large, 
                 * fixed number of times since someone has to end the conversation and there
                 * is not gaurantee the message is recvd
                 */
                int fin = 0;            
                for (fin = 0; fin < FINAL_SEND ; fin++)
                {
                    if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                            (struct sockaddr *) &clientaddr, clientlen) < 0) {
                        error("ERROR in sendto");
                    }
                }   
                VLOG(INFO, "Just receieved last packet (1), exiting program. Sent closure ack (3)/1");
                fclose(fp);
                free(sndpkt);
                exit(0);
            }                       
        /*
         * if the packet is higher than what is needed
         * send a duplicate ack 
         */
        
        } else if ( recvpkt->hdr.seqno > needed_pkt ) {
            
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = needed_pkt;
            sndpkt ->hdr.ctr_flags = 2; /* type (2) ack - packet is higher than needed out of order */
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            printf("sending duplicate ack (2) number %d\n", needed_pkt );

/* 
         * if you received the last packet but at the very end, 
         * exit the program. identified by setting control flag to -2
         */
        }else if ( recvpkt->hdr.seqno < needed_pkt ) {
        
            if ( recvpkt->hdr.ctr_flags == -2) {

                sndpkt = make_packet(0);
                sndpkt->hdr.ackno = -1;
                sndpkt->hdr.ctr_flags = 3; /* type (3)/2 - last packet at  the very end */
                int fin = 0;            
                /*
                 * if you receive the last packet at the very end, send the ack for it a large, 
                 * fixed number of times since someone has to end the conversation and there
                 * is not gaurantee the message is recvd
                 */
                for (fin = 0; fin < FINAL_SEND ; fin++)
                {
                    if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                            (struct sockaddr *) &clientaddr, clientlen) < 0) {
                        error("ERROR in sendto");
                    }
                }                
                VLOG(INFO, "Just receieved last packet (2), exiting program. Send closure ack (3)/2");
                fclose(fp);
                free(sndpkt);
                exit(0);
            }

            /*
             * if it wasn't the final packet, then the number should be lower than needed
             * so send duplicate ack to specify the one you needed 
             */     

            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = needed_pkt;
            sndpkt->hdr.ctr_flags = 4; /* type (4) ack - lower than expected */
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            printf("sending duplicate ack (4) number %d\n", needed_pkt );

         
            
        }
       


    }


    return 0;
}
