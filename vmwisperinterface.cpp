#include "vmwisperinterface.h"
#include <thread>
#include <string.h>


const char JSTypeNameStr[] = "Whisper" ;

const char* VMWisperInterface::Name() {
    return JSTypeNameStr;
}

const char* VMWisperInterface::JSTypeName() {
    return( (const char*) JSTypeNameStr );
}


// This is called when a new instance is needed for the JS code
// example : var x = new Log();
// make init here
VMWisperInterface* VMWisperInterface::allocNewInstance(ISDRVirtualMachineEnv *host) {
    (void)host ;
    VMWisperInterface* result = new VMWisperInterface();
    result->init();
    return( result );
}

void VMWisperInterface::init() {

    input = new CpxSampleQueue(10);
    encoder = nullptr ;
    threadParams.stop = false ;
    threadParams.in_queue = input ;
    threadParams.params  = &wconfig ;
    threadParams.mbox   = nullptr ;

    wconfig.keep_ms   = std::min(wconfig.keep_ms,   wconfig.step_ms);
    wconfig.length_ms = std::max(wconfig.length_ms, wconfig.step_ms);
    mbox = nullptr ;
}

// This is called when the allocated instance is no longer required
void VMWisperInterface::deleteInstance(IJSClass *instance) {

    delete instance ;
}

void VMWisperInterface::releaseResources() {
    CpxBlock *b ;
    if( input != nullptr ) {
        while( !input->isEmpty() ) {
            input->consume(b);
            if( b->data != nullptr )
                free(b->data);
            free(b);
        }
    }

}

void whisper_thread( ThreadParams *tparams ) ;
bool VMWisperInterface::configure(const char *modelfile, const char *language, const char* mboxname) {

    if( threadParams.running ) {
        fprintf(stderr, "error: cannot reconfigure\n" );
        fflush(stderr);
        return( false );
    }

    wconfig.language = std::string( language );
    wconfig.model    = std::string( modelfile );

    if (whisper_lang_id(wconfig.language.c_str()) == -1) {
        fprintf(stderr, "error: unknown language '%s'\n", wconfig.language.c_str());
        fflush(stderr);
        return( false );
    }

    mbox = vmtools->getMBox((char *)mboxname);
    threadParams.mbox = mbox ;
    threadParams.stop = false ;
    encoder = new std::thread( whisper_thread, &threadParams );
    while( threadParams.running == false ) {
        if( threadParams.stop == true ) {
            if( encoder->joinable() )
                encoder->join();
            return(false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return( true );
}

bool VMWisperInterface::process(CpxBlock *b) {
    if( encoder == nullptr ) {
        return( false );
    }
    input->add( b );
    return( true );
}

int processData_method( void *stack ) ;
int configure_method( void *stack );

void VMWisperInterface::declareMethods( ISDRVirtualMachineEnv *host ) {
    host->addMethod( (const char *)"configure", configure_method, true);
    host->addMethod( (const char *)"processData", processData_method, true );
}

int configure_method(void *stack) {
    int n = vmtools->getStackSize( stack );
    if( n < 3 ) {
        vmtools->throwException( stack, (char *)"Missing argument(s) ! configure(path_to_model, language, mbox)");
        return(0);
    }
    const char *path     = vmtools->getString( stack, 0 );
    const char *language = vmtools->getString( stack, 1 );
    const char *boxname  = vmtools->getString( stack, 2 );

    VMWisperInterface* object = (VMWisperInterface *)vmtools->getObject(stack);
    if( object != nullptr ) {
        bool res = object->configure( path, language , boxname) ;
        vmtools->pushBool( stack, res );
        return(1);
    }
    vmtools->pushBool( stack, false );
    return(1);
}

int processData_method( void *stack ) {
    int n = vmtools->getStackSize( stack );
    if( n != 1 ) {
        vmtools->throwException( stack, (char *)"processData( IQ ) : IQ missing");
        return(0);
    }


    CpxBlock *b = vmtools->getIQData( stack, 0 );
    if( b->length == 0 ) {
        if( b->data != nullptr )
            free(b->data);
        if( b->json_attribute != nullptr )
            free(b->json_attribute);
        free(b);
        vmtools->pushBool( stack, true );
        return(1);
    }
    VMWisperInterface* object = (VMWisperInterface *)vmtools->getObject(stack);
    if( object != nullptr ) {
        bool res = object->process( b );
        vmtools->pushBool( stack, res );
        return(1);
    }
    vmtools->pushBool( stack, false );
    return(1);
}

//  500 -> 00:05.000
// 6000 -> 01:00.000
std::string to_timestamp(int64_t t) {
    int64_t sec = t/100;
    int64_t msec = t - sec*100;
    int64_t min = sec/60;
    sec = sec - min*60;

    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d.%03d", (int) min, (int) sec, (int) msec);

    return std::string(buf);
}


void whisper_thread( ThreadParams *tparams ) {
    CpxBlock *b ;
    struct whisper_params *params = tparams->params ;

    struct whisper_context * ctx = whisper_init_from_file(params->model.c_str());
    if( ctx == nullptr ) {
        tparams->stop = true ;
    }
    tparams->running = true ;
    const int n_samples_step = (1e-3*params->step_ms  )*WHISPER_SAMPLE_RATE;
    int n_iter = 0;

    const bool use_vad = n_samples_step <= 0; // sliding window mode uses VAD
    const int n_new_line = !use_vad ? std::max(1, params->length_ms / params->step_ms - 1) : 1; // number of steps to print new line

    params->no_timestamps  = !use_vad;
    params->no_context    |= use_vad;
    params->max_tokens     = 0;
    params->n_threads      = std::max(4, (int32_t) std::thread::hardware_concurrency()-2);


    std::vector<whisper_token> prompt_tokens;
    if (!whisper_is_multilingual(ctx)) {
        if (tparams->params->language != "en" || tparams->params->translate) {
            tparams->params->language = "en";
            tparams->params->translate = false;
            fprintf(stderr, "%s: WARNING: model is not multilingual, ignoring language and translation options\n", __func__);
        }
    }

    IQAccumulator acc ;
    TMBox* mbox = tparams->mbox ;
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    wparams.print_progress   = false;
    wparams.print_special    = params->print_special;
    wparams.print_realtime   = false;
    wparams.print_timestamps = true ; //!params->no_timestamps;
    wparams.translate        = params->translate;
    wparams.max_tokens       = params->max_tokens;
    wparams.language         = params->language.c_str();
    wparams.n_threads        = params->n_threads;

    wparams.audio_ctx        = params->audio_ctx;
    wparams.speed_up         = params->speed_up;


    // disable temperature fallback
    wparams.temperature_inc  = -1.0f;
    while( tparams->stop == false ) {
        fprintf( stdout, "\nwaiting for data\n"); fflush(stdout);
        tparams->in_queue->consume(b);
        if( b->length == 0 ) {
            if( b->data != nullptr )
                free(b->data);
            free(b);
            continue ;
        }
        acc.append( (TYPECPX *)b->data, b->length );
        free(b->data);
        free(b);

        while( acc.maxRead() >0 ) {

            std::vector<float> pcmf32 ;
            // gather the audio samples
            acc.getReal( acc.maxRead(), &pcmf32 );

            // run the inference
            {
                wparams.prompt_tokens    = params->no_context ? nullptr : prompt_tokens.data();
                wparams.prompt_n_tokens  = params->no_context ? 0       : prompt_tokens.size();

                fprintf( stdout, "injecting %d samples\n", (int)pcmf32.size()); fflush(stdout);
                if (whisper_full(ctx, wparams, pcmf32.data(), pcmf32.size()) != 0) {
                    fprintf(stderr, " failed to process audio\n" );
                    tparams->stop = true ;
                    continue ;
                }

                //fprintf( stdout, "processed.\n"); fflush(stdout);
                const int n_segments = whisper_full_n_segments(ctx);
                for (int i = 0; i < n_segments; ++i) {
                    const char * text = whisper_full_get_segment_text(ctx, i);
                    if( mbox == nullptr ) {
                        fprintf( stdout, "[%d-%d] %s\n", n_iter, i, text);
                        fflush(stdout);
                    } else {
                        size_t L = strlen(text);
                        if( L > 0 ) {
                            TMBoxMessage* msg = (TMBoxMessage *)malloc( sizeof(TMBoxMessage));
                            msg->fromTID = 0 ;
                            msg->memsize = L+1 ;
                            msg->payload = (char *)malloc( msg->memsize );
                            snprintf( msg->payload, L,"%s",text);
                            mbox->postMessage(msg);
                        }
                    }
                }

                ++n_iter;

                if (!use_vad && (n_iter % n_new_line) == 0) {
                    printf("\n");

                    // Add tokens of the last full length segment as the prompt
                    if (!params->no_context) {
                        prompt_tokens.clear();

                        const int n_segments = whisper_full_n_segments(ctx);
                        for (int i = 0; i < n_segments; ++i) {
                            const int token_count = whisper_full_n_tokens(ctx, i);
                            for (int j = 0; j < token_count; ++j) {
                                prompt_tokens.push_back(whisper_full_get_token_id(ctx, i, j));
                            }
                        }
                    }
                }
            }
        }

    }
}
