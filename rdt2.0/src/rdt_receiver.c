/*
 * Nabil Rahiman
 * NYU Abudhabi
 * email: nr83@nyu.edu
 */

#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>

#include "common.h"
#include "packet.h"


/*
 * You are required to change the implementation to support
 * window size greater than one.
 * In the current implementation window size is one, hence we have
 * only one send and receive packet
 */
tcp_packet *recvpkt;
tcp_packet *recvpkt2;
tcp_packet *sndpkt;

sigset_t sigmask;  
struct itimerval timer; 

volatile int stop = 0;

/*
 * handler to know when the timer counts down 
 */
void handler(int sig)
{
    if (sig == SIGALRM)
    {

        VLOG(DEBUG, "RESEND FUNCTION TRIGGERED");   
        stop = 1;
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
    signal(SIGALRM, handler);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}





int main(int argc, char **argv) {
    VLOG(DEBUG, "VALUE OF DATA_SIZE! %lu",  DATA_SIZE);
    int sockfd; /* socket */
    int portno; /* port to listen on */
    int needed_pkt = 0; /* int to ensure that we don't allow for out of order packets*/
    int needed_pkt2 = 0; /* int to ensure that we don't allow for out of order packets*/
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    FILE *fp;
    char buffer[MSS_SIZE];
    struct timeval tp;


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

    

    init_timer(500, handler);

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
         * case 1: if we get the next packet we are expecting in sequence,
         * then send write the packet to the output file. Wait to see if there is any new packet coming for 
         * 500 ms before sending ack. If there was already one waiting, send ack for both, cumulatively
         */
        if( recvpkt->hdr.seqno == needed_pkt )
        {

            gettimeofday(&tp, NULL);
            VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);

            fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
            fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);


        /**/
            /*
             * Wait for another for 500 ms for another to possible packet
             */ 
            needed_pkt2 = recvpkt->hdr.seqno + recvpkt->hdr.data_size; /* specify which number the next packet should have*/
                
            /* start the wait */
            start_timer(); 
            // if (stop)
            // {
            //     printf("cleanup \n");
            // }
            if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0 && errno == EINTR) 
            {
                error("ERROR in recvfrom");
            }
            recvpkt2 = (tcp_packet *) buffer;
            assert(get_data_size(recvpkt2) <= DATA_SIZE);
            if ( recvpkt2->hdr.data_size == 0) /* if it was an empty packet, close program*/
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
            /* end the wait*/
            stop_timer(); 

            /* if what you recieved was good, add it to the written file */
            if( recvpkt2->hdr.seqno == needed_pkt2 )
            {
                gettimeofday(&tp, NULL);
                VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt2->hdr.data_size, recvpkt2->hdr.seqno);
                fseek(fp, recvpkt2->hdr.seqno, SEEK_SET);
                fwrite(recvpkt2->data, 1, recvpkt2->hdr.data_size, fp);
                recvpkt2 = recvpkt; /* if the packet was good, make sure that you are sending a cumulative ack */
            }       
        /**/

            /* send cumulative ack of the two packets or the one packet if no second one came */
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = recvpkt->hdr.seqno + recvpkt->hdr.data_size;
            needed_pkt = sndpkt->hdr.ackno;
            sndpkt->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }            

        /*
         * case 2: ignore duplicates received from rdt sender
         */
        }else if ( recvpkt->hdr.seqno < needed_pkt ){

            continue;

        /*
         * case 3: send a duplicate ack when we are getting an out of order packet
         * higher than what is needed
         */
        }else{
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = needed_pkt;
            sndpkt->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
        }
    }


    return 0;
}
