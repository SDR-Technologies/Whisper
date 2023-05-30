#ifndef VMWISPERINTERFACE_H
#define VMWISPERINTERFACE_H

#include <thread>
#include "vmplugins.h"
#include "vmtypes.h"
#include "vmtoolbox.h"
#include "whisper++/whisper.h"
#include "iqaccumulator.h"

struct whisper_params {
    int32_t n_threads  = std::max(4, (int32_t) std::thread::hardware_concurrency()-2);
    int32_t n_processors =  1;
    int32_t step_ms    = 3000;
    int32_t length_ms  = 10000;
    int32_t keep_ms    = 200;
    int32_t capture_id = -1;
    int32_t max_tokens = 32;
    int32_t audio_ctx  = 0;

    float vad_thold    = 0.6f;
    float freq_thold   = 100.0f;

    bool speed_up      = false;
    bool translate     = false;
    bool print_special = false;
    bool no_context    = true;
    bool no_timestamps = false;

    std::string language  = "en";
    std::string model     = "models/ggml-base.en.bin";
    std::string fname_out;
};

typedef struct tp {
    bool stop = false ;
    bool running = false ;
    ConsumerProducerQueue<CpxBlock *> *in_queue ;
    whisper_params *params;
    TMBox* mbox ;
} ThreadParams ;


class VMWisperInterface : public IJSClass
{
public:
    VMWisperInterface() = default ;


    const char* Name() ;
    const char* JSTypeName() ;
    VMWisperInterface* allocNewInstance(ISDRVirtualMachineEnv *host) ;
    void deleteInstance( IJSClass *instance ) ;
    void declareMethods( ISDRVirtualMachineEnv *host ) ;

    void init();
    void releaseResources();

    //
    bool configure(const char *modelfile, const char *language, const char *mboxname);
    bool process( CpxBlock *b );

private:
    whisper_params wconfig;
    CpxSampleQueue* input ;
    std::thread *encoder ;
    ThreadParams threadParams ;
    TMBox* mbox ;
};

#endif // VMWISPERINTERFACE_H
