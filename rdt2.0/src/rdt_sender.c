/*
 * Nabil Rahiman
 * NYU Abudhabi
 * email: nr83@nyu.edu
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>

#include"packet.h"
#include"common.h"

#define STDIN_FD    0
#define RETRY  120 //millisecond 

int next_seqno=0;
int send_base=0;
int window_size = 1;

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
tcp_packet* window[10]; //added a packet window  

sigset_t sigmask;       


void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        //Resend all packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "Timeout happened");
        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
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
    int pkt_base;
    int portno, len;
    int next_seqno;
    int window_base;//added this to identify the base of each window
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

    /* check command line arguments */
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

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");


    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    //Stop and wait protocol

    init_timer(RETRY, resend_packets);
    next_seqno = 0;
    while (1)
    {	
    	window_base = next_seqno;
    	//loop 10 times to make 10 packets
    	int i = 0;
    	while ( i < 10 )//store the pointer to the i'th packet in window[i]
    	{
    		len = fread(buffer, 1, DATA_SIZE, fp);
    		if (len<=0){
    			VLOG(INFO, "End Of File has been reached and we may have gotten some packets from it");
    			break;
    		}
    		pkt_base = next_seqno;
        	next_seqno = pkt_base + len;
    		window[i] = make_packet(len);
    		VLOG(DEBUG, "window[i]=%lu", window[i] );
    		memcpy(window[i]->data, buffer, len);
	        window[i]->hdr.seqno = pkt_base;

    		VLOG(DEBUG, "window[i]->hdr.seqno=%d", window[i]->hdr.seqno );
	        i++;
    	}

        if ( len <= 0 && i == 0)
        {
            VLOG(INFO, "End Of File has been reached and we didn't get any packets on this iteration ");
            sndpkt = make_packet(0);
            sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                    (const struct sockaddr *)&serveraddr, serverlen);
	    free(sndpkt); 
            break;
        }        
        //Wait for ACK
        do {

            VLOG(DEBUG, "Sending 10 packets starting from seq number %d to %s", 
                    window_base, inet_ntoa(serveraddr.sin_addr));
            /*
             * If the sendto is called for the first time, the system will
             * will assign a random port number so that server can send its
             * response to the src port.
             */
            //since the i'th packet is stored in window[i], loop through and attempt to send each packet
            //should get an error based on the packet
            int i;
	    
            VLOG(DEBUG, "GOT HERE 1");


	    for (i = 0; i < 10; ++i)
            {
            	if(sendto(sockfd, window[i], TCP_HDR_SIZE + get_data_size(window[i]), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
	            {
	                error("sendto error");
 			VLOG(DEBUG, "error for pckt seq no %d", window[i]->hdr.seqno);
		    }
            }
	    VLOG(DEBUG, "GOT HERE 2");
          

            start_timer();
            //ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
            //struct sockaddr *src_addr, socklen_t *addrlen);

            do //keep receiving acks until you get the largest ack or the timer runs out (in background)
            {
            	/* code */
	            if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
	                        (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
	            {
	                error("recvfrom error");
	            }
	            recvpkt = (tcp_packet *)buffer;
	            printf("recvpckt-size %d \n", get_data_size(recvpkt));
	            assert(get_data_size(recvpkt) <= DATA_SIZE);
	        }while(recvpkt->hdr.ackno != next_seqno);
            
            stop_timer();
            /*resend the entire window if you don't recv ack for the window base */
        } while(recvpkt->hdr.ackno < window_base);
	int i;
	for ( i = 0; i < 10; ++i)
	{
	     free(window[i]);
	}
    }

    return 0;

}



