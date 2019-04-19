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
volatile int needed_pkt = 0; /* int to ensure that we don't allow for out of order packets*/
volatile int stop = 0;

struct sockaddr_in serveraddr; /* server's addr */
struct sockaddr_in clientaddr; /* client addr */
struct timeval tp;
struct itimerval timer;

FILE *fp; /* pointer for output file */

sigset_t sigmask;  

tcp_packet *recvpkt;
tcp_packet *sndpkt;



/*
 * handler to know when the timer counts down 
 */
void ack_sender(int sig)
{
    if (sig == SIGALRM)
    {
        stop = 1;

        VLOG(DEBUG, "HANDLER TRIGGERED");   
       
        /* 
         * if received exact packet needed, write to file 
         * ignore out of order packets lower than needed  
         */
        if( recvpkt->hdr.seqno == needed_pkt )
        {
            gettimeofday(&tp, NULL);
            VLOG(DEBUG, "TYPE 2 %lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
            fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
            fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);

            /* send ack for the packet */
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = recvpkt->hdr.seqno + recvpkt->hdr.data_size;
            sndpkt->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            printf("sending ack (3) number %d\n", needed_pkt );

        /* 
         * if higher than needed,
         * out of order packet, send a duplicate ack 
         */
        }else if ( recvpkt->hdr.seqno > needed_pkt ) { 
            
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = needed_pkt;
            sndpkt ->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            printf("sending duplicate ack (3) number %d\n", needed_pkt );

        } 

        


    }
    
}



void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}


void stop_timer()
{

    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}


/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for resending unacknowledged packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, ack_sender);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}





int main(int argc, char **argv) {
    VLOG(DEBUG, "VALUE OF DATA_SIZE! %lu",  DATA_SIZE);
  

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

    

    init_timer(500, ack_sender);

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
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            VLOG(INFO, "End Of File has been reached");
            fclose(fp);
            free(sndpkt);
            break;
        }

        /* 
         * sendto: ack back to the client 
         */

        /* 
         * if we get the next packet we are expecting in sequence,
         * then write the packet to the output file. Wait to see if there is any new packet coming for 
         * 500 ms before sending ack. If there was already one waiting, send ack for both, cumulatively
         * otherwise, ack for the first will be triggered automatically
         * ignore out of order packets lower than needed
         */
        if( recvpkt->hdr.seqno == needed_pkt )
        {

            gettimeofday(&tp, NULL);
            VLOG(DEBUG, "TYPE 1 %lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);

            /* write the packet*/
            fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
            fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);
            
            /* we are now expecting the second packet*/
            
            needed_pkt = recvpkt->hdr.seqno + recvpkt->hdr.data_size; /* specify which number the next packet should have*/
            printf("after type 1 receipt needed_pkt has a value %d\n", needed_pkt );
            

            /* start the wait for second packet */
           
            start_timer(); 
            if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0 && errno == EINTR) 
            {
                error("ERROR in recvfrom");
            }
            recvpkt = (tcp_packet *) buffer;
            assert(get_data_size(recvpkt) <= DATA_SIZE);
            if ( recvpkt->hdr.data_size == 0) /* if it was an empty packet, close program*/
            {
                sndpkt = make_packet(0);
                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                        (struct sockaddr *) &clientaddr, clientlen) < 0) {
                    error("ERROR in sendto");
                }
                VLOG(INFO, "End Of File has been reached");
                fclose(fp);
                free(sndpkt);
                break;
            }
            /* stop the timer */
            stop_timer(); 

        
            /* 
             * if did receive a packet, then stop will still have  a value 0
             * otherwise it will have a value 1 and would have already sent ack for first packet 
             * 
             */
            if (stop==0){

                /* if it was the second packet was needed */
                if( recvpkt->hdr.seqno == needed_pkt ) 
                {
                    needed_pkt = recvpkt->hdr.seqno + recvpkt->hdr.data_size;
                    gettimeofday(&tp, NULL);
                    VLOG(DEBUG, "TYPE 2 %lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
                    fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
                    fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);
                    printf("after type 2 receipt needed_pkt has a value %d\n", needed_pkt );

                    /* send ack for the packet */
                    sndpkt = make_packet(0);
                    sndpkt->hdr.ackno = recvpkt->hdr.seqno + recvpkt->hdr.data_size;
                    sndpkt->hdr.ctr_flags = ACK;
                    if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                            (struct sockaddr *) &clientaddr, clientlen) < 0) {
                        error("ERROR in sendto");
                    } 
                    printf("sending ack (1) number %d\n", needed_pkt );

                /* if higher than expected out of order packet, send a duplicate ack */
                }else if ( recvpkt->hdr.seqno > needed_pkt ) { 
            
                    sndpkt = make_packet(0);
                    sndpkt->hdr.ackno = needed_pkt;
                    sndpkt ->hdr.ctr_flags = ACK;
                    if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                            (struct sockaddr *) &clientaddr, clientlen) < 0) {
                        error("ERROR in sendto");
                    }
                    printf("sending duplicate ack (1) number %d\n", needed_pkt );

                } 
                

            }



        /*
         * send a duplicate ack when we are getting an out of order packet
         * higher than what is needed
         */
        
        } else if ( recvpkt->hdr.seqno > needed_pkt ) {
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = needed_pkt;
            sndpkt ->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            printf("sending duplicate ack (2) number %d\n", needed_pkt );

        }

       


    }


    return 0;
}
