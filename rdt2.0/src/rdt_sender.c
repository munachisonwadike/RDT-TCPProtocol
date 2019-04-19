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
#define RETRY  120 /* millisecond */ 

int next_seqno=0;
int send_base=0;
int window_size = 1;
 
int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
tcp_packet* window[10]; /* array to store packet window */  

sigset_t sigmask;       


void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {

        VLOG(DEBUG, "RESEND FUNCTION TRIGGERED");   
        int i = 0;    
        for (i = 0; i < 10; ++i)
        {
            /* Resend all packets range between sendBase and nextSeqNum */ 
            if(sendto(sockfd, window[i], TCP_HDR_SIZE + get_data_size(window[i]), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }
            printf("packet with seqno %d just sent -- loop[%d] \n", window[i]->hdr.seqno, i );

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
    int shift;
    int window_base; /* to identify the base of each window */
    int window_old;
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

    /* Stop and wait protocol */

    init_timer(RETRY, resend_packets);

    /*
     * set the initial value of window base
     * loop 10 times to make 10 packets
     * store pointer to packet i in window[i]
     */
    next_seqno = 0;
	window_base = next_seqno;

	int i = 0;
	while ( i < 10 )
	{
		len = fread(buffer, 1, DATA_SIZE, fp);
		if (len<=0){
			VLOG(INFO, "End Of File has been reached and we may have gotten some packets from it");
			break;
		}
		pkt_base = next_seqno;
    	next_seqno = pkt_base + len;
		window[i] = make_packet(len);
 		memcpy(window[i]->data, buffer, len);
        window[i]->hdr.seqno = pkt_base;
        i++;
	}

    if ( len <= 0 && i == 0)
    {
        VLOG(INFO, "End Of File has been reached and we didn't get any packets on this iteration ");
        sndpkt = make_packet(0);
        sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                (const struct sockaddr *)&serveraddr, serverlen);
        free(sndpkt); 
        exit(1);
    }        

    int stop = 0; 

    /*
     * constantly send the packets, wait for acks, 
     * and slide the window up for the next iteration of this loop
     */
    do {


        VLOG(DEBUG, "Sending window starting at seq number %d to %s", 
                window_base, inet_ntoa(serveraddr.sin_addr));
        /*
         * If the sendto is called for the first time, the system will
         * will assign a random port number so that server can send its
         * response to the src port.
         */
        

        /*
         * since the i'th packet is stored in window[i], loop through and attempt to send each packet
         * should get an error based on the packet
         */
        int i;
	    for (i = 0; i < 10; ++i)
        {
        	if(sendto(sockfd, window[i], TCP_HDR_SIZE + get_data_size(window[i]), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto error");
            }
            printf("packet with seqno %d just sent \n", window[i]->hdr.seqno);

        }
          
        /* 
         * start the timer right after sending and while waiting for ACKS
         * this is the only time that gets restarted every time we shift the window
         */
        start_timer();

        /* if you get an ack, process it and stop timer */ 
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                    (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
        {
            error("recvfrom error");
        }
        recvpkt = (tcp_packet *)buffer;

        assert(get_data_size(recvpkt) <= DATA_SIZE);

        /* if you received an ack, calculate the packet number relative to current sending windo to shift to that packet */
        shift = ( recvpkt->hdr.ackno - window_base ) / DATA_SIZE ; 
        
        /* shift to new window */
        window_old = window_base;
        window_base = window[shift]->hdr.seqno;
        printf( "just received ack number %d causing shift %d while the window_base goes from %d to %d \n",  recvpkt->hdr.ackno, shift, window_old, window_base );
        stop_timer();


        /* 
         * populate the empty part of the new window 
         * note you won't send out the content of the window till the next loop iteration 
         */     
        int j; 
        for ( j = 0 ; j < 10 - shift ; ++j )
        {

            window[j] = window[j + shift]; 
        }
        /* 'next_seqno' becomes the seq number for the last packet in new window */
        for ( j = 10 - shift ; j < 10 ; ++j )
        {
            len = fread(buffer, 1, DATA_SIZE, fp);
            if ( len <=0 ){
                VLOG(INFO, "End Of File has been reached and we may have gotten some packets from it");
                window[j] = make_packet(0);
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
         * if you are at the end of the file, 
         * go into a loop to send all the remaining packets 
         */
        int k;
        if( stop == 1 )
        {   
            printf("WE ARE NOW IN THE STOP STATEMENT\n");            
            do
            {  

                printf("GOT TO THE LOOP\n");
                /* send the packets */
                for ( k = 0; k < 10; ++k)
                {
                    printf("kth value of loop = %d\n", k);
                    if(sendto(sockfd, window[k], TCP_HDR_SIZE + get_data_size(window[k]), 0, 
                            ( const struct sockaddr *)&serveraddr, serverlen) < 0)
                    {
                        error("sendto error");
                    }
                    printf("packet with seqno %d just sent \n", window[k]->hdr.seqno);

                }
          
                start_timer();
                /* if you get an ack, process it and stop timer - timer ensures you don't wait indefinitely */
                if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                            (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
                {
                    error("recvfrom error");
                }
                recvpkt = (tcp_packet *)buffer;
                assert(get_data_size(recvpkt) <= DATA_SIZE);
                printf( "just received ack number %d causing shift %d  for FINAL window %d \n",  recvpkt->hdr.ackno, shift, window_base );
                stop_timer();
               
                /* shrink the window start closer to the end by setting to the highest acked packet */

            }while( recvpkt->hdr.ackno > 0 );   


            /* after sending the last window off, send a 0 packet so that the receiver knows to close itself */
            VLOG(INFO, "End Of File has been reached and time to stop ");
            sndpkt = make_packet(0);
            sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                    (const struct sockaddr *)&serveraddr, serverlen);
            free(sndpkt); 
            exit(1);
        
        }

    } while(!stop);
    VLOG(DEBUG, "ACK for packets before the one with seq no %d received", recvpkt->hdr.ackno);

   
    /* free the packets allocated with malloc */   
    for ( i = 0; i < 10; ++i)
    {
        free(window[i]);
    }




    return 0;

}



