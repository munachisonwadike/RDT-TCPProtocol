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
#define WINDOW_SIZE 10


char buffer[DATA_SIZE];
char *hostname;
    
int pkt_base;
int portno; 
int len;
int next_seqno;
int sockfd;
int serverlen;
int shift;
int window_base;
int window_index;

int last_packet = -1;
int last_ack = 0;
int next_seqno=0;
int send_base=0;
       
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
        int i = 0;    
        for (i = 0; i < WINDOW_SIZE; ++i)
        {
            /* 
             * resend all packets range between sendBase and nextSeqNum 
             */ 
            if(sendto(sockfd, window[i], TCP_HDR_SIZE + get_data_size(window[i]), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }
            printf("packet with seqno %d resent \n", window[i]->hdr.seqno);

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
	i = 0;
	while ( i < WINDOW_SIZE )
	{
		len = fread(buffer, 1, DATA_SIZE, fp);
		if (len<=0){
            window[i] = make_packet(0);
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
    		window[i] = make_packet(len);
     		memcpy(window[i]->data, buffer, len);
            window[i]->hdr.seqno = pkt_base;
            
        }
        i++;
	}

    /*
     * send the first ten packets  in the window
     */

    for ( i = 0; i < WINDOW_SIZE; ++i)
    {

        if(sendto(sockfd, window[i], TCP_HDR_SIZE + get_data_size(window[i]), 0, 
                ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto error");
        }
        printf("packet %d sent \n", window[i]->hdr.seqno);

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
            window_base = (last_ack) / DATA_SIZE;
            shift = ( recvpkt->hdr.ackno - last_ack )/ DATA_SIZE;
            last_ack = recvpkt->hdr.ackno;

            /* 
             * if you received an ack, calculate the new window and populate the empty part of it
             */


            /* step 1: calculate the new window by simultaneously deleting and freeing 
             * all packets in closed interval [base, base + shift], and secondly by copying all packets in 
             * the closed interval [base + shift + 1, windowsize-1] to new respective positions shift steps 
             * behind them in the window. 
             */

            window_index = 0; 
            while (window_index < WINDOW_SIZE)
            {
 
                if ( window_index >= window_base + shift ){
                    window[window_index-shift] = window[window_index];

                    VLOG(DEBUG, "generating window with index %d, window[window_index]->hdr.seqno %d shift %d ", window_index-shift, window[window_index-shift]->hdr.seqno, shift );

                }else{
                    free(window[window_index]);
                }
                window_index++;
            }


            /* step 2: populate the interval [ (base + windowize - 1) - shift), base + windowsize -1] with
             * the new packets. the "-1" is because base + windowsize - 1 gives index of the last element in window when full
             * and substracting shift gives us the index of last element when its not full, However, since a shift of one 
             * means replacing just the last element, two from second to last, etc., we substract shift + 1 so the 1's cancel
             */
            
            for ( window_index = (window_base + WINDOW_SIZE) - shift; window_index < window_base + WINDOW_SIZE ; window_index ++ )
            {
                len = fread(buffer, 1, DATA_SIZE, fp);
                if ( len <=0 ){
                    VLOG(INFO, " End Of File ");
                    window[window_index] = make_packet(1);
                }else{
                    pkt_base = next_seqno;
                    next_seqno = pkt_base + len; 
                    window[window_index] = make_packet(len);
                    memcpy(window[window_index]->data, buffer, len);
                    window[window_index]->hdr.seqno = pkt_base;
                    VLOG(DEBUG, "generating window with index %d, window[window_index]->hdr.seqno %d shift %d  ", window_index, window[window_index]->hdr.seqno, shift )

                    // last_packet = next_seqno;
                }
                 
 
            }

            /*
             * loop through the packet window that has just been made now and resend all the packets
             */
            VLOG(DEBUG, "sending window from base %d -> %s", 
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

        

        

    } while( 1 );

    /*
     *send 0 ack in closing
     */
    VLOG(INFO, "Empty file");
    sndpkt = make_packet(0);
    sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
            (const struct sockaddr *)&serveraddr, serverlen);
    free(sndpkt); 

   /* 
    * if you are at the end of the program, 
    * free the packets
    */
    for ( i = 0; i < WINDOW_SIZE; ++i)
    {
        free(window[i]);
    }   




    return 0;

}


