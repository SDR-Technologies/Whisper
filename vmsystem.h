/****************************************************************
 *                                                              *
 * @copyright  Copyright (c) 2020 SDR-Technologies SAS          *
 * @author     Sylvain AZARIAN - s.azarian@sdr-technologies.fr  *
 * @project    SDR Virtual Machine                              *
 *                                                              *
 * Code propriete exclusive de la société SDR-Technologies SAS  *
 *                                                              *
 ****************************************************************/


#ifndef VMSYSTEM_H
#define VMSYSTEM_H

#include <stddef.h>

typedef int (*js_c_function)(void *ctx);
class IJSClass ;

class ISDRVirtualMachineEnv {
public:
    virtual IJSClass* getInstance( const char *JSTypeName ) ;
    virtual void pushInstance( IJSClass *instance  );

    virtual void addMethod( const char *method_name, js_c_function method_call, bool args=false);
} ;


class FFTWHelper {
public:
    virtual void* planAlloc(int n, void* in, void* out, int direction ) = 0 ;
    virtual void planFree( void* plan ) = 0 ;
} ;

typedef struct {
    long fromTID ;
    size_t memsize ;
    char *payload ;
} TMBoxMessage ;



class TMBox {
public:
    virtual char *getMBoxName() = 0 ;
    virtual void postMessage( TMBoxMessage *m )= 0 ;
    virtual bool hasMessage() = 0 ;
    virtual TMBoxMessage *popTMessage() = 0 ;
} ;


#endif // VMSYSTEM_H
