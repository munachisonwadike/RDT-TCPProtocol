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
int last_buffered; /* to note the last byte buffered in receiver window */
int optval; /* flag value for setsockopt */
int portno; /* port to listen on */
int sockfd; /* socket */
int windex;
int window_index;

int needed_pkt = 0; /* int to ensure that we don't allow for out of order packets*/
int stop = 0;

int FINAL_SEND = 50; /* number of times to send off the ack for last packet */
int RCV_WIND_SIZE = 10; /* receiver window */

struct sockaddr_in serveraddr; /* server's addr */
struct sockaddr_in clientaddr; /* client addr */
struct timeval tp; 

FILE *fp; /* pointer for output file */

sigset_t sigmask;  

tcp_packet *recvpkt;
tcp_packet *sndpkt;
tcp_packet *rcv_window[10]; /* buffer for out of sequence packets */  





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


    /* 
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    clientlen = sizeof(clientaddr);


    /*
     * set up the receiver buffer
     */

    for (windex = 0; windex < RCV_WIND_SIZE ; windex++)
    {
        /* 
         * fill receive window with empty packets where the ack number is -1. If we buffer a packet in a given
         * slot in the window (by copying it from a socket), then its ack number 0 or greater already
         */        
        rcv_window[windex] = make_packet(DATA_SIZE);
        rcv_window[windex]->hdr.ackno = -1;
    }


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

        // printf("JUST RECEIVED PACKET %d with flags %d ", recvpkt->hdr.seqno, recvpkt->hdr.ctr_flags);

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

            printf("JUST RECEIVED (NEEDED) PACKET %d with flags %d ", recvpkt->hdr.seqno, recvpkt->hdr.ctr_flags);


            gettimeofday(&tp, NULL);
            VLOG(DEBUG, " %lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
            // memset ( rcv_window[0], 0, TCP_HDR_SIZE+DATA_SIZE );
            memcpy(rcv_window[0], recvpkt, TCP_HDR_SIZE + DATA_SIZE);

            /* 
             * if the packet you receieved was the last packet, 
             * exit the program
             */
             if ( rcv_window[0]->hdr.ctr_flags == -2) {  

                fseek(fp, rcv_window[0]->hdr.seqno, SEEK_SET);
                printf("Writing (3) last packet to output file with seqno %d\n", rcv_window[0]->hdr.seqno);
                fwrite(rcv_window[0]->data, 1, rcv_window[0]->hdr.data_size, fp);


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
                VLOG(INFO, "Receieved last packet (1), exiting program. Sent closure ack (3)/1. Please be patient!");
                fclose(fp);
                free(sndpkt);
                exit(0);
            }       

            /*
             * write all contiguously buffered packets starting with the one just received to 
             * the output file. call the last one contiguously buffered "last buffered" 
             */
            // window_index = 0;
            // do
            // {   
            //     if(window_index==0){
            //         last_buffered = window_index;
                    fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
                    printf("\nWriting (2) contiguous packets to the file - iteration [%d], seqno %d\n", window_index, recvpkt->hdr.seqno);
                    fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);
            //         window_index++;
            //     }else{
            //         last_buffered = window_index;
            //         fseek(fp, rcv_window[window_index]->hdr.seqno, SEEK_SET);
            //         printf("\nWriting (2) contiguous packets to the file - iteration [%d], seqno %d\n", window_index, rcv_window[window_index]->hdr.seqno);
            //         fwrite(rcv_window[window_index]->data, 1, rcv_window[window_index]->hdr.data_size, fp);
            //         window_index++;
            //     }
                

            // }while ( ( rcv_window[window_index]->hdr.ackno != -1 ) && ( window_index < RCV_WIND_SIZE ) );
            
            // printf("last-buffered after the writing loop has value %d\n", last_buffered);
             /* update the number of the expected packet */
            // needed_pkt = rcv_window[last_buffered]->hdr.seqno + rcv_window[last_buffered]->hdr.data_size;
            // VLOG(DEBUG, "NEEDED PACKET HAS A VALUE OF %d", needed_pkt);

            // VLOG(DEBUG, "Last buffered packet is %d", rcv_window[last_buffered]->hdr.seqno );

            needed_pkt = recvpkt->hdr.seqno + recvpkt->hdr.data_size;
            VLOG(DEBUG, "NEEDED PACKET HAS A VALUE OF %d", needed_pkt);

            VLOG(DEBUG, "Last buffered packet is %d", recvpkt->hdr.seqno );

             

            /*
             * copy any packet in closed interval [last_buffered + 1, windowsize-1] to
             * 'last-buffered+ 1' steps behind it in the window and zero out its own positioning and
             *  making sure to have first freed what was last-buffered steps behind.
             */

            // window_index = 0; 
            // while (window_index < RCV_WIND_SIZE)
            // {   
                

            //     if ( window_index > last_buffered ){
            //         // memset ( rcv_window[0], 0, TCP_HDR_SIZE + DATA_SIZE );
            //         memcpy(rcv_window[window_index - (last_buffered + 1)], rcv_window[window_index], TCP_HDR_SIZE + DATA_SIZE);
            //         rcv_window[window_index]->hdr.ackno = -1;
            //         // VLOG(DEBUG, "copying index %d to index %d window size %d ", 
            //         //     window_index, window_index - (last_buffered + 1) , RCV_WIND_SIZE )
            //     }
            //     window_index++;
            // }

            /*
             * send an ack for the next needed packet 
             */
            sndpkt = make_packet(0);
           
            sndpkt->hdr.ctr_flags = 1; /* type (1) ack  - send the next one naturally */
            sndpkt->hdr.ackno = needed_pkt;
            
            VLOG(DEBUG, "after receipt needed_pkt has a value %d", needed_pkt );

	        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
	                (struct sockaddr *) &clientaddr, clientlen) < 0) {
	            error("ERROR in sendto");
	        }
	        VLOG( DEBUG, "sending ack (1) number %d", needed_pkt );  
            


                           
        /*
         * if the packet is higher than what is needed
         * send a duplicate ack, after buffering the out of order packet
         */
        
        } else if ( recvpkt->hdr.seqno > needed_pkt ) {
            
            /* used ( x + y - 1 ) / y to get ceiling of x/y in C - trying to get the right index value */
            window_index = ( (recvpkt->hdr.seqno - needed_pkt ) + DATA_SIZE - 1 ) / DATA_SIZE;


            memcpy(rcv_window[window_index], recvpkt, rcv_window[window_index]->hdr.data_size);

            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = needed_pkt;
            sndpkt ->hdr.ctr_flags = 2; /* type (2) ack - packet is higher than needed out of order */
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            printf("sending duplicate ack (2) number %d\n", needed_pkt );


        }else if ( recvpkt->hdr.seqno < needed_pkt ) {
        
            

            /*
             * the number shouldn't be lower than needed
             * so send duplicate ack to specify the one you needed 
             */     

            // sndpkt = make_packet(0);
            // sndpkt->hdr.ackno = needed_pkt;
            // sndpkt->hdr.ctr_flags = 4; /* type (4) ack - lower than expected */
            // if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
            //         (struct sockaddr *) &clientaddr, clientlen) < 0) {
            //     error("ERROR in sendto");
            // }
            // printf("sending duplicate ack (4) number %d\n", needed_pkt );

         
            
        }
        // printf("\n\n");
 
    }


    return 0;
}
