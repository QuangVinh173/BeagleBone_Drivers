#ifndef MAIN_H
#define MAIN_H

/* Control */
#define PRINT_DEBUG 0
#define PRINT_DESCRIPTOR 1 /* Print descriptor info */

/* See the sync between OUT and IN transfer */
#define PRINT_DEBUG_TX_RX_SYNC 0

#define BUG_NOT_ENOUGH_BYTE_RX 0
/* - [BUG1] This bug is highly likely occured when send more than FIFO threshold of device 
   - In kernel, when we try to lock the whole write operation for echoing out. Especially
        when the bytes are > FIFO threshold, then if device FIFO is full, it expects the host 
        to send IN token to flush data out of FIFO for the next incomning bytes, but host
        continues to send more bytes while device sends NACK to stop getting bytes
        => nothing is solved.
    */

#endif
