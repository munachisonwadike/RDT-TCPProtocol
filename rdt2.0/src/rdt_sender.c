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
#define RESEND  1000 /* milliseconds */ 



char buffer[DATA_SIZE];
char *hostname;
    
int final_loop;
int pkt_base;
int portno; 
int len;
int next_seqno;
int sockfd;
int serverlen;
int shift;
int window_index;


int avoidance = 0; /* moves protocol from slow start to congestion avoidance */
int final_packet_reached = 0;
int last_packet = -1;
int last_ack = 0;
int next_seqno=0;
int send_base=0;


int CWND_SIZE = 1; /* actual value of effective window which we change in slow start and congestion avoidance - use plot.py to visualise*/
int WINDOW_SIZE = 100; 
int SSTHRESH = 100; /* set the initial value of SSTHRESH finite due to memory constraints */

struct sockaddr_in serveraddr;
struct itimerval timer; 


int i; int j;
volatile int k = -1;

FILE *csv_file;
FILE *fp;


sigset_t sigmask;  

tcp_packet *sndpkt;
tcp_packet *recvpkt;
tcp_packet* window[100]; /* array to store packet window */  

     

/*
 * resend function for when a TCP timeout is triggered - resend the entire current window
 */
void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        VLOG(DEBUG, "RESEND FUNCTION TRIGGERED");   
        /*
         * if timeout triggered, then a packet's lost
         * we need to decrease window, reset CWND and 
         * halve SSTHRESH
         */
        if( avoidance == 0){
            CWND_SIZE = 1;
            SSTHRESH = CWND_SIZE / 2;
            avoidance = 1;
        }else{
            CWND_SIZE = 1;
            SSTHRESH = (CWND_SIZE / 2) + 3;
        }


        
        VLOG(DEBUG, "[RE]sending window of size %d/%d from base %d -> %s", CWND_SIZE, WINDOW_SIZE,  
            window[0]->hdr.seqno, inet_ntoa(serveraddr.sin_addr));  

        /*
         * then resend the packets in the congestion window 
         */

        for (window_index = 0; window_index < CWND_SIZE; window_index++)
        {
            if(sendto(sockfd, window[window_index], TCP_HDR_SIZE + get_data_size(window[window_index]), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto error");
            }
            printf("packet %d [RE]sent \n", window[window_index]->hdr.seqno);

 
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
     * initialise timer 
     */
    init_timer(RESEND, resend_packets);


    /*
     * intialise the csv file where you will output the size of the window
     */

    csv_file = fopen("CWND", "w");
    if(csv_file == NULL){
        printf("Couldn't open CSV file\n");
        exit(1);
    }
    
    


    /*
     * set the initial value of window base, fill window with packets
     * store pointer to packet i in window[i]
     */
    next_seqno = 0;
	window_index = 0;
	while ( window_index < WINDOW_SIZE )
	{
        VLOG(INFO, " LOOP %d ", window_index);
		len = fread(buffer, 1, DATA_SIZE, fp);
		if (len <  DATA_SIZE){ 
            VLOG(INFO, " End Of File (1). Window size %d ", WINDOW_SIZE);
            
            final_packet_reached = 1;

            if (len > 0 ){
                pkt_base = next_seqno; /* sequence number of the packet being sent */

                WINDOW_SIZE = window_index + 1; /* since this packet counts, window size is just the index of this packet plus one */
                window[window_index] = make_packet(len);
                /* identify current as final packet with control flag set to -2 */
                window[window_index]->hdr.ctr_flags = -2; 
                /* zero out the ack number for usage in fast retransmit */
                window[window_index]->hdr.ackno = 0;
                /* copy data into the packet */
                memcpy(window[window_index]->data, buffer, len);
                /* stamp the sequence number on */
                window[window_index]->hdr.seqno = pkt_base;
            /*
             * else, just identify last packet in window as the last and move on
             */
            }else if (len == 0){
                window[window_index-1]->hdr.ctr_flags = -2; /* identify last as final packet with control flag set to -2 */
            }     


		}else{ 
            pkt_base = next_seqno;
        	next_seqno = pkt_base + len;
    		window[window_index] = make_packet(len);
            
            /* zero out the control flags since reciever needs it to tell if its the last packet */
            window[window_index]->hdr.ctr_flags = 0;
            /* zero out the ack number for usage in fast retransmit */
            window[window_index]->hdr.ackno = 0;

     		/* copy data into the packet */
            memcpy(window[window_index]->data, buffer, len);
            /* stamp the sequence number on */
            window[window_index]->hdr.seqno = pkt_base;
            
        }
        window_index++;
	}

    /*
     * send the first set of packet in the window
     */
    for ( window_index = 0; window_index < CWND_SIZE; window_index++)
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
     * and slide the window up for the next 
     * iteration of do-while loop
     */ 
    do 
    {
        /* send the value of window size to the csv before receiving a packet */
        fprintf(csv_file, "%d\n", CWND_SIZE);

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


        
        if( recvpkt->hdr.ackno == last_packet ){
            printf("last_packet received %d, recvpkt->hdr.ackno %d\n", last_packet, recvpkt->hdr.ackno );
            exit(1);
        }

        

        /*
         * since the receiver only sends acks to indicate 
         * which it needs, we can accept acks >= last ack
         */
        if(recvpkt->hdr.ackno >= last_ack)
        {   
            /* 
             * if you get an ack, stop time to process it 
             */ 
            stop_timer();

            
            /*
             * check how much far ahead in window the 
             * recvd packet is than the base for needed 
             * acks and set new needed ack to the the 
             * received packet number 
             */           
            shift = ( recvpkt->hdr.ackno - last_ack )/ DATA_SIZE;
            last_ack = recvpkt->hdr.ackno;

            /* 
             * if you received an ack, calculate the new window and populate the empty part of it
             */

            /* option 1, step 1 if we haven't reach last packet, calculate the new 
             * window by simultaneously deleting and freeing all packets in closed 
             * interval [0, shift], and secondly by copying all packets in the closed 
             * interval [shift + 1, windowsize-1] to new respective positions shift steps 
             * behind them in the window. 
             */
            if (final_packet_reached == 0){
                window_index = 0; 
                while (window_index < WINDOW_SIZE)
                {
     
                    if ( window_index >= shift ){

                        window[window_index-shift] = window[window_index];

                    }else{
                        free(window[window_index]);
                    }
                    window_index++;
                }
                /* option 1, step 2 - then populate the interval [windowize - shift, windowsize -1] with
                 * the new packets. concerning the left endpoint, windowsize - shift==1 gives index of the 
                 * last element in window if shift is 0, the loop will not run becaause window_index will == windowsize
                 */
            
                for ( window_index =  WINDOW_SIZE - shift; window_index < WINDOW_SIZE ; window_index ++ )
                {
                    len = fread(buffer, 1, DATA_SIZE, fp);
                    if ( len < DATA_SIZE ){
                        
                        
                        VLOG(INFO, " End Of File (2). Window size %d ", WINDOW_SIZE);
                        final_packet_reached = 1;

                        if (len > 0 ){
                            pkt_base = next_seqno; /* sequence number of the packet being sent */

                            WINDOW_SIZE = window_index + 1; /* since this packet counts, window size is just the index of this packet plus one */
                            window[window_index] = make_packet(len);
                            /* identify current as final packet with control flag set to -2 */
                            window[window_index]->hdr.ctr_flags = -2; 
                            /* zero out the ack number for usage in fast retransmit */
                            window[window_index]->hdr.ackno = 0;
                            /* copy data into the packet */
                            memcpy(window[window_index]->data, buffer, len);
                            /* stamp the sequence number on */
                            window[window_index]->hdr.seqno = pkt_base;
                        /*
                         * else, just identify last packet in window as the last and move on
                         */
                        }else if (len == 0){
                            window[window_index-1]->hdr.ctr_flags = -2; /* identify last as final packet with control flag set to -2 */
                        }     

                        break;

                    }else{
                        pkt_base = next_seqno; /* sequence number of the packet being sent */

                        next_seqno = pkt_base + len; 
                        window[window_index] = make_packet(len);

                        /* zero out the control flags since we will need them */
                        window[window_index]->hdr.ctr_flags = 0;
                        /* zero out the ack number for usage in fast retransmit */
                        window[window_index]->hdr.ackno = 0;
                        /* copy data into the packet */
                        memcpy(window[window_index]->data, buffer, len);
                        /* stamp the sequence number on */
                        window[window_index]->hdr.seqno = pkt_base;

                    }
                }

            }else{            
            /* option 2, step 1 if we reach last packet, calculate the new window by simultaneously deleting 
             * and freeing all packets in closed interval [0, shift], and secondly by copying all packets in 
             * the closed interval [shift + 1, windowsize-1] to new respective positions shift steps 
             * behind them in the window. 
             */
                window_index = 0; 
                while (window_index < WINDOW_SIZE)
                {
     
                    if ( window_index >= shift ){
                        window[window_index-shift] = window[window_index];

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
                
                

            }

            


            /* 
             * if the ack we got was the same as the last ack, increment its ack number 
             * by 1 for fast retransmit and resend the window if the ack number == 3
             *  
             */

            if ( shift == 0 ){
                window[0]->hdr.ackno ++;

                if ( window[0]->hdr.ackno == 3 )
                {


                    /*
                     * if we are doing a retransmit, then we a packet was lost as we need to decrease window
                     * if the congestion window exceeds the threshold, reset the former and halve the latter
                     */

                    if( avoidance == 0){
                        CWND_SIZE = 1;
                        SSTHRESH = CWND_SIZE / 2;
                        avoidance = 1;
                    }else{
                        CWND_SIZE = 1;
                        SSTHRESH = (CWND_SIZE / 2) + 3;
                    }


                    VLOG(DEBUG, "sending window of size %d/%d from base %d -> %s", CWND_SIZE, WINDOW_SIZE,  
                        window[0]->hdr.seqno, inet_ntoa(serveraddr.sin_addr));       

                    for (window_index = 0; window_index < CWND_SIZE; window_index++)
                    {
                        if(sendto(sockfd, window[window_index], TCP_HDR_SIZE + get_data_size(window[window_index]), 0, 
                                ( const struct sockaddr *)&serveraddr, serverlen) < 0)
                        {
                            error("sendto error");
                        }
                        printf("packet %d sent \n", window[window_index]->hdr.seqno);

             
                    }
                }
                
                
            /* 
             * if the ack we got was different from the last one, then we window slid up and we 
             * only need to send the whole window 
             */
            } else {

                /*
                 * if we are sending a new window, it means a packet was successfully received,
                 * so increase the congestion window, and decrease it accordingly if it exceeds 
                 * ssthresh  
                 */

                if( avoidance == 0){

                    CWND_SIZE  = CWND_SIZE * 2;

                    if (CWND_SIZE >= SSTHRESH){
                        CWND_SIZE = 1;
                        SSTHRESH = CWND_SIZE / 2;
                        avoidance = 1;
                    }

                }else{

                    CWND_SIZE++;

                    if (CWND_SIZE >= SSTHRESH){
                        CWND_SIZE = 1;
                        SSTHRESH = (CWND_SIZE / 2) + 3;
                    }

                }

                VLOG(DEBUG, "sending window of size %d/%d from base %d -> %s", CWND_SIZE, WINDOW_SIZE,  
                    window[0]->hdr.seqno, inet_ntoa(serveraddr.sin_addr));       

                for (window_index = 0; window_index < CWND_SIZE; window_index++)
                {
                    if(sendto(sockfd, window[window_index], TCP_HDR_SIZE + get_data_size(window[window_index]), 0, 
                            ( const struct sockaddr *)&serveraddr, serverlen) < 0)
                    {
                        error("sendto error");
                    }
                    printf("packet %d sent \n", window[window_index]->hdr.seqno);

         
                }
            }


            start_timer(); 
        }
 
    } while( 1 );

    
    


    fclose(csv_file);

    return 0;

}


