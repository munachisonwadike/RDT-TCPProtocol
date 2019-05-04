#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
 
#include"common.h"
#include"packet.h"

#define STDIN_FD    0
#define RESEND  1000 /* millisecond */ 



char buffer[DATA_SIZE];
char *hostname;
    
int pkt_base;
int portno; 
int len;
int next_seqno;
int sockfd;
int serverlen;
int shift;
int window_index;


int final_packet_reached = 0;
int last_packet = -1;
int last_ack = 0;
int next_seqno=0;
int send_base=0;
int WINDOW_SIZE = 10;

struct sockaddr_in serveraddr;
struct itimerval timer; 


int i; int j;
volatile int k = -1;

FILE *fp;

sigset_t sigmask;  

tcp_packet *sndpkt;
tcp_packet *recvpkt;
tcp_packet* window[10]; /* array to store packet window */  

     

/*
 * if the resend function is triggered, resend the entire current window
 */
void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        VLOG(DEBUG, "RESEND FUNCTION TRIGGERED");   
        window_index = 0;    
        for (window_index = 0; window_index < WINDOW_SIZE; ++window_index)
        {
            /* 
             * resend all packets range between sendBase and nextSeqNum 
             */ 
            if(sendto(sockfd, window[window_index], TCP_HDR_SIZE + get_data_size(window[window_index]), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }
            printf("packet with seqno %d resent \n", window[window_index]->hdr.seqno);

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
 * function to initialize timer, delay in milliseconds,
 * signal handler function for resending unacknowledged packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, resend_packets);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}


int main (int argc, char **argv)
{
 

    /* 
     * check command line arguments 
     */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    /* 
     * socket: create the socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");


    /*
     * initialize server server details 
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* 
     * covert host into network byte order 
     */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* 
     * build the server's Internet address 
     */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    /* 
     * stop and wait protocol 
     */
    init_timer(RESEND, resend_packets);

    /*
     * set the initial value of window base loop 10 times to make 10 packets
     * store pointer to packet i in window[i]
     */
    next_seqno = 0;
	window_index = 0;
	while ( window_index < WINDOW_SIZE )
	{
		len = fread(buffer, 1, DATA_SIZE, fp);
		if (len<=0){
            window[window_index] = make_packet(0);
            if(len==0){
                VLOG(INFO, "Empty file");
                sndpkt = make_packet(0);
                sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                        (const struct sockaddr *)&serveraddr, serverlen);
                free(sndpkt); 
            }
		}
		else{ 
            pkt_base = next_seqno;
        	next_seqno = pkt_base + len;
    		window[window_index] = make_packet(len);
     		memcpy(window[window_index]->data, buffer, len);
            window[window_index]->hdr.seqno = pkt_base;
            
        }
        window_index++;
	}

    /*
     * send the first ten packets  in the window
     */
    for ( window_index = 0; window_index < WINDOW_SIZE; window_index++)
    {

        if(sendto(sockfd, window[window_index], TCP_HDR_SIZE + get_data_size(window[window_index]), 0, 
                ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto error");
        }
        printf("packet %d sent \n", window[window_index]->hdr.seqno);

    }

    /*
     * start the timer to wait for the ack 
     * for the first packet in the window
     */
    start_timer();  

    /*
     * constantly send the packets, wait for acks, 
     * and slide the window up for the next iteration of this loop
     */
    int x = 0;
    do 
    {

        /* 
         * receive packets and see if they are the acks you expected 
         */
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                    (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
        {
            error("recvfrom error");
        }
        
        recvpkt = (tcp_packet *)buffer;

        printf(" ack receieved - %d | last_ack - %d \n", recvpkt->hdr.ackno, last_ack );
        assert(get_data_size(recvpkt) <= DATA_SIZE);


        /*
         * if we get a packet from the received saying is it closing, then the sender should close
         * and exit
         */
        if( recvpkt->hdr.ackno == last_packet ){
            printf("last_packet received %d, recvpkt->hdr.ackno %d\n", last_packet, recvpkt->hdr.ackno );
            exit(1);
        }


        /*
         * since the receiver only sends acks when a packet has been written,
         * we can accept acks > than last ack
         */
        if(recvpkt->hdr.ackno >= last_ack)
        {   
            /* 
             * if you get an ack, stop time to process it 
             */ 
            stop_timer();

            /*
             * check how much far ahead in window the recvd packet is than 
             * the base for needed acks and set new needed ack to the the received packet number 
             */           
            shift = ( recvpkt->hdr.ackno - last_ack )/ DATA_SIZE;
            last_ack = recvpkt->hdr.ackno;

            /* 
             * if you received an ack, calculate the new window and populate the empty part of it
             */


            /* option 1, step 1 if we haven't reach last packet, calculate the new window by simultaneously deleting and freeing 
             * all packets in closed interval [0, shift], and secondly by copying all packets in 
             * the closed interval [shift + 1, windowsize-1] to new respective positions shift steps 
             * behind them in the window. 
             */
            if (final_packet_reached == 0){
                window_index = 0; 
                while (window_index < WINDOW_SIZE)
                {
     
                    if ( window_index >= shift ){
                        window[window_index-shift] = window[window_index];

                        VLOG(DEBUG, "(1) generating window (size %d)with index %d set to %d shift %d  ", 
                            WINDOW_SIZE, window_index-shift, window[window_index-shift]->hdr.seqno, shift )
                    }else{
                        free(window[window_index]);
                    }
                    window_index++;
                }
                /* option 1, step 2 - then populate the interval [windowize - shift, windowsize -1] with
                 * the new packets. concerning the left endpoint, windowsize - 1 gives index of the last element in window when full
                 * and substracting shift gives the first index to start repopulating from
                 */
            
                for ( window_index =  WINDOW_SIZE - 1 - shift; window_index < WINDOW_SIZE ; window_index ++ )
                {
                    len = fread(buffer, 1, DATA_SIZE, fp);
                    if ( len <=0 ){
                        VLOG(INFO, " End Of File ");
                        WINDOW_SIZE = window_index + 1;
                        final_packet_reached = 1;
                        break;
                    }else{
                        pkt_base = next_seqno;
                        next_seqno = pkt_base + len; 
                        window[window_index] = make_packet(len);
                        memcpy(window[window_index]->data, buffer, len);
                        window[window_index]->hdr.seqno = pkt_base;
                        VLOG(DEBUG, "(2) generating window (size %d) with index %d, set to %d shift %d  ", 
                            WINDOW_SIZE, window_index-shift, window[window_index-shift]->hdr.seqno, shift )

                        // last_packet = next_seqno;
                    }
                }


            }else{
            
            /* option 2, step 1 if we reach last packet, calculate the new window by simultaneously deleting and freeing 
             * all packets in closed interval [0, shift], and secondly by copying all packets in 
             * the closed interval [shift + 1, windowsize-1] to new respective positions shift steps 
             * behind them in the window. 
             */
                window_index = 0; 
                while (window_index < WINDOW_SIZE)
                {
     
                    if ( window_index >= shift ){
                        window[window_index-shift] = window[window_index];

                        VLOG(DEBUG, "window is now size %d with index %d, window[window_index]->hdr.seqno %d shift %d  ", 
                            WINDOW_SIZE, window_index, window[window_index]->hdr.seqno, shift )

                    }else{
                        free(window[window_index]);
                    }
                    window_index++;
                }
                /*
                 * option 2, step 2 - reset the window size, such that only the 
                 * half-open invterval [0, windowsize-shift ) is considered in future send-offs
                 */            
                WINDOW_SIZE = WINDOW_SIZE - shift;  
                
                /*
                 *send 0 ack if nothing left to send in window and in closing
                 */
                if (WINDOW_SIZE == 0){
                
                    VLOG(INFO, "Empty file");
                    sndpkt = make_packet(0);
                    sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                            (const struct sockaddr *)&serveraddr, serverlen);
                    free(sndpkt); 
                    break;
                }


            }

            /*
             * loop through the packet window and send all the packets
             */
            VLOG(DEBUG, "sending window of size %d from base %d -> %s", WINDOW_SIZE,  
                window[0]->hdr.seqno, inet_ntoa(serveraddr.sin_addr));       

            for (window_index = 0; window_index < WINDOW_SIZE; window_index++)
            {
                if(sendto(sockfd, window[window_index], TCP_HDR_SIZE + get_data_size(window[window_index]), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
                {
                    error("sendto error");
                }
                printf("packet %d sent \n", window[window_index]->hdr.seqno);

     
            }

              
            start_timer(); 
        }

        x ++;
        if (x==5)
            break  ;


    } while( 1 );

    
    




    return 0;

}


