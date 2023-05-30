/****************************************************************
 *                                                              *
 * @copyright  Copyright (c) 2020 SDR-Technologies SAS          *
 * @author     Sylvain AZARIAN - s.azarian@sdr-technologies.fr  *
 * @project    SDR Virtual Machine                              *
 *                                                              *
 * Code propriete exclusive de la société SDR-Technologies SAS  *
 *                                                              *
 ****************************************************************/

#include <string.h>
#include "iqaccumulator.h"

IQAccumulator::IQAccumulator()
{
    mBufferLength = 0 ;
    mWritePos = 0 ;
    mReadPos = 0 ;
    _buffer = NULL ;

}

IQAccumulator::~IQAccumulator() {
    if( _buffer != NULL )
        cpxfree(_buffer);
}

int IQAccumulator::maxRead() {
    return( mWritePos-mReadPos);
}

void IQAccumulator::append(TYPECPX *data, int length) {
    mtx.lock();
    if( mBufferLength == 0 ) {
        mBufferLength = 4*length ;
        _buffer = cpxalloc( mBufferLength );
        memcpy( _buffer, data, length*sizeof(TYPECPX));
        mWritePos = length ;
        mReadPos = 0 ;
        mtx.unlock();
        return ;
    }
    int freespace = mBufferLength - mWritePos ;
    if( freespace < length ) {
        // too small realloc
        mBufferLength += 2*length ;
        TYPECPX *nbuffer = cpxalloc( mBufferLength );
        memcpy( nbuffer, _buffer+mReadPos, (mWritePos-mReadPos)*sizeof(TYPECPX)) ;
        mWritePos -= mReadPos ;
        mReadPos = 0 ;
        cpxfree(_buffer);
        _buffer = nbuffer ;
    }
    memcpy( _buffer+mWritePos, data, length*sizeof(TYPECPX));
    mWritePos += length ;
    mtx.unlock();
}


int IQAccumulator::get(int length, TYPECPX *destination) {
    mtx.lock();
    int content_size = maxRead();
    if( length > content_size ) {
        mtx.unlock();
        return(0);
    }
    memcpy( destination, _buffer+mReadPos, length*sizeof(TYPECPX)) ;
    mReadPos += length ;
    memmove( _buffer, _buffer+mReadPos, (mWritePos-mReadPos)*sizeof(TYPECPX)) ;
    mWritePos -= mReadPos ;
    mReadPos = 0 ;
    mtx.unlock();

    return(length);
}


int IQAccumulator::getReal(int length, std::vector<float> *destination) {
    mtx.lock();
    int content_size = maxRead();
    if( length > content_size ) {
        mtx.unlock();
        return(0);
    }
    TYPECPX *tab = _buffer + mReadPos ;
    for( int i=0 ; i < length ; i++ ) {
         TYPECPX x = tab[i];
         destination->push_back( x.I );
    }
    mReadPos += length ;
    memmove( _buffer, _buffer+mReadPos, (mWritePos-mReadPos)*sizeof(TYPECPX)) ;
    mWritePos -= mReadPos ;
    mReadPos = 0 ;
    mtx.unlock();

    return(length);
}
