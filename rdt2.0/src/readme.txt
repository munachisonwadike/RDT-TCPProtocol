For Task 1, we were asked to edit the starter code to increase the window 
size to ten packets.

In order to do this, we would need to ensure that we can send up to ten 
packets at a time in order. Each packet has its own sequence number.

The recommndation, as per the third paragraph of 3.5.4, is there is 
only one retransmission timer for the entirety of transmission. Unlike in 
go back n, according to the last paragraph of 3.5.4, TCP retransmits at 
most one packet, and wouldn't retransmit packet n if the ack for packet 
n+1 arrived before the timeout for packet n.

At the sender, We implement the sliding window by setting window base at 
the largest acked packet whenever we receive an ack, and then send all 
the new packets that fall within the window that haven't been sent. When we
wish to send the packets, we simply generate them all in a loop and send them
all in a loop, knowing it is always at most 10 packets. Whenever the singular
timeout expires, resend the packet that we received the dupliacte ACK for.

The receiver only needs to send the ack for packet n, and in all other cases,
resend the ack for the most recently received in-order packet showing it expects
the next one. Just as in go back n explained in 3.4.3, if packet n has been 
received, it means all packets with lower sequence numbers have also been received. 
See table 3.2 for details of ack generation followed

Since we only have one timer in the simplified version, and since we send acks
cumulatively, we just need to shift the window base to the highest ack received
and reset the timer to start as we send the packet at the base of the window.
Also, for the same reasons, if we send all the packets in a window and all the acks
arrive before the timeout except the first packet, then we must resend all of them
