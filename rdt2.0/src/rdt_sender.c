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

int needed_ack = 0;
int next_seqno=0;
int send_base=0;
int stop = 0;

       
struct sockaddr_in serveraddr;
struct itimerval timer; 


int j;
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

int i;
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
            if(i==0){
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


    for ( i = 0; i < WINDOW_SIZE; ++i)
    {

        if(sendto(sockfd, window[i], TCP_HDR_SIZE + get_data_size(window[i]), 0, 
                ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto error");
        }
        printf("packet %d sent \n", window[i]->hdr.seqno);

    }

    start_timer();  
    /*
     * constantly send the packets, wait for acks, 
     * and slide the window up for the next iteration of this loop
     */
    do 
    {

         /* 
         * start the timer right after sending and while waiting for ACKS
         * this is the only time that gets restarted every time we shift the window
         */
        

        
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                    (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
        {
            error("recvfrom error");
        }
        
        recvpkt = (tcp_packet *)buffer;

        printf(" ack receieved - %d | needed_ack - %d \n", recvpkt->hdr.ackno, needed_ack );
        assert(get_data_size(recvpkt) <= DATA_SIZE);


        if(recvpkt->hdr.ackno >= needed_ack)
        {   
            /* 
             * if you get an ack, stop time to process it 
             */ 
            stop_timer();

            needed_ack = recvpkt->hdr.ackno;
            /* 
             * if you received an ack, calculate the new window 
             */
            int i = 0; int shift = 0;
            while (i < WINDOW_SIZE)
            {
 
                if ( window[i]->hdr.seqno >= recvpkt->hdr.ackno ){
                    window[i-shift] = window[i];

                }else{
                    free(window[i]);
                    shift++;
                }
                i++;
            }
            /* 
             * populate the empty part of the new window note you won't send
             * out the content of the window till the next loop iteration 'next_seqno' becomes the seq number 
             * for the last packet in new window 
             */
            
            for ( j =  WINDOW_SIZE - shift ; j < WINDOW_SIZE ; ++j )
            {
                len = fread(buffer, 1, DATA_SIZE, fp);
                if ( len <=0 ){
                    VLOG(INFO, " End Of File ");
                    window[j] = make_packet(1);
                    window[j]->hdr.seqno = 0; /*send an older packet number so the sender ignores these
                                                just a filler so it doesn't close*/
                    stop = 1;
                }else{
                    pkt_base = next_seqno;
                    next_seqno = pkt_base + len; 
                    window[j] = make_packet(len);
                    memcpy(window[j]->data, buffer, len);
                    window[j]->hdr.seqno = pkt_base;
                }
                 
 
            }

            /*
             * since the i'th packet is stored in window[i], loop through 
             * and attempt to send each packet should get an error based on the packet
             */
            VLOG(DEBUG, "sending window from base %d -> %s", 
                window[0]->hdr.seqno, inet_ntoa(serveraddr.sin_addr));       

            for (i = WINDOW_SIZE - shift; i < WINDOW_SIZE; ++i)
            {
                if(sendto(sockfd, window[i], TCP_HDR_SIZE + get_data_size(window[i]), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
                {
                    error("sendto error");
                }
                printf("packet %d sent \n", window[i]->hdr.seqno);

     
            }

              
            start_timer(); 
        }

        if (stop == 1){
            break;
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



