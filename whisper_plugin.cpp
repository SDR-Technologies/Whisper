/****************************************************************
 *                                                              *
 * @copyright  Copyright (c) 2020 SDR-Technologies SAS          *
 * @author     Sylvain AZARIAN - s.azarian@sdr-technologies.fr  *
 * @project    SDR Virtual Machine                              *
 *                                                              *
 * Code propriete exclusive de la société SDR-Technologies SAS  *
 *                                                              *
 ****************************************************************/

#include <iostream>

#include "plugin_factory.h"
#include "vmsystem.h"
#include "vmtoolbox.h"
#include "vmwisperinterface.h"

StackTools *vmtools = nullptr ;


SDK_PLUGIN_EXPORT_C
auto GetPluginFactory() -> IPluginFactory*
{

    static PluginFactory pinfo = [] {
        auto p = PluginFactory("SDRVM Extensions", "0.1-alpha");
        p.registerClass<VMWisperInterface>("Whisper");
        return p;
    }();
    return &pinfo;
}


SDK_PLUGIN_EXPORT_C
void setTools( StackTools *tools ) {
    vmtools = tools ;
}

struct _DLLInit{
    _DLLInit(){
        vmtools = nullptr ;

        std::cerr << " [TRACE] Whisper plugin loaded OK." << std::endl;
    }
    ~_DLLInit(){
        std::cerr << " [TRACE] Whisper plugin unloaded." << std::endl;
    }

} dll_init;
