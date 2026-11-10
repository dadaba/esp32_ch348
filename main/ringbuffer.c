#include <stdio.h> 
#include <stdlib.h> 
#include <string.h> 
// #include <assert.h>
#include "ringbuffer.h"
#include "freertos/FreeRTOS.h"
#include "portmacro.h"
#include "spinlock.h"
#include "esp_log.h"

static unsigned long _getRingbufferValidLen( struct s_ringbuffer * p_rb, bool lock);

/*************************************************************
 * function:初始化环形缓冲区
 * param:@p_rb 环形缓冲区指针
 * return:无
 *************************************************************/
void _initRingbuffer( struct s_ringbuffer * p_rb, int size) 
{ 	
	if((*p_rb).pHead == NULL) { 
		(*p_rb).pHead = (UCHAR*) malloc(size); 
	} 
	portMUX_INITIALIZE(&p_rb->lock);
	portENTER_CRITICAL_SAFE(&p_rb->lock);	
	(*p_rb).pValid = (*p_rb).pValidTail = (*p_rb).pHead; 
	(*p_rb).pTail = (*p_rb).pHead + size; 
	(*p_rb).bufferSize = size;
	portEXIT_CRITICAL_SAFE(&p_rb->lock);

}

void initRingbuffer( struct s_ringbuffer * p_rb) 
{
	_initRingbuffer(p_rb, RING_BUFFER_SIZE);
}

void initRingbufferWithSize( struct s_ringbuffer * p_rb, int ring_buffer_size) 
{
	_initRingbuffer(p_rb, ring_buffer_size);
}

/**************************************************************
 * function:向缓冲区中写入数据
 * param:@buffer 写入的数据指针
 *       @addLen 写入的数据长度
 * return:-1:写入长度过大
 *        -2:缓冲区没有初始化
 ***************************************************************/ 
static long _writeRingbuffer( UCHAR* buffer,unsigned long addLen, struct s_ringbuffer * p_rb, bool lock)
{ 
	if(addLen > p_rb->bufferSize) 
		return -2; 
	if( (*p_rb).pHead==NULL ) 
		return -1; 
	
	if(lock) portENTER_CRITICAL_SAFE(&p_rb->lock);

	// ASSERT(buffer); //将要存入的数据copy到pValidTail处 
	unsigned long validLen = _getRingbufferValidLen(p_rb, false);

	if( (*p_rb).pValidTail + addLen > (*p_rb).pTail)//需要分成两段copy 
	{ 
		long len1 = (*p_rb).pTail - (*p_rb).pValidTail; 
		long len2 = addLen - len1; 
		memcpy( (*p_rb).pValidTail, buffer, len1); 
		memcpy( (*p_rb).pHead, buffer + len1, len2); 
		
        (*p_rb).pValidTail = (*p_rb).pHead + len2;//新的有效数据区结尾指针
	}
	else 
	{ 
		memcpy( (*p_rb).pValidTail, buffer, addLen); 
		(*p_rb).pValidTail += addLen;//新的有效数据区结尾指针 

	} //需重新计算已使用区的起始位置 

	if(validLen + addLen > p_rb->bufferSize) 
	{ 
		long moveLen = validLen + addLen - p_rb->bufferSize;//有效指针将要移动的长度 
		if((*p_rb).pValid + moveLen > (*p_rb).pTail)//需要分成两段计算 
		{ 
			long len1 = (*p_rb).pTail - (*p_rb).pValid; 
			long len2 = moveLen - len1; 
			(*p_rb).pValid = (*p_rb).pHead + len2; 
		}
		else 
		{ 
			(*p_rb).pValid = (*p_rb).pValid + moveLen; 
		} 
	}

	if(lock) portEXIT_CRITICAL_SAFE(&p_rb->lock);

	return 0; 
} 

long writeRingbuffer( UCHAR* buffer,unsigned long addLen, struct s_ringbuffer * p_rb)
{
	return _writeRingbuffer(buffer, addLen, p_rb, true);
}

long writeRingbufferNoLock( UCHAR* buffer,unsigned long addLen, struct s_ringbuffer * p_rb)
{
	return _writeRingbuffer(buffer, addLen, p_rb, false);
}
/**************************************************************
 * function:从缓冲区内取出数据
 * param   :@buffer:接受读取数据的buffer
 *          @len:将要读取的数据的长度
 * return  :-1:没有初始化
 *          >0:实际读取的长度
 ***************************************************************/ 
long _readRingbuffer( UCHAR* buffer,unsigned long len, struct s_ringbuffer * p_rb, bool lock) 
{ 	
	if((*p_rb).pHead==NULL) 
		return -1; 	
	// ASSERT(buffer); 
	unsigned long validLen = _getRingbufferValidLen(p_rb, false);

	if(validLen == 0) 
		return 0; 

	if( len > validLen) 
		len = validLen; 
	if((*p_rb).pValid + len > (*p_rb).pTail)//需要分成两段copy 
	{ 
		long len1 = (*p_rb).pTail - (*p_rb).pValid; 
		long len2 = len - len1; 
		memcpy( buffer, (*p_rb).pValid, len1);		//第一段 
		memcpy( buffer+len1, (*p_rb).pHead, len2);//第二段，绕到整个存储区的开头 
		if(lock) portENTER_CRITICAL_SAFE(&p_rb->lock);
		(*p_rb).pValid = (*p_rb).pHead + len2;//更新已使用缓冲区的起始 
	}
	else 
	{ 
		memcpy( buffer, (*p_rb).pValid, len); 
		if(lock) portENTER_CRITICAL_SAFE(&p_rb->lock);
		(*p_rb).pValid = (*p_rb).pValid +len;//更新已使用缓冲区的起始 
	} 
	if(lock) portEXIT_CRITICAL_SAFE(&p_rb->lock);

	return len; 
} 


long copyRingbuffer( UCHAR* buffer,unsigned long len, struct s_ringbuffer * p_rb) 
{ 	
	if((*p_rb).pHead==NULL)
		return -1; 	
	// ASSERT(buffer); 
	unsigned long validLen = _getRingbufferValidLen(p_rb, false);

	if(validLen == 0) 
		return 0; 

	if( len > validLen)
		len = validLen;
	if((*p_rb).pValid + len > (*p_rb).pTail)//需要分成两段copy 
	{ 
		long len1 = (*p_rb).pTail - (*p_rb).pValid; 
		long len2 = len - len1; 
		memcpy( buffer, (*p_rb).pValid, len1);		//第一段 
		memcpy( buffer+len1, (*p_rb).pHead, len2);//第二段，绕到整个存储区的开头 
	}
	else 
	{ 
		memcpy( buffer, (*p_rb).pValid, len); 
	} 
	return len; 
} 


long readRingbuffer( UCHAR* buffer,unsigned long len, struct s_ringbuffer * p_rb)
{
	return _readRingbuffer(buffer, len, p_rb, true);
}

long readRingbufferNoLock( UCHAR* buffer,unsigned long len, struct s_ringbuffer * p_rb)
{
	return _readRingbuffer(buffer, len, p_rb, false);
}
/**************************************************************
 * function:获取已使用缓冲区的长度
 * return  :已使用的buffer长度
 ***************************************************************/ 
static unsigned long _getRingbufferValidLen( struct s_ringbuffer * p_rb, bool lock)
{
	if(lock) portENTER_CRITICAL_SAFE(&p_rb->lock);
	long len = p_rb->pValidTail - p_rb->pValid;
	if(len < 0) 
		len = len + p_rb->bufferSize;
	if(lock) portEXIT_CRITICAL_SAFE(&p_rb->lock);

    return len;
}

unsigned long getRingbufferValidLenNoLock( struct s_ringbuffer * p_rb )
{
	return _getRingbufferValidLen(p_rb, false);
}

unsigned long getRingbufferValidLen( struct s_ringbuffer * p_rb )
{
	return _getRingbufferValidLen(p_rb, true);
}



static unsigned long _getRingbufferFreeLen( struct s_ringbuffer * p_rb, bool lock)
{
    return (RING_BUFFER_SIZE - _getRingbufferValidLen(p_rb, lock));
}

unsigned long getRingbufferFreeLenNoLock( struct s_ringbuffer * p_rb)
{
	return _getRingbufferFreeLen(p_rb, false);
}

unsigned long getRingbufferFreeLen( struct s_ringbuffer * p_rb)
{
	return _getRingbufferFreeLen(p_rb, true);
}

/**************************************************************
 * function:释放环形缓冲区
 ***************************************************************/ 
void _releaseRingbuffer( struct s_ringbuffer * p_rb, bool lock) 
{ 
	if(lock) portENTER_CRITICAL_SAFE(&p_rb->lock);
	if((*p_rb).pHead!=NULL) 
		free((*p_rb).pHead); 
	(*p_rb).pHead = NULL; 
	if(lock) portEXIT_CRITICAL_SAFE(&p_rb->lock);
}

void releaseRingbufferNoLock( struct s_ringbuffer * p_rb)
{
	_releaseRingbuffer(p_rb, false);
}
void releaseRingbuffer( struct s_ringbuffer * p_rb)
{
	_releaseRingbuffer(p_rb, true);
}
