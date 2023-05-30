/****************************************************************
 *                                                              *
 * @copyright  Copyright (c) 2020 SDR-Technologies SAS          *
 * @author     Sylvain AZARIAN - s.azarian@sdr-technologies.fr  *
 * @project    SDR Virtual Machine                              *
 *                                                              *
 * Code propriete exclusive de la société SDR-Technologies SAS  *
 *                                                              *
 ****************************************************************/

#ifndef IQACCUMULATOR_H
#define IQACCUMULATOR_H

#include <mutex>
#include <vector>
#include "vmtypes.h"

class IQAccumulator
{
public:
    IQAccumulator();
    ~IQAccumulator();
    int maxRead();
    void append(TYPECPX *data, int length);
    int get(int length, TYPECPX *destination);

    int getReal(int length, std::vector<float> *destination );

private:
    std::mutex mtx ;
    int mBufferLength ;
    int mWritePos ;
    int mReadPos ;
    TYPECPX *_buffer ;
};

#endif // IQACCUMULATOR_H
