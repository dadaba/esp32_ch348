#ifndef __RINGBUFFER_H__
#define __RINGBUFFER_H__
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#define RING_BUFFER_SIZE  2048   //缓冲区的长度,可以修改

typedef unsigned char UCHAR;
typedef unsigned char BYTE;

struct s_ringbuffer{
    portMUX_TYPE lock;
	//unsigned long validLen;        //已使用的数据长度 
    UCHAR* pHead;                  //环形存储区的首地址 
    UCHAR* pTail;                  //环形存储区的结尾地址 
    UCHAR* pValid;                 //已使用的缓冲区的首地址 
    UCHAR* pValidTail;             //已使用的缓冲区的尾地址
    unsigned int bufferSize;
//	BYTE RcvBuf[RING_BUFFER_SIZE]; //接收缓冲区    
};

void initRingbuffer( struct s_ringbuffer * p_rb); 
void initRingbufferWithSize( struct s_ringbuffer * p_rb, int ring_buffer_size);
long writeRingbuffer( UCHAR* buffer,unsigned long addLen, struct s_ringbuffer * p_rb );
long readRingbuffer(UCHAR* buffer,unsigned long len, struct s_ringbuffer * p_rb );
unsigned long getRingbufferValidLen( struct s_ringbuffer * p_rb );
unsigned long getRingbufferFreeLen( struct s_ringbuffer * p_rb );
void releaseRingbuffer( struct s_ringbuffer * p_rb );

long writeRingbufferNoLock( UCHAR* buffer,unsigned long addLen, struct s_ringbuffer * p_rb );
long readRingbufferNoLock(UCHAR* buffer,unsigned long len, struct s_ringbuffer * p_rb );
unsigned long getRingbufferValidLenNoLock( struct s_ringbuffer * p_rb );
unsigned long getRingbufferFreeLenNoLock( struct s_ringbuffer * p_rb );
void releaseRingbufferNoLock( struct s_ringbuffer * p_rb );
long copyRingbuffer( UCHAR* buffer,unsigned long len, struct s_ringbuffer * p_rb);

#endif
