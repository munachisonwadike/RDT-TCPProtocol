This is an implementation of the TCP reliable data transfer protocol.
We send a file across a lossy network, simulated using mininet, with 
two C files- a sender and receiver.

At the sender, We implement the sliding window by setting window base at 
the largest acked packet (whenever an ack is received by sender), and then send  
the new packets that fall within the window that haven't been sent. As per
the relevant TCP rcf, there is only one retransmission timer for the 
entirety of transmission, linked to the packet at the head of the window.
Whenever the timeout expires, the receiver resends the packet window.
Through Fast Retransmit, we also resend the window if the receiver gets a triplicate
ack for the packet at the head of the window.We implemented the sender window as an 
array of packets and read the bytes of the file being sent into the packets as needed.
 
We ensure that the receiver buffers out of sequence up to a certain extent
packets, as well as that the sender does not send packets outside of 
the window of the receiver. To do this, need to we created a buffer where 
the receiver places out of sequence packets. Whenever packets are received,
the receiver simply checks the buffer whether it has received packets contiguous with
the one just received, in the past, and sends a cumulative ack for all those packets.

Following the relevant RFCs, we also implement TCP  slowstart, and congestion avoidance
A csv file showing the way the window size changes over time is included for visualisation
purposes. Note that in order to allow all the packets to fit in memory, we set a max value
for ssthresh to be initially fixed, but large, this way, if there is no loss on the 
network, the value of the window of packets will not be too large. Also, we distinguish
between the maximum value size of the window (which is the same as the initial value of
ssthresh or the maximum amount of packets initially sent) and the size of the congestion 
window. For modularity, we make the latter a second variable called cwnd. It is this variable
which we change, and which can be visualised using the python script.

