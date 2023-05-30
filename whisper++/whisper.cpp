#define WHISPER_BUILD
#include "whisper.h"

#include "ggml.h"

#include <algorithm>
#include <cassert>
#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <regex>
#include <random>

#if defined(GGML_BIG_ENDIAN)
#include <bit>

template<typename T>
static T byteswap(T value) {
    return std::byteswap(value);
}

template<>
float byteswap(float value) {
    return std::bit_cast<float>(byteswap(std::bit_cast<std::uint32_t>(value)));
}

template<typename T>
static void byteswap_tensor_data(ggml_tensor * tensor) {
    T * datum = reinterpret_cast<T *>(tensor->data);
    for (int i = 0; i < ggml_nelements(tensor); i++) {
        datum[i] = byteswap(datum[i]);
    }
}

static void byteswap_tensor(ggml_tensor * tensor) {
    switch (tensor->type) {
        case GGML_TYPE_I16: {
            byteswap_tensor_data<int16_t>(tensor);
            break;
        }
        case GGML_TYPE_F16: {
            byteswap_tensor_data<ggml_fp16_t>(tensor);
            break;
        }
        case GGML_TYPE_I32: {
            byteswap_tensor_data<int32_t>(tensor);
            break;
        }
        case GGML_TYPE_F32: {
            byteswap_tensor_data<float>(tensor);
            break;
        }
        default: { // GML_TYPE_I8
            break;
        }
    }
}

#define BYTESWAP_VALUE(d) d = byteswap(d)
#define BYTESWAP_FILTERS(f)            \
    do {                              \
        for (auto & datum : f.data) { \
            datum = byteswap(datum);  \
        }                             \
    } while (0)
#define BYTESWAP_TENSOR(t)       \
    do {                         \
        byteswap_tensor(tensor); \
    } while (0)
#else
#define BYTESWAP_VALUE(d) do {} while (0)
#define BYTESWAP_FILTERS(f) do {} while (0)
#define BYTESWAP_TENSOR(t) do {} while (0)
#endif

#define WHISPER_ASSERT(x) \
    do { \
        if (!(x)) { \
            fprintf(stderr, "WHISPER_ASSERT: %s:%d: %s\n", __FILE__, __LINE__, #x); \
            abort(); \
        } \
    } while (0)

// define this to enable verbose trace logging - useful for debugging purposes
//#define WHISPER_DEBUG

#if defined(WHISPER_DEBUG)
#define WHISPER_PRINT_DEBUG(...) \
    do { \
        fprintf(stderr, __VA_ARGS__); \
    } while (0)
#else
#define WHISPER_PRINT_DEBUG(...)
#endif

#define WHISPER_USE_FLASH_ATTN
//#define WHISPER_USE_FLASH_FF
#define WHISPER_MAX_DECODERS 16

#define WHISPER_USE_SCRATCH
#define WHISPER_MAX_SCRATCH_BUFFERS 16

// available whisper models
enum e_model {
    MODEL_UNKNOWN,
    MODEL_TINY,
    MODEL_BASE,
    MODEL_SMALL,
    MODEL_MEDIUM,
    MODEL_LARGE,
};

static const std::map<std::string, std::pair<int, std::string>> g_lang = {
    { "en",  { 0,  "english",         } },
    { "zh",  { 1,  "chinese",         } },
    { "de",  { 2,  "german",          } },
    { "es",  { 3,  "spanish",         } },
    { "ru",  { 4,  "russian",         } },
    { "ko",  { 5,  "korean",          } },
    { "fr",  { 6,  "french",          } },
    { "ja",  { 7,  "japanese",        } },
    { "pt",  { 8,  "portuguese",      } },
    { "tr",  { 9,  "turkish",         } },
    { "pl",  { 10, "polish",          } },
    { "ca",  { 11,  "catalan",        } },
    { "nl",  { 12,  "dutch",          } },
    { "ar",  { 13,  "arabic",         } },
    { "sv",  { 14,  "swedish",        } },
    { "it",  { 15,  "italian",        } },
    { "id",  { 16,  "indonesian",     } },
    { "hi",  { 17,  "hindi",          } },
    { "fi",  { 18,  "finnish",        } },
    { "vi",  { 19,  "vietnamese",     } },
    { "iw",  { 20,  "hebrew",         } },
    { "uk",  { 21,  "ukrainian",      } },
    { "el",  { 22,  "greek",          } },
    { "ms",  { 23,  "malay",          } },
    { "cs",  { 24,  "czech",          } },
    { "ro",  { 25,  "romanian",       } },
    { "da",  { 26,  "danish",         } },
    { "hu",  { 27,  "hungarian",      } },
    { "ta",  { 28,  "tamil",          } },
    { "no",  { 29,  "norwegian",      } },
    { "th",  { 30,  "thai",           } },
    { "ur",  { 31,  "urdu",           } },
    { "hr",  { 32,  "croatian",       } },
    { "bg",  { 33,  "bulgarian",      } },
    { "lt",  { 34,  "lithuanian",     } },
    { "la",  { 35,  "latin",          } },
    { "mi",  { 36,  "maori",          } },
    { "ml",  { 37,  "malayalam",      } },
    { "cy",  { 38,  "welsh",          } },
    { "sk",  { 39,  "slovak",         } },
    { "te",  { 40,  "telugu",         } },
    { "fa",  { 41,  "persian",        } },
    { "lv",  { 42,  "latvian",        } },
    { "bn",  { 43,  "bengali",        } },
    { "sr",  { 44,  "serbian",        } },
    { "az",  { 45,  "azerbaijani",    } },
    { "sl",  { 46,  "slovenian",      } },
    { "kn",  { 47,  "kannada",        } },
    { "et",  { 48,  "estonian",       } },
    { "mk",  { 49,  "macedonian",     } },
    { "br",  { 50,  "breton",         } },
    { "eu",  { 51,  "basque",         } },
    { "is",  { 52,  "icelandic",      } },
    { "hy",  { 53,  "armenian",       } },
    { "ne",  { 54,  "nepali",         } },
    { "mn",  { 55,  "mongolian",      } },
    { "bs",  { 56,  "bosnian",        } },
    { "kk",  { 57,  "kazakh",         } },
    { "sq",  { 58,  "albanian",       } },
    { "sw",  { 59,  "swahili",        } },
    { "gl",  { 60,  "galician",       } },
    { "mr",  { 61,  "marathi",        } },
    { "pa",  { 62,  "punjabi",        } },
    { "si",  { 63,  "sinhala",        } },
    { "km",  { 64,  "khmer",          } },
    { "sn",  { 65,  "shona",          } },
    { "yo",  { 66,  "yoruba",         } },
    { "so",  { 67,  "somali",         } },
    { "af",  { 68,  "afrikaans",      } },
    { "oc",  { 69,  "occitan",        } },
    { "ka",  { 70,  "georgian",       } },
    { "be",  { 71,  "belarusian",     } },
    { "tg",  { 72,  "tajik",          } },
    { "sd",  { 73,  "sindhi",         } },
    { "gu",  { 74,  "gujarati",       } },
    { "am",  { 75,  "amharic",        } },
    { "yi",  { 76,  "yiddish",        } },
    { "lo",  { 77,  "lao",            } },
    { "uz",  { 78,  "uzbek",          } },
    { "fo",  { 79,  "faroese",        } },
    { "ht",  { 80,  "haitian creole", } },
    { "ps",  { 81,  "pashto",         } },
    { "tk",  { 82,  "turkmen",        } },
    { "nn",  { 83,  "nynorsk",        } },
    { "mt",  { 84,  "maltese",        } },
    { "sa",  { 85,  "sanskrit",       } },
    { "lb",  { 86,  "luxembourgish",  } },
    { "my",  { 87,  "myanmar",        } },
    { "bo",  { 88,  "tibetan",        } },
    { "tl",  { 89,  "tagalog",        } },
    { "mg",  { 90,  "malagasy",       } },
    { "as",  { 91,  "assamese",       } },
    { "tt",  { 92,  "tatar",          } },
    { "haw", { 93,  "hawaiian",       } },
    { "ln",  { 94,  "lingala",        } },
    { "ha",  { 95,  "hausa",          } },
    { "ba",  { 96,  "bashkir",        } },
    { "jw",  { 97,  "javanese",       } },
    { "su",  { 98,  "sundanese",      } },
};

static const size_t MB = 1024*1024;

static const std::map<e_model, size_t> MEM_REQ_SCRATCH0 = {
    { MODEL_TINY,     12ull*MB },
    { MODEL_BASE,     15ull*MB },
    { MODEL_SMALL,    23ull*MB },
    { MODEL_MEDIUM,   31ull*MB },
    { MODEL_LARGE,    38ull*MB },
};

static const std::map<e_model, size_t> MEM_REQ_SCRATCH1 = {
    { MODEL_TINY,     18ull*MB },
    { MODEL_BASE,     24ull*MB },
    { MODEL_SMALL,    36ull*MB },
    { MODEL_MEDIUM,   48ull*MB },
    { MODEL_LARGE,    60ull*MB },
};

static const std::map<e_model, size_t> MEM_REQ_SCRATCH2 = {
    { MODEL_TINY,      4ull*MB },
    { MODEL_BASE,      4ull*MB },
    { MODEL_SMALL,     6ull*MB },
    { MODEL_MEDIUM,    7ull*MB },
    { MODEL_LARGE,     9ull*MB },
};

static const std::map<e_model, size_t> MEM_REQ_SCRATCH3 = {
    { MODEL_TINY,      4ull*MB },
    { MODEL_BASE,      4ull*MB },
    { MODEL_SMALL,     6ull*MB },
    { MODEL_MEDIUM,    7ull*MB },
    { MODEL_LARGE,     9ull*MB },
};

static const std::map<e_model, size_t> MEM_REQ_MODEL = {
    { MODEL_TINY,     74ull*MB },
    { MODEL_BASE,    142ull*MB },
    { MODEL_SMALL,   466ull*MB },
    { MODEL_MEDIUM, 1464ull*MB },
    { MODEL_LARGE,  2952ull*MB },
};

static const std::map<e_model, size_t> MEM_REQ_KV_SELF = {
    { MODEL_TINY,      3ull*MB },
    { MODEL_BASE,      6ull*MB },
    { MODEL_SMALL,    16ull*MB },
    { MODEL_MEDIUM,   43ull*MB },
    { MODEL_LARGE,    71ull*MB },
};

static const std::map<e_model, size_t> MEM_REQ_KV_CROSS = {
    { MODEL_TINY,      9ull*MB },
    { MODEL_BASE,     18ull*MB },
    { MODEL_SMALL,    53ull*MB },
    { MODEL_MEDIUM,  141ull*MB },
    { MODEL_LARGE,   235ull*MB },
};

static const std::map<e_model, size_t> MEM_REQ_ENCODE = {
    { MODEL_TINY,      6ull*MB },
    { MODEL_BASE,      8ull*MB },
    { MODEL_SMALL,    13ull*MB },
    { MODEL_MEDIUM,   22ull*MB },
    { MODEL_LARGE,    33ull*MB },
};

static const std::map<e_model, size_t> MEM_REQ_DECODE = {
    { MODEL_TINY,      3ull*MB },
    { MODEL_BASE,      5ull*MB },
    { MODEL_SMALL,    10ull*MB },
    { MODEL_MEDIUM,   18ull*MB },
    { MODEL_LARGE,    27ull*MB },
};

struct whisper_mel {
    int n_len;
    int n_mel;

    std::vector<float> data;
};

struct whisper_filters {
    int32_t n_mel;
    int32_t n_fft;

    std::vector<float> data;
};

struct whisper_vocab {
    using id    = int32_t;
    using token = std::string;

    int n_vocab = 51864;

    std::map<token, id> token_to_id;
    std::map<id, token> id_to_token;

    id token_eot  = 50256;
    id token_sot  = 50257;
    id token_prev = 50360;
    id token_solm = 50361; // ??
    id token_not  = 50362; // no timestamps
    id token_beg  = 50363;

    // available tasks
    static const id token_translate  = 50358;
    static const id token_transcribe = 50359;

    bool is_multilingual() const {
        return n_vocab == 51865;
    }
};

struct whisper_segment {
    int64_t t0;
    int64_t t1;

    std::string text;

    std::vector<whisper_token_data> tokens;
};

// medium
// hparams: {
// 'n_mels': 80,
// 'n_vocab': 51864,
// 'n_audio_ctx': 1500,
// 'n_audio_state': 1024,
// 'n_audio_head': 16,
// 'n_audio_layer': 24,
// 'n_text_ctx': 448,
// 'n_text_state': 1024,
// 'n_text_head': 16,
// 'n_text_layer': 24
// }
//
// default hparams (Whisper tiny)
struct whisper_hparams {
    int32_t n_vocab       = 51864;
    int32_t n_audio_ctx   = 1500;
    int32_t n_audio_state = 384;
    int32_t n_audio_head  = 6;
    int32_t n_audio_layer = 4;
    int32_t n_text_ctx    = 448;
    int32_t n_text_state  = 384;
    int32_t n_text_head   = 6;
    int32_t n_text_layer  = 4;
    int32_t n_mels        = 80;
    int32_t f16           = 1;
};

// audio encoding layer
struct whisper_layer_encoder {
    // encoder.blocks.*.attn_ln
    struct ggml_tensor * attn_ln_0_w;
    struct ggml_tensor * attn_ln_0_b;

    // encoder.blocks.*.attn.out
    struct ggml_tensor * attn_ln_1_w;
    struct ggml_tensor * attn_ln_1_b;

    // encoder.blocks.*.attn.query
    struct ggml_tensor * attn_q_w;
    struct ggml_tensor * attn_q_b;

    // encoder.blocks.*.attn.key
    struct ggml_tensor * attn_k_w;

    // encoder.blocks.*.attn.value
    struct ggml_tensor * attn_v_w;
    struct ggml_tensor * attn_v_b;

    // encoder.blocks.*.mlp_ln
    struct ggml_tensor * mlp_ln_w;
    struct ggml_tensor * mlp_ln_b;

    // encoder.blocks.*.mlp.0
    struct ggml_tensor * mlp_0_w;
    struct ggml_tensor * mlp_0_b;

    // encoder.blocks.*.mlp.2
    struct ggml_tensor * mlp_1_w;
    struct ggml_tensor * mlp_1_b;
};

// token decoding layer
struct whisper_layer_decoder {
    // decoder.blocks.*.attn_ln
    struct ggml_tensor * attn_ln_0_w;
    struct ggml_tensor * attn_ln_0_b;

    // decoder.blocks.*.attn.out
    struct ggml_tensor * attn_ln_1_w;
    struct ggml_tensor * attn_ln_1_b;

    // decoder.blocks.*.attn.query
    struct ggml_tensor * attn_q_w;
    struct ggml_tensor * attn_q_b;

    // decoder.blocks.*.attn.key
    struct ggml_tensor * attn_k_w;

    // decoder.blocks.*.attn.value
    struct ggml_tensor * attn_v_w;
    struct ggml_tensor * attn_v_b;

    // decoder.blocks.*.cross_attn_ln
    struct ggml_tensor * cross_attn_ln_0_w;
    struct ggml_tensor * cross_attn_ln_0_b;

    // decoder.blocks.*.cross_attn.out
    struct ggml_tensor * cross_attn_ln_1_w;
    struct ggml_tensor * cross_attn_ln_1_b;

    // decoder.blocks.*.cross_attn.query
    struct ggml_tensor * cross_attn_q_w;
    struct ggml_tensor * cross_attn_q_b;

    // decoder.blocks.*.cross_attn.key
    struct ggml_tensor * cross_attn_k_w;

    // decoder.blocks.*.cross_attn.value
    struct ggml_tensor * cross_attn_v_w;
    struct ggml_tensor * cross_attn_v_b;

    // decoder.blocks.*.mlp_ln
    struct ggml_tensor * mlp_ln_w;
    struct ggml_tensor * mlp_ln_b;

    // decoder.blocks.*.mlp.0
    struct ggml_tensor * mlp_0_w;
    struct ggml_tensor * mlp_0_b;

    // decoder.blocks.*.mlp.2
    struct ggml_tensor * mlp_1_w;
    struct ggml_tensor * mlp_1_b;
};

struct whisper_kv_cache {
    struct ggml_tensor * k;
    struct ggml_tensor * v;

    struct ggml_context * ctx;

    std::vector<uint8_t> buf;

    int n; // number of tokens currently in the cache
};

struct whisper_model {
    e_model type = MODEL_UNKNOWN;

    whisper_hparams hparams;
    whisper_filters filters;

    // encoder.positional_embedding
    struct ggml_tensor * e_pe;

    // encoder.conv1
    struct ggml_tensor * e_conv_1_w;
    struct ggml_tensor * e_conv_1_b;

    // encoder.conv2
    struct ggml_tensor * e_conv_2_w;
    struct ggml_tensor * e_conv_2_b;

    // encoder.ln_post
    struct ggml_tensor * e_ln_w;
    struct ggml_tensor * e_ln_b;

    // decoder.positional_embedding
    struct ggml_tensor * d_pe;

    // decoder.token_embedding
    struct ggml_tensor * d_te;

    // decoder.ln
    struct ggml_tensor * d_ln_w;
    struct ggml_tensor * d_ln_b;

    std::vector<whisper_layer_encoder> layers_encoder;
    std::vector<whisper_layer_decoder> layers_decoder;

    // context
    struct ggml_context * ctx;

    // the model memory buffer is read-only and can be shared between processors
    std::vector<uint8_t> * buf;

    // tensors
    int n_loaded;
    std::map<std::string, struct ggml_tensor *> tensors;
};

struct whisper_sequence {
    std::vector<whisper_token_data> tokens;

    // the accumulated transcription in the current interation (used to truncate the tokens array)
    int result_len;

    double sum_logprobs_all; // the sum of the log probabilities of the tokens
    double sum_logprobs;     // the sum of the log probabilities of the tokens (first result_len tokens)
    double avg_logprobs;     // the average log probability of the tokens
    double entropy;          // the entropy of the tokens
    double score;            // likelihood rank score
};

// TAGS: WHISPER_DECODER_INIT
struct whisper_decoder {
    // each decoders keeps its own KV-cache
    whisper_kv_cache kv_self;

    // the currently generated sequence of tokens
    whisper_sequence sequence;

    int seek_delta; // the window shift found so far based on the decoded timestamp tokens

    bool failed;    // has the current segment failed to decode?
    bool completed; // has the decoder completed the current segment?
    bool has_ts;    // have we already sampled a non-beg timestamp token for the current segment?

    // new token probs, logits and logprobs after the last whisper_decode (1-dimensional array: [n_vocab])
    std::vector<float> probs;
    std::vector<float> logits;
    std::vector<float> logprobs;

    std::vector<whisper_token> tokens_tmp; // used for whisper_decode calls
};

struct whisper_context {
    int64_t t_load_us   = 0;
    int64_t t_mel_us    = 0;
    int64_t t_sample_us = 0;
    int64_t t_encode_us = 0;
    int64_t t_decode_us = 0;
    int64_t t_start_us  = 0;

    int32_t n_sample = 0; // number of tokens sampled
    int32_t n_encode = 0; // number of encoder calls
    int32_t n_decode = 0; // number of decoder calls
    int32_t n_fail_p = 0; // number of logprob threshold failures
    int32_t n_fail_h = 0; // number of entropy threshold failures

    ggml_type wtype; // weight type (FP32 or FP16)

    whisper_mel mel;

    whisper_model model;
    whisper_vocab vocab;

    // cross-attention KV cache for the decoders
    // shared between all decoders
    whisper_kv_cache kv_cross;

    whisper_decoder decoders[WHISPER_MAX_DECODERS] = {};

    // memory buffers used by encode / decode contexts
    std::vector<uint8_t> buf_compute;
    std::vector<uint8_t> buf_scratch[WHISPER_MAX_SCRATCH_BUFFERS];

    int    buf_last = 0;
    size_t buf_max_size[WHISPER_MAX_SCRATCH_BUFFERS] = { 0 };

    // decode output (2-dimensional array: [n_tokens][n_vocab])
    std::vector<float> logits;

    std::vector<whisper_segment> result_all;
    std::vector<whisper_token>   prompt_past;

    // work container used to avoid memory allocations
    std::vector<std::pair<double, whisper_vocab::id>> logits_id;

    mutable std::mt19937 rng; // used for sampling at t > 0.0

    int lang_id = 0; // english by default

    // [EXPERIMENTAL] token-level timestamps data
    int64_t t_beg = 0;
    int64_t t_last = 0;
    whisper_token tid_last;
    std::vector<float> energy; // PCM signal energy

    // [EXPERIMENTAL] speed-up techniques
    int32_t exp_n_audio_ctx = 0; // 0 - use default

    void use_buf(struct ggml_context * ctx, int i) {
#if defined(WHISPER_USE_SCRATCH)
        size_t last_size = 0;

        if (i == -1) {
            last_size = ggml_set_scratch(ctx, { 0, 0, nullptr, });
        } else {
            auto & buf = buf_scratch[i];
            last_size = ggml_set_scratch(ctx, { 0, buf.size(), buf.data(), });
        }

        if (buf_last >= 0) {
            buf_max_size[buf_last] = std::max(buf_max_size[buf_last], last_size);
        }

        buf_last = i;
#else
        (void) i;
        (void) ctx;
#endif
    }

    size_t get_buf_max_mem(int i) const {
#if defined(WHISPER_USE_SCRATCH)
        return buf_max_size[i];
#else
        (void) i;
        return 0;
#endif
    }
};

template<typename T>
static void read_safe(whisper_model_loader * loader, T & dest) {
    loader->read(loader->context, &dest, sizeof(T));
    BYTESWAP_VALUE(dest);
}

static bool kv_cache_init(
        const struct whisper_hparams & hparams,
                        const size_t   mem_bytes,
             struct whisper_kv_cache & cache,
                           ggml_type   wtype,
                                 int   n_ctx) {
    cache.buf.resize(mem_bytes);

    struct ggml_init_params params;
    params.mem_size   = cache.buf.size();
    params.mem_buffer = cache.buf.data();

    cache.ctx = ggml_init(params);

    if (!cache.ctx) {
        fprintf(stderr, "%s: failed to allocate memory for kv cache\n", __func__);
        return false;
    }

    const int n_text_state = hparams.n_text_state;
    const int n_text_layer = hparams.n_text_layer;

    const int n_mem      = n_text_layer*n_ctx;
    const int n_elements = n_text_state*n_mem;

    cache.k = ggml_new_tensor_1d(cache.ctx, wtype, n_elements);
    cache.v = ggml_new_tensor_1d(cache.ctx, wtype, n_elements);

    return true;
}

static bool kv_cache_reinit(struct whisper_kv_cache & cache) {
    WHISPER_ASSERT(cache.ctx);

    const int n_elements = ggml_nelements(cache.k);
    WHISPER_ASSERT(n_elements == ggml_nelements(cache.v));

    const ggml_type wtype = cache.k->type;
    WHISPER_ASSERT(wtype == cache.v->type);

    WHISPER_ASSERT(cache.buf.size() >= 2*n_elements*ggml_type_size(wtype));

    struct ggml_init_params params;
    params.mem_size   = cache.buf.size();
    params.mem_buffer = cache.buf.data();

    cache.ctx = ggml_init(params);

    if (!cache.ctx) {
        fprintf(stderr, "%s: failed to allocate memory for kv cache\n", __func__);
        return false;
    }

    cache.k = ggml_new_tensor_1d(cache.ctx, wtype, n_elements);
    cache.v = ggml_new_tensor_1d(cache.ctx, wtype, n_elements);

    return true;
}

static void kv_cache_free(struct whisper_kv_cache & cache) {
    if (cache.ctx) {
        ggml_free(cache.ctx);
        cache.ctx = nullptr;
    }
}

// load the model from a ggml file
//
// file format:
//
//   - hparams
//   - pre-computed mel filters
//   - vocab
//   - weights
//
// see the convert-pt-to-ggml.py script for details
//
static bool whisper_model_load(struct whisper_model_loader * loader, whisper_context & wctx) {
#ifdef __WHISPER_TRACE__
    fprintf(stderr, "%s: loading model\n", __func__);
#endif
    const int64_t t_start_us = ggml_time_us();

    wctx.t_start_us = t_start_us;

    auto & model = wctx.model;
    auto & vocab = wctx.vocab;

    // verify magic
    {
        uint32_t magic;
        read_safe(loader, magic);
        if (magic != 0x67676d6c) {
            fprintf(stderr, "%s: invalid model data (bad magic)\n", __func__);
            return false;
        }
    }

    //load hparams
    {
        auto & hparams = model.hparams;

        read_safe(loader, hparams.n_vocab);
        read_safe(loader, hparams.n_audio_ctx);
        read_safe(loader, hparams.n_audio_state);
        read_safe(loader, hparams.n_audio_head);
        read_safe(loader, hparams.n_audio_layer);
        read_safe(loader, hparams.n_text_ctx);
        read_safe(loader, hparams.n_text_state);
        read_safe(loader, hparams.n_text_head);
        read_safe(loader, hparams.n_text_layer);
        read_safe(loader, hparams.n_mels);
        read_safe(loader, hparams.f16);

        assert(hparams.n_text_state == hparams.n_audio_state);

        if (hparams.n_audio_layer == 4) {
            model.type = e_model::MODEL_TINY;
        }

        if (hparams.n_audio_layer == 6) {
            model.type = e_model::MODEL_BASE;
        }

        if (hparams.n_audio_layer == 12) {
            model.type = e_model::MODEL_SMALL;
        }

        if (hparams.n_audio_layer == 24) {
            model.type = e_model::MODEL_MEDIUM;
        }

        if (hparams.n_audio_layer == 32) {
            model.type = e_model::MODEL_LARGE;
        }

        // for the big tensors, we have the option to store the data in 16-bit floats
        // in order to save memory and also to speed up the computation
        wctx.wtype = model.hparams.f16 ? GGML_TYPE_F16 : GGML_TYPE_F32;

        const size_t scale = model.hparams.f16 ? 1 : 2;
#ifdef __WHISPER_TRACE__
        fprintf(stderr, "%s: n_vocab       = %d\n", __func__, hparams.n_vocab);
        fprintf(stderr, "%s: n_audio_ctx   = %d\n", __func__, hparams.n_audio_ctx);
        fprintf(stderr, "%s: n_audio_state = %d\n", __func__, hparams.n_audio_state);
        fprintf(stderr, "%s: n_audio_head  = %d\n", __func__, hparams.n_audio_head);
        fprintf(stderr, "%s: n_audio_layer = %d\n", __func__, hparams.n_audio_layer);
        fprintf(stderr, "%s: n_text_ctx    = %d\n", __func__, hparams.n_text_ctx);
        fprintf(stderr, "%s: n_text_state  = %d\n", __func__, hparams.n_text_state);
        fprintf(stderr, "%s: n_text_head   = %d\n", __func__, hparams.n_text_head);
        fprintf(stderr, "%s: n_text_layer  = %d\n", __func__, hparams.n_text_layer);
        fprintf(stderr, "%s: n_mels        = %d\n", __func__, hparams.n_mels);
        fprintf(stderr, "%s: f16           = %d\n", __func__, hparams.f16);
        fprintf(stderr, "%s: type          = %d\n", __func__, model.type);

        // print memory requirements
        {
            // this is the total memory required to run the inference
            const size_t mem_required =
                     MEM_REQ_SCRATCH0.at (model.type) +
                     MEM_REQ_SCRATCH1.at (model.type) +
                     MEM_REQ_SCRATCH2.at (model.type) +
                     MEM_REQ_SCRATCH3.at (model.type) +
                scale*MEM_REQ_MODEL.at   (model.type) +
                scale*MEM_REQ_KV_CROSS.at(model.type) +
                scale*std::max(MEM_REQ_ENCODE.at(model.type), MEM_REQ_DECODE.at(model.type));

            // this is the memory required by one decoder
            const size_t mem_required_decoder =
                scale*MEM_REQ_KV_SELF.at(model.type);

            fprintf(stderr, "%s: mem required  = %7.2f MB (+ %7.2f MB per decoder)\n", __func__,
                    mem_required / 1024.0 / 1024.0, mem_required_decoder / 1024.0 / 1024.0);          
        }
#endif
        // initialize all memory buffers
        // always have at least one decoder

        wctx.model.buf = new std::vector<uint8_t>();
        wctx.model.buf->resize(scale*MEM_REQ_MODEL.at(model.type));

        if (!kv_cache_init(model.hparams, scale*MEM_REQ_KV_SELF.at(model.type), wctx.decoders[0].kv_self, wctx.wtype, model.hparams.n_text_ctx)) {
            fprintf(stderr, "%s: kv_cache_init() failed for self-attention cache\n", __func__);
            return false;
        }
#ifdef __WHISPER_TRACE__
        {
            const size_t memory_size = ggml_nbytes(wctx.decoders[0].kv_self.k) + ggml_nbytes(wctx.decoders[0].kv_self.v);
            fprintf(stderr, "%s: kv self size  = %7.2f MB\n", __func__, memory_size/1024.0/1024.0);
        }
#endif
        if (!kv_cache_init(model.hparams, scale*MEM_REQ_KV_CROSS.at(model.type), wctx.kv_cross, wctx.wtype, model.hparams.n_audio_ctx)) {
            fprintf(stderr, "%s: kv_cache_init() failed for cross-attention cache\n", __func__);
            return false;
        }
#ifdef __WHISPER_TRACE__
        {
            const size_t memory_size = ggml_nbytes(wctx.kv_cross.k) + ggml_nbytes(wctx.kv_cross.v);
            fprintf(stderr, "%s: kv cross size = %7.2f MB\n", __func__, memory_size/1024.0/1024.0);
        }
#endif
        wctx.buf_compute.resize(scale*std::max(MEM_REQ_ENCODE.at(model.type), MEM_REQ_DECODE.at(model.type)));

        wctx.buf_scratch[0].resize(MEM_REQ_SCRATCH0.at(model.type));
        wctx.buf_scratch[1].resize(MEM_REQ_SCRATCH1.at(model.type));
        wctx.buf_scratch[2].resize(MEM_REQ_SCRATCH2.at(model.type));
        wctx.buf_scratch[3].resize(MEM_REQ_SCRATCH3.at(model.type));
    }

    // load mel filters
    {
        auto & filters = wctx.model.filters;

        read_safe(loader, filters.n_mel);
        read_safe(loader, filters.n_fft);

        filters.data.resize(filters.n_mel * filters.n_fft);
        loader->read(loader->context, filters.data.data(), filters.data.size() * sizeof(float));
        BYTESWAP_FILTERS(filters);
    }

    // load vocab
    {
        int32_t n_vocab = 0;
        read_safe(loader, n_vocab);

        //if (n_vocab != model.hparams.n_vocab) {
        //    fprintf(stderr, "%s: invalid model file '%s' (bad vocab size %d != %d)\n",
        //            __func__, fname.c_str(), n_vocab, model.hparams.n_vocab);
        //    return false;
        //}

        std::string word;
        std::vector<char> tmp;

        tmp.reserve(128);

        for (int i = 0; i < n_vocab; i++) {
            uint32_t len;
            read_safe(loader, len);

            if (len > 0) {
                tmp.resize(len);
                loader->read(loader->context, &tmp[0], tmp.size()); // read to buffer
                word.assign(&tmp[0], tmp.size());
            } else {
                // seems like we have an empty-string token in multi-language models (i = 50256)
                //fprintf(stderr, "%s: warning: empty-string token in vocab, i = %d\n", __func__, i);
                word = "";
            }

            vocab.token_to_id[word] = i;
            vocab.id_to_token[i] = word;

            //printf("%s: vocab[%d] = '%s'\n", __func__, i, word.c_str());
        }

        vocab.n_vocab = model.hparams.n_vocab;
        if (vocab.is_multilingual()) {
            vocab.token_eot++;
            vocab.token_sot++;
            vocab.token_prev++;
            vocab.token_solm++;
            vocab.token_not++;
            vocab.token_beg++;
        }

        if (n_vocab < model.hparams.n_vocab) {
            //fprintf(stderr, "%s: adding %d extra tokens\n", __func__, model.hparams.n_vocab - n_vocab);
            for (int i = n_vocab; i < model.hparams.n_vocab; i++) {
                if (i > vocab.token_beg) {
                    word = "[_TT_" + std::to_string(i - vocab.token_beg) + "]";
                } else if (i == vocab.token_eot) {
                    word = "[_EOT_]";
                } else if (i == vocab.token_sot) {
                    word = "[_SOT_]";
                } else if (i == vocab.token_prev) {
                    word = "[_PREV_]";
                } else if (i == vocab.token_not) {
                    word = "[_NOT_]";
                } else if (i == vocab.token_beg) {
                    word = "[_BEG_]";
                } else {
                    word = "[_extra_token_" + std::to_string(i) + "]";
                }
                vocab.token_to_id[word] = i;
                vocab.id_to_token[i] = word;
            }
        }

        wctx.logits.reserve(vocab.n_vocab*model.hparams.n_text_ctx);

        wctx.logits_id.reserve(n_vocab);

        // TAGS: WHISPER_DECODER_INIT
        wctx.decoders[0].sequence.tokens.reserve(model.hparams.n_text_ctx);

        wctx.decoders[0].probs.reserve   (vocab.n_vocab);
        wctx.decoders[0].logits.reserve  (vocab.n_vocab);
        wctx.decoders[0].logprobs.reserve(vocab.n_vocab);
    }

    size_t ctx_size = 0;

    const ggml_type wtype = wctx.wtype;

    {
        const auto & hparams = model.hparams;

        const int n_vocab = hparams.n_vocab;

        const int n_audio_ctx   = hparams.n_audio_ctx;
        const int n_audio_state = hparams.n_audio_state;
        const int n_audio_layer = hparams.n_audio_layer;

        const int n_text_ctx   = hparams.n_text_ctx;
        const int n_text_state = hparams.n_text_state;
        const int n_text_layer = hparams.n_text_layer;

        const int n_mels = hparams.n_mels;

        // encoder
        {
            ctx_size += n_audio_ctx*n_audio_state*ggml_type_size(GGML_TYPE_F32); // e_pe;

            ctx_size += 3*n_mels*n_audio_state*ggml_type_size(wtype);         // e_conv_1_w
            ctx_size +=          n_audio_state*ggml_type_size(GGML_TYPE_F32); // e_conv_1_b

            ctx_size += 3*n_audio_state*n_audio_state*ggml_type_size(wtype);         // e_conv_2_w
            ctx_size +=                 n_audio_state*ggml_type_size(GGML_TYPE_F32); // e_conv_2_b

            ctx_size += n_audio_state*ggml_type_size(GGML_TYPE_F32); // e_ln_w;
            ctx_size += n_audio_state*ggml_type_size(GGML_TYPE_F32); // e_ln_b;
        }

        // decoder
        {
            ctx_size += n_text_ctx*n_text_state*ggml_type_size(GGML_TYPE_F32); // d_pe;

            ctx_size += n_vocab*n_text_state*ggml_type_size(wtype); // d_te;

            ctx_size += n_text_state*ggml_type_size(GGML_TYPE_F32); // d_ln_w;
            ctx_size += n_text_state*ggml_type_size(GGML_TYPE_F32); // d_ln_b;
        }

        // encoder layers
        {
            ctx_size += n_audio_layer*(n_audio_state*ggml_type_size(GGML_TYPE_F32)); // mlp_ln_w
            ctx_size += n_audio_layer*(n_audio_state*ggml_type_size(GGML_TYPE_F32)); // mlp_ln_b

            ctx_size += n_audio_layer*(4*n_audio_state*n_audio_state*ggml_type_size(wtype));         // mlp_0_w
            ctx_size += n_audio_layer*(              4*n_audio_state*ggml_type_size(GGML_TYPE_F32)); // mlp_0_b

            ctx_size += n_audio_layer*(4*n_audio_state*n_audio_state*ggml_type_size(wtype));         // mlp_1_w
            ctx_size += n_audio_layer*(                n_audio_state*ggml_type_size(GGML_TYPE_F32)); // mlp_1_b

            ctx_size += n_audio_layer*(n_audio_state*ggml_type_size(GGML_TYPE_F32)); // attn_ln_0_w
            ctx_size += n_audio_layer*(n_audio_state*ggml_type_size(GGML_TYPE_F32)); // attn_ln_0_b

            ctx_size += n_audio_layer*(n_audio_state*n_audio_state*ggml_type_size(wtype));         // attn_q_w
            ctx_size += n_audio_layer*(              n_audio_state*ggml_type_size(GGML_TYPE_F32)); // attn_q_b

            ctx_size += n_audio_layer*(n_audio_state*n_audio_state*ggml_type_size(wtype)); // attn_k_w

            ctx_size += n_audio_layer*(n_audio_state*n_audio_state*ggml_type_size(wtype));         // attn_v_w
            ctx_size += n_audio_layer*(              n_audio_state*ggml_type_size(GGML_TYPE_F32)); // attn_v_b

            ctx_size += n_audio_layer*(n_audio_state*n_audio_state*ggml_type_size(wtype));         // attn_ln_1_w
            ctx_size += n_audio_layer*(              n_audio_state*ggml_type_size(GGML_TYPE_F32)); // attn_ln_1_b
        }

        // decoder layers
        {
            ctx_size += n_text_layer*(n_text_state*ggml_type_size(GGML_TYPE_F32)); // mlp_ln_w
            ctx_size += n_text_layer*(n_text_state*ggml_type_size(GGML_TYPE_F32)); // mlp_ln_b

            ctx_size += n_text_layer*(4*n_text_state*n_text_state*ggml_type_size(wtype));         // mlp_0_w
            ctx_size += n_text_layer*(             4*n_text_state*ggml_type_size(GGML_TYPE_F32)); // mlp_0_b

            ctx_size += n_text_layer*(4*n_text_state*n_text_state*ggml_type_size(wtype));         // mlp_1_w
            ctx_size += n_text_layer*(               n_text_state*ggml_type_size(GGML_TYPE_F32)); // mlp_1_b

            ctx_size += n_text_layer*(n_text_state*ggml_type_size(GGML_TYPE_F32)); // attn_ln_0_w
            ctx_size += n_text_layer*(n_text_state*ggml_type_size(GGML_TYPE_F32)); // attn_ln_0_b

            ctx_size += n_text_layer*(n_text_state*n_text_state*ggml_type_size(wtype));         // attn_q_w
            ctx_size += n_text_layer*(             n_text_state*ggml_type_size(GGML_TYPE_F32)); // attn_q_b

            ctx_size += n_text_layer*(n_text_state*n_text_state*ggml_type_size(wtype)); // attn_k_w

            ctx_size += n_text_layer*(n_text_state*n_text_state*ggml_type_size(wtype));         // attn_v_w
            ctx_size += n_text_layer*(             n_text_state*ggml_type_size(GGML_TYPE_F32)); // attn_v_b

            ctx_size += n_text_layer*(n_text_state*n_text_state*ggml_type_size(wtype));         // attn_ln_1_w
            ctx_size += n_text_layer*(             n_text_state*ggml_type_size(GGML_TYPE_F32)); // attn_ln_1_b
                                                                                                //
            ctx_size += n_text_layer*(n_text_state*ggml_type_size(GGML_TYPE_F32)); // cross_attn_ln_0_w
            ctx_size += n_text_layer*(n_text_state*ggml_type_size(GGML_TYPE_F32)); // cross_attn_ln_0_b

            ctx_size += n_text_layer*(n_text_state*n_text_state*ggml_type_size(wtype));         // cross_attn_q_w
            ctx_size += n_text_layer*(             n_text_state*ggml_type_size(GGML_TYPE_F32)); // cross_attn_q_b

            ctx_size += n_text_layer*(n_text_state*n_text_state*ggml_type_size(wtype)); // cross_attn_k_w

            ctx_size += n_text_layer*(n_text_state*n_text_state*ggml_type_size(wtype));         // cross_attn_v_w
            ctx_size += n_text_layer*(             n_text_state*ggml_type_size(GGML_TYPE_F32)); // cross_attn_v_b

            ctx_size += n_text_layer*(n_text_state*n_text_state*ggml_type_size(wtype));         // cross_attn_ln_1_w
            ctx_size += n_text_layer*(             n_text_state*ggml_type_size(GGML_TYPE_F32)); // cross_attn_ln_1_b
        }

        ctx_size += (15 + 15*n_audio_layer + 24*n_text_layer)*256; // object overhead

        //fprintf(stderr, "%s: model ctx     = %7.2f MB\n", __func__, ctx_size/(1024.0*1024.0));
    }

    // create the ggml context
    {
        struct ggml_init_params params;
        params.mem_size   = wctx.model.buf->size();
        params.mem_buffer = wctx.model.buf->data();

        model.ctx = ggml_init(params);
        if (!model.ctx) {
            fprintf(stderr, "%s: ggml_init() failed\n", __func__);
            return false;
        }
    }

    // prepare memory for the weights
    {
        auto & ctx = model.ctx;

        const auto & hparams = model.hparams;

        const int n_vocab = hparams.n_vocab;

        const int n_audio_ctx   = hparams.n_audio_ctx;
        const int n_audio_state = hparams.n_audio_state;
        const int n_audio_layer = hparams.n_audio_layer;

        const int n_text_ctx   = hparams.n_text_ctx;
        const int n_text_state = hparams.n_text_state;
        const int n_text_layer = hparams.n_text_layer;

        const int n_mels = hparams.n_mels;

        model.layers_encoder.resize(n_audio_layer);
        model.layers_decoder.resize(n_text_layer);

        // encoder
        {
            model.e_pe = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, n_audio_state, n_audio_ctx);

            model.e_conv_1_w = ggml_new_tensor_3d(ctx, wtype,         3, n_mels, n_audio_state);
            model.e_conv_1_b = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, 1, n_audio_state);

            model.e_conv_2_w = ggml_new_tensor_3d(ctx, wtype,         3, n_audio_state, n_audio_state);
            model.e_conv_2_b = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, 1, n_audio_state);

            model.e_ln_w = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_audio_state);
            model.e_ln_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_audio_state);

            // map by name
            model.tensors["encoder.positional_embedding"] = model.e_pe;

            model.tensors["encoder.conv1.weight"] = model.e_conv_1_w;
            model.tensors["encoder.conv1.bias"]   = model.e_conv_1_b;

            model.tensors["encoder.conv2.weight"] = model.e_conv_2_w;
            model.tensors["encoder.conv2.bias"]   = model.e_conv_2_b;

            model.tensors["encoder.ln_post.weight"] = model.e_ln_w;
            model.tensors["encoder.ln_post.bias"]   = model.e_ln_b;

            for (int i = 0; i < n_audio_layer; ++i) {
                auto & layer = model.layers_encoder[i];

                layer.mlp_ln_w = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_audio_state);
                layer.mlp_ln_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_audio_state);

                layer.mlp_0_w = ggml_new_tensor_2d(ctx, wtype,           n_audio_state, 4*n_audio_state);
                layer.mlp_0_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 4*n_audio_state);

                layer.mlp_1_w = ggml_new_tensor_2d(ctx, wtype,         4*n_audio_state, n_audio_state);
                layer.mlp_1_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32,   n_audio_state);

                layer.attn_ln_0_w = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_audio_state);
                layer.attn_ln_0_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_audio_state);

                layer.attn_q_w = ggml_new_tensor_2d(ctx, wtype,         n_audio_state, n_audio_state);
                layer.attn_q_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_audio_state);

                layer.attn_k_w = ggml_new_tensor_2d(ctx, wtype,         n_audio_state, n_audio_state);

                layer.attn_v_w = ggml_new_tensor_2d(ctx, wtype,         n_audio_state, n_audio_state);
                layer.attn_v_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_audio_state);

                layer.attn_ln_1_w = ggml_new_tensor_2d(ctx, wtype,         n_audio_state, n_audio_state);
                layer.attn_ln_1_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_audio_state);

                // map by name
                model.tensors["encoder.blocks." + std::to_string(i) + ".mlp_ln.weight"] = layer.mlp_ln_w;
                model.tensors["encoder.blocks." + std::to_string(i) + ".mlp_ln.bias"]   = layer.mlp_ln_b;

                model.tensors["encoder.blocks." + std::to_string(i) + ".mlp.0.weight"] = layer.mlp_0_w;
                model.tensors["encoder.blocks." + std::to_string(i) + ".mlp.0.bias"]   = layer.mlp_0_b;

                model.tensors["encoder.blocks." + std::to_string(i) + ".mlp.2.weight"] = layer.mlp_1_w;
                model.tensors["encoder.blocks." + std::to_string(i) + ".mlp.2.bias"]   = layer.mlp_1_b;

                model.tensors["encoder.blocks." + std::to_string(i) + ".attn_ln.weight"] = layer.attn_ln_0_w;
                model.tensors["encoder.blocks." + std::to_string(i) + ".attn_ln.bias"]   = layer.attn_ln_0_b;

                model.tensors["encoder.blocks." + std::to_string(i) + ".attn.query.weight"] = layer.attn_q_w;
                model.tensors["encoder.blocks." + std::to_string(i) + ".attn.query.bias"]   = layer.attn_q_b;

                model.tensors["encoder.blocks." + std::to_string(i) + ".attn.key.weight"] = layer.attn_k_w;

                model.tensors["encoder.blocks." + std::to_string(i) + ".attn.value.weight"] = layer.attn_v_w;
                model.tensors["encoder.blocks." + std::to_string(i) + ".attn.value.bias"]   = layer.attn_v_b;

                model.tensors["encoder.blocks." + std::to_string(i) + ".attn.out.weight"] = layer.attn_ln_1_w;
                model.tensors["encoder.blocks." + std::to_string(i) + ".attn.out.bias"]   = layer.attn_ln_1_b;
            }
        }

        // decoder
        {
            model.d_pe = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, n_text_state, n_text_ctx);

            model.d_te = ggml_new_tensor_2d(ctx, wtype, n_text_state, n_vocab);

            model.d_ln_w = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_text_state);
            model.d_ln_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_text_state);

            // map by name
            model.tensors["decoder.positional_embedding"] = model.d_pe;

            model.tensors["decoder.token_embedding.weight"] = model.d_te;

            model.tensors["decoder.ln.weight"] = model.d_ln_w;
            model.tensors["decoder.ln.bias"]   = model.d_ln_b;

            for (int i = 0; i < n_text_layer; ++i) {
                auto & layer = model.layers_decoder[i];

                layer.mlp_ln_w = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_text_state);
                layer.mlp_ln_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_text_state);

                layer.mlp_0_w = ggml_new_tensor_2d(ctx, wtype,           n_text_state, 4*n_text_state);
                layer.mlp_0_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 4*n_text_state);

                layer.mlp_1_w = ggml_new_tensor_2d(ctx, wtype,         4*n_text_state, n_text_state);
                layer.mlp_1_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32,   n_text_state);

                layer.attn_ln_0_w = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_text_state);
                layer.attn_ln_0_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_text_state);

                layer.attn_q_w = ggml_new_tensor_2d(ctx, wtype,         n_text_state, n_text_state);
                layer.attn_q_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_text_state);

                layer.attn_k_w = ggml_new_tensor_2d(ctx, wtype,         n_text_state, n_text_state);

                layer.attn_v_w = ggml_new_tensor_2d(ctx, wtype,         n_text_state, n_text_state);
                layer.attn_v_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_text_state);

                layer.attn_ln_1_w = ggml_new_tensor_2d(ctx, wtype,         n_text_state, n_text_state);
                layer.attn_ln_1_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_text_state);

                layer.cross_attn_ln_0_w = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_text_state);
                layer.cross_attn_ln_0_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_text_state);

                layer.cross_attn_q_w = ggml_new_tensor_2d(ctx, wtype,         n_text_state, n_text_state);
                layer.cross_attn_q_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_text_state);

                layer.cross_attn_k_w = ggml_new_tensor_2d(ctx, wtype,         n_text_state, n_text_state);

                layer.cross_attn_v_w = ggml_new_tensor_2d(ctx, wtype,         n_text_state, n_text_state);
                layer.cross_attn_v_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_text_state);

                layer.cross_attn_ln_1_w = ggml_new_tensor_2d(ctx, wtype,         n_text_state, n_text_state);
                layer.cross_attn_ln_1_b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_text_state);

                // map by name
                model.tensors["decoder.blocks." + std::to_string(i) + ".mlp_ln.weight"] = layer.mlp_ln_w;
                model.tensors["decoder.blocks." + std::to_string(i) + ".mlp_ln.bias"]   = layer.mlp_ln_b;

                model.tensors["decoder.blocks." + std::to_string(i) + ".mlp.0.weight"] = layer.mlp_0_w;
                model.tensors["decoder.blocks." + std::to_string(i) + ".mlp.0.bias"]   = layer.mlp_0_b;

                model.tensors["decoder.blocks." + std::to_string(i) + ".mlp.2.weight"] = layer.mlp_1_w;
                model.tensors["decoder.blocks." + std::to_string(i) + ".mlp.2.bias"]   = layer.mlp_1_b;

                model.tensors["decoder.blocks." + std::to_string(i) + ".attn_ln.weight"] = layer.attn_ln_0_w;
                model.tensors["decoder.blocks." + std::to_string(i) + ".attn_ln.bias"]   = layer.attn_ln_0_b;

                model.tensors["decoder.blocks." + std::to_string(i) + ".attn.query.weight"] = layer.attn_q_w;
                model.tensors["decoder.blocks." + std::to_string(i) + ".attn.query.bias"]   = layer.attn_q_b;

                model.tensors["decoder.blocks." + std::to_string(i) + ".attn.key.weight"] = layer.attn_k_w;

                model.tensors["decoder.blocks." + std::to_string(i) + ".attn.value.weight"] = layer.attn_v_w;
                model.tensors["decoder.blocks." + std::to_string(i) + ".attn.value.bias"]   = layer.attn_v_b;

                model.tensors["decoder.blocks." + std::to_string(i) + ".attn.out.weight"] = layer.attn_ln_1_w;
                model.tensors["decoder.blocks." + std::to_string(i) + ".attn.out.bias"]   = layer.attn_ln_1_b;

                model.tensors["decoder.blocks." + std::to_string(i) + ".cross_attn_ln.weight"] = layer.cross_attn_ln_0_w;
                model.tensors["decoder.blocks." + std::to_string(i) + ".cross_attn_ln.bias"]   = layer.cross_attn_ln_0_b;

                model.tensors["decoder.blocks." + std::to_string(i) + ".cross_attn.query.weight"] = layer.cross_attn_q_w;
                model.tensors["decoder.blocks." + std::to_string(i) + ".cross_attn.query.bias"]   = layer.cross_attn_q_b;

                model.tensors["decoder.blocks." + std::to_string(i) + ".cross_attn.key.weight"] = layer.cross_attn_k_w;

                model.tensors["decoder.blocks." + std::to_string(i) + ".cross_attn.value.weight"] = layer.cross_attn_v_w;
                model.tensors["decoder.blocks." + std::to_string(i) + ".cross_attn.value.bias"]   = layer.cross_attn_v_b;

                model.tensors["decoder.blocks." + std::to_string(i) + ".cross_attn.out.weight"] = layer.cross_attn_ln_1_w;
                model.tensors["decoder.blocks." + std::to_string(i) + ".cross_attn.out.bias"]   = layer.cross_attn_ln_1_b;
            }
        }
    }

    // load weights
    {
        size_t total_size = 0;

        model.n_loaded = 0;

        while (true) {
            int32_t n_dims;
            int32_t length;
            int32_t ftype;

            read_safe(loader, n_dims);
            read_safe(loader, length);
            read_safe(loader, ftype);

            if (loader->eof(loader->context)) {
                break;
            }

            int32_t nelements = 1;
            int32_t ne[3] = { 1, 1, 1 };
            for (int i = 0; i < n_dims; ++i) {
                read_safe(loader, ne[i]);
                nelements *= ne[i];
            }

            std::string name;
            std::vector<char> tmp(length); // create a buffer
            loader->read(loader->context, &tmp[0], tmp.size()); // read to buffer
            name.assign(&tmp[0], tmp.size());

            if (model.tensors.find(name) == model.tensors.end()) {
                fprintf(stderr, "%s: unknown tensor '%s' in model file\n", __func__, name.data());
                return false;
            }

            auto tensor = model.tensors[name.data()];
            if (ggml_nelements(tensor) != nelements) {
                fprintf(stderr, "%s: tensor '%s' has wrong size in model file\n", __func__, name.data());
                return false;
            }

            if (tensor->ne[0] != ne[0] || tensor->ne[1] != ne[1] || tensor->ne[2] != ne[2]) {
                fprintf(stderr, "%s: tensor '%s' has wrong shape in model file: got [%d, %d, %d], expected [%d, %d, %d]\n",
                        __func__, name.data(), tensor->ne[0], tensor->ne[1], tensor->ne[2], ne[0], ne[1], ne[2]);
                return false;
            }

            const size_t bpe = (ftype == 0) ? sizeof(float) : sizeof(ggml_fp16_t);

            if (nelements*bpe != ggml_nbytes(tensor)) {
                fprintf(stderr, "%s: tensor '%s' has wrong size in model file: got %zu, expected %zu\n",
                        __func__, name.data(), ggml_nbytes(tensor), nelements*bpe);
                return false;
            }

            loader->read(loader->context, tensor->data, ggml_nbytes(tensor));
            BYTESWAP_TENSOR(tensor);

            //printf("%48s - [%5d, %5d, %5d], type = %6s, %6.2f MB\n", name.data(), ne[0], ne[1], ne[2], ftype == 0 ? "float" : "f16", ggml_nbytes(tensor)/1024.0/1024.0);
            total_size += ggml_nbytes(tensor);
            model.n_loaded++;
        }

        //fprintf(stderr, "%s: model size    = %7.2f MB\n", __func__, total_size/1024.0/1024.0);

        if (model.n_loaded == 0) {
            fprintf(stderr, "%s: WARN no tensors loaded from model file - assuming empty model for testing\n", __func__);
        } else if (model.n_loaded != (int) model.tensors.size()) {
            fprintf(stderr, "%s: ERROR not all tensors loaded from model file - expected %zu, got %d\n", __func__, model.tensors.size(), model.n_loaded);
            return false;
        }
    }

    wctx.rng = std::mt19937(0);

    wctx.t_load_us = ggml_time_us() - t_start_us;

    return true;
}

// evaluate the encoder
//
// given audio recording (more specifically, its log mel spectrogram), runs forward pass of the encoder
// part of the transformer model and returns the encoded features
//
//   - model:      the model
//   - n_threads:  number of threads to use
//   - mel_offset: offset in the mel spectrogram (i.e. audio offset)
//
static bool whisper_encode(
        whisper_context & wctx,
              const int   mel_offset,
              const int   n_threads) {
    const int64_t t_start_us = ggml_time_us();

    const auto & model   = wctx.model;
    const auto & mel_inp = wctx.mel;
    const auto & hparams = model.hparams;

    const int n_ctx   = wctx.exp_n_audio_ctx > 0 ? wctx.exp_n_audio_ctx : hparams.n_audio_ctx;
    const int n_state = hparams.n_audio_state;
    const int n_head  = hparams.n_audio_head;
    const int n_layer = hparams.n_audio_layer;

    const int n_mels = hparams.n_mels;
    assert(mel_inp.n_mel == n_mels);

    struct ggml_init_params params;
    params.mem_size   = wctx.buf_compute.size();
    params.mem_buffer = wctx.buf_compute.data();

    struct ggml_context * ctx0 = ggml_init(params);

    wctx.use_buf(ctx0, 0);

    struct ggml_tensor * mel = ggml_new_tensor_2d(ctx0, GGML_TYPE_F32, 2*n_ctx, n_mels);
    assert(mel->type == GGML_TYPE_F32);
    {
        float * dst = (float *) mel->data;
        memset(dst, 0, ggml_nbytes(mel));

        const int i0 = std::min(mel_offset, mel_inp.n_len);
        const int i1 = std::min(mel_offset + 2*n_ctx, mel_inp.n_len);

        for (int j = 0; j < mel_inp.n_mel; ++j) {
            for (int i = i0; i < i1; ++i) {
                dst[j*2*n_ctx + (i - i0)] = mel_inp.data[j*mel_inp.n_len + i];
            }
        }
    }

    struct ggml_tensor * cur;

    // convolution + gelu
    {
        wctx.use_buf(ctx0, 1);

        cur = ggml_conv_1d_1s(ctx0, model.e_conv_1_w, mel);
        cur = ggml_add(ctx0,
                ggml_repeat(ctx0,
                    model.e_conv_1_b,
                    cur),
                cur);

        cur = ggml_gelu(ctx0, cur);

        wctx.use_buf(ctx0, 0);

        cur = ggml_conv_1d_2s(ctx0, model.e_conv_2_w, cur);
        cur = ggml_add(ctx0,
                ggml_repeat(ctx0,
                    model.e_conv_2_b,
                    cur),
                cur);

        cur = ggml_gelu(ctx0, cur);
    }

    wctx.use_buf(ctx0, 3);

    // ===================================================================
    // NOTE: experimenting with partial evaluation of the encoder (ignore)
    //static int iter = -1;
    //const int n_iter = 1500/n_ctx;

    //iter = (iter + 1) % n_iter;

    //if (iter == 0) {
    //    memset(model.memory_cross_k->data, 0, ggml_nbytes(model.memory_cross_k));
    //    memset(model.memory_cross_v->data, 0, ggml_nbytes(model.memory_cross_v));
    //}

    static int iter = 0;

    const size_t e_pe_stride = model.e_pe->ne[0]*ggml_element_size(model.e_pe);
    const size_t e_pe_offset = model.e_pe->ne[0]*ggml_element_size(model.e_pe)*n_ctx*iter;

    struct ggml_tensor * e_pe = ggml_view_2d(ctx0, model.e_pe, model.e_pe->ne[0], n_ctx, e_pe_stride, e_pe_offset);

    cur = ggml_add(ctx0, e_pe, ggml_transpose(ctx0, cur));

    // ===================================================================

    // original:
    //cur = ggml_add(ctx0, model.e_pe, ggml_transpose(ctx0, cur));

    struct ggml_tensor * inpL = cur;

    for (int il = 0; il < n_layer; ++il) {
        const auto & layer = model.layers_encoder[il];

        // norm
        {
            wctx.use_buf(ctx0, 0);

            cur = ggml_norm(ctx0, inpL);

            // cur = ln_0_w*cur + ln_0_b
            cur = ggml_add(ctx0,
                    ggml_mul(ctx0,
                        ggml_repeat(ctx0, layer.attn_ln_0_w, cur),
                        cur),
                    ggml_repeat(ctx0, layer.attn_ln_0_b, cur));
        }

        // self-attention
        {
            wctx.use_buf(ctx0, 1);

            struct ggml_tensor * Qcur = ggml_mul_mat(ctx0,
                    layer.attn_q_w,
                    cur);

            Qcur = ggml_add(ctx0,
                    ggml_repeat(ctx0,
                        layer.attn_q_b,
                        Qcur),
                    Qcur);

            //Qcur = ggml_scale(ctx0, Qcur, ggml_new_f32(ctx0, pow(float(n_state)/n_head, -0.25)));

            // note: no bias for Key
            struct ggml_tensor * Kcur = ggml_mul_mat(ctx0,
                    layer.attn_k_w,
                    cur);

            //Kcur = ggml_scale(ctx0, Kcur, ggml_new_f32(ctx0, pow(float(n_state)/n_head, -0.25)));

            struct ggml_tensor * Vcur = ggml_mul_mat(ctx0,
                    layer.attn_v_w,
                    cur);

            Vcur = ggml_add(ctx0,
                    ggml_repeat(ctx0,
                        layer.attn_v_b,
                        Vcur),
                    Vcur);

            // ------

            wctx.use_buf(ctx0, 0);

#ifdef WHISPER_USE_FLASH_ATTN
            struct ggml_tensor * Q =
                ggml_permute(ctx0,
                        ggml_cpy(ctx0,
                            Qcur,
                            ggml_new_tensor_3d(ctx0, wctx.wtype, n_state/n_head, n_head, n_ctx)),
                        0, 2, 1, 3);

            struct ggml_tensor * K =
                ggml_permute(ctx0,
                        ggml_cpy(ctx0,
                            Kcur,
                            ggml_new_tensor_3d(ctx0, wctx.wtype, n_state/n_head, n_head, n_ctx)),
                        0, 2, 1, 3);

            struct ggml_tensor * V =
                ggml_cpy(ctx0,
                        ggml_permute(ctx0,
                            ggml_reshape_3d(ctx0,
                                Vcur,
                                n_state/n_head, n_head, n_ctx),
                            1, 2, 0, 3),
                        ggml_new_tensor_3d(ctx0, wctx.wtype, n_ctx, n_state/n_head, n_head)
                        );

            struct ggml_tensor * KQV = ggml_flash_attn(ctx0, Q, K, V, false);
#else
            struct ggml_tensor * Q =
                ggml_permute(ctx0,
                        ggml_cpy(ctx0,
                            Qcur,
                            ggml_new_tensor_3d(ctx0, GGML_TYPE_F32, n_state/n_head, n_head, n_ctx)),
                        0, 2, 1, 3);

            struct ggml_tensor * K =
                ggml_permute(ctx0,
                        ggml_cpy(ctx0,
                            Kcur,
                            ggml_new_tensor_3d(ctx0, wctx.wtype, n_state/n_head, n_head, n_ctx)),
                        0, 2, 1, 3);

            // K * Q
            struct ggml_tensor * KQ = ggml_mul_mat(ctx0, K, Q);

            struct ggml_tensor * KQ_scaled =
                ggml_scale(ctx0,
                        KQ,
                        ggml_new_f32(ctx0, 1.0f/sqrt(float(n_state)/n_head))
                        );

            struct ggml_tensor * KQ_soft_max = ggml_soft_max(ctx0, KQ_scaled);

            //struct ggml_tensor * V_trans =
            //    ggml_permute(ctx0,
            //            ggml_cpy(ctx0,
            //                Vcur,
            //                ggml_new_tensor_3d(ctx0, wctx.wtype, n_state/n_head, n_head, n_ctx)),
            //            1, 2, 0, 3);

            //struct ggml_tensor * KQV = ggml_mul_mat(ctx0, V_trans, KQ_soft_max);

            struct ggml_tensor * V =
                ggml_cpy(ctx0,
                        ggml_permute(ctx0,
                            ggml_reshape_3d(ctx0,
                                Vcur,
                                n_state/n_head, n_head, n_ctx),
                            0, 2, 1, 3),
                        ggml_new_tensor_3d(ctx0, wctx.wtype, n_state/n_head, n_ctx, n_head)
                        );

            struct ggml_tensor * KQV = ggml_mul_mat(ctx0, ggml_transpose(ctx0, V), KQ_soft_max);
#endif
            struct ggml_tensor * KQV_merged = ggml_permute(ctx0, KQV, 0, 2, 1, 3);

            wctx.use_buf(ctx0, 1);

            cur = ggml_cpy(ctx0,
                    KQV_merged,
                    ggml_new_tensor_2d(ctx0, GGML_TYPE_F32, n_state, n_ctx));
        }

        // projection
        {
            wctx.use_buf(ctx0, 0);

            cur = ggml_mul_mat(ctx0,
                    layer.attn_ln_1_w,
                    cur);

            wctx.use_buf(ctx0, 1);

            cur = ggml_add(ctx0,
                    ggml_repeat(ctx0, layer.attn_ln_1_b, cur),
                    cur);
        }

        wctx.use_buf(ctx0, 2);

        // add the input
        cur = ggml_add(ctx0, cur, inpL);

        struct ggml_tensor * inpFF = cur;

        // feed-forward network
        {
            // norm
            {
                wctx.use_buf(ctx0, 0);

                cur = ggml_norm(ctx0, inpFF);

                wctx.use_buf(ctx0, 1);

                // cur = mlp_ln_w*cur + mlp_ln_b
                cur = ggml_add(ctx0,
                        ggml_mul(ctx0,
                            ggml_repeat(ctx0, layer.mlp_ln_w, cur),
                            cur),
                        ggml_repeat(ctx0, layer.mlp_ln_b, cur));
            }

#ifdef WHISPER_USE_FLASH_FF
            wctx.use_buf(ctx0, 0);

            cur = ggml_flash_ff(ctx0,
                    ggml_cpy(ctx0, cur, ggml_new_tensor_2d(ctx0, wctx.wtype, n_state, n_ctx)),
                    layer.mlp_0_w, layer.mlp_0_b, layer.mlp_1_w, layer.mlp_1_b);
#else
            wctx.use_buf(ctx0, 0);

            // fully connected
            cur = ggml_mul_mat(ctx0,
                    layer.mlp_0_w,
                    cur);

            wctx.use_buf(ctx0, 1);

            cur = ggml_add(ctx0,
                    ggml_repeat(ctx0, layer.mlp_0_b, cur),
                    cur);

            wctx.use_buf(ctx0, 0);

            // GELU activation
            cur = ggml_gelu(ctx0, cur);

            wctx.use_buf(ctx0, 1);

            // projection
            cur = ggml_mul_mat(ctx0,
                    layer.mlp_1_w,
                    cur);

            wctx.use_buf(ctx0, 0);

            cur = ggml_add(ctx0,
                    ggml_repeat(ctx0, layer.mlp_1_b, cur),
                    cur);
#endif
        }

        wctx.use_buf(ctx0, 3);

        inpL = ggml_add(ctx0, cur, inpFF);
    }

    cur = inpL;

    // norm
    {
        wctx.use_buf(ctx0, 0);

        cur = ggml_norm(ctx0, cur);

        wctx.use_buf(ctx0, 1);

        // cur = ln_f_g*cur + ln_f_b
        cur = ggml_add(ctx0,
                ggml_mul(ctx0,
                    ggml_repeat(ctx0, model.e_ln_w, cur),
                    cur),
                ggml_repeat(ctx0, model.e_ln_b, cur));
    }

    wctx.use_buf(ctx0, -1);

    // run the computation
    {
        struct ggml_cgraph gf = {};
        gf.n_threads = n_threads;

        ggml_build_forward_expand(&gf, cur);
        ggml_graph_compute       (ctx0, &gf);

        //ggml_graph_print(&gf);
    }

    // cur
    //{
    //    printf("ne0 = %d\n", cur->ne[0]);
    //    printf("ne1 = %d\n", cur->ne[1]);
    //    for (int i = 0; i < 10; ++i) {
    //        printf("%8.4f ", ((float *)(cur->data))[i]);
    //    }
    //    printf("... ");
    //    for (int i = cur->ne[0] - 10; i < cur->ne[0]; ++i) {
    //        printf("%8.4f ", ((float *)(cur->data))[i]);
    //    }
    //    printf("\n");
    //}

    // pre-compute cross-attention memory
    {
        struct ggml_cgraph gf = {};
        gf.n_threads = n_threads;

        // TODO: hack to disconnect the encoded features from the previous graph
        cur->op = GGML_OP_NONE;
        cur->src0 = nullptr;
        cur->src1 = nullptr;

        for (int il = 0; il < model.hparams.n_text_layer; ++il) {
            auto & layer = model.layers_decoder[il];

            wctx.use_buf(ctx0, 0);

            struct ggml_tensor * Kcross = ggml_mul_mat(ctx0,
                    layer.cross_attn_k_w,
                    cur);

            Kcross = ggml_scale(ctx0, Kcross, ggml_new_f32(ctx0, pow(float(n_state)/n_head, -0.25)));

            wctx.use_buf(ctx0, 1);

            struct ggml_tensor * Vcross = ggml_mul_mat(ctx0,
                    layer.cross_attn_v_w,
                    cur);

            Vcross = ggml_add(ctx0,
                    ggml_repeat(ctx0,
                        layer.cross_attn_v_b,
                        Vcross),
                    Vcross);

            wctx.use_buf(ctx0, -1);

            //struct ggml_tensor * k = ggml_view_1d(ctx0, wctx.kv_cross.k, n_state*n_ctx, (ggml_element_size(wctx.kv_cross.k)*n_state)*(il*hparams.n_audio_ctx + iter*n_ctx));
            //struct ggml_tensor * v = ggml_view_1d(ctx0, wctx.kv_cross.v, n_state*n_ctx, (ggml_element_size(wctx.kv_cross.v)*n_state)*(il*hparams.n_audio_ctx + iter*n_ctx));
            struct ggml_tensor * k = ggml_view_1d(ctx0, wctx.kv_cross.k, n_state*n_ctx, (ggml_element_size(wctx.kv_cross.k)*n_state)*(il*n_ctx));
            struct ggml_tensor * v = ggml_view_1d(ctx0, wctx.kv_cross.v, n_state*n_ctx, (ggml_element_size(wctx.kv_cross.v)*n_state)*(il*n_ctx));

            ggml_build_forward_expand(&gf, ggml_cpy(ctx0, Kcross, k));
            ggml_build_forward_expand(&gf, ggml_cpy(ctx0, Vcross, v));
        }

        ggml_graph_compute(ctx0, &gf);
        //ggml_graph_print(&gf);
    }

    ////////////////////////////////////////////////////////////////////////////

    //printf("%s: used_mem = %f MB, %f MB, %f MB %f MB %f MB\n", __func__,
    //        ggml_used_mem(ctx0)/1024.0/1024.0,
    //        wctx.get_buf_max_mem(0)/1024.0/1024.0,
    //        wctx.get_buf_max_mem(1)/1024.0/1024.0,
    //        wctx.get_buf_max_mem(2)/1024.0/1024.0,
    //        wctx.get_buf_max_mem(3)/1024.0/1024.0);

    ggml_free(ctx0);

    wctx.t_encode_us += ggml_time_us() - t_start_us;
    wctx.n_encode++;

    return true;
}

// evaluate the decoder
//
// given text prompt + audio features -> computes the logits for the next token
//
//   - model:      the model
//   - n_threads:  number of threads to use
//   - tokens:     text prompt
//   - n_tokens:   number of tokens in the prompt
//   - n_past:     number of past tokens to prefix the prompt with
//
static bool whisper_decode(
        whisper_context & wctx,
        whisper_decoder & decoder,
    const whisper_token * tokens,
              const int   n_tokens,
              const int   n_past,
              const int   n_threads) {
    const int64_t t_start_us = ggml_time_us();

    const auto & model   = wctx.model;
    const auto & hparams = model.hparams;

    auto & kv_self = decoder.kv_self;

    WHISPER_ASSERT(!!kv_self.ctx);

    auto & logits_out = wctx.logits;

    const int n_vocab = hparams.n_vocab;

    const int n_ctx   = hparams.n_text_ctx;
    const int n_state = hparams.n_text_state;
    const int n_head  = hparams.n_text_head;
    const int n_layer = hparams.n_text_layer;

    const int N = n_tokens;
    const int M = wctx.exp_n_audio_ctx > 0 ? wctx.exp_n_audio_ctx : hparams.n_audio_ctx;

    //WHISPER_PRINT_DEBUG("%s: n_past = %d, N = %d, M = %d, n_ctx = %d\n", __func__, n_past, N, M, n_ctx);

    struct ggml_init_params params;
    params.mem_size   = wctx.buf_compute.size();
    params.mem_buffer = wctx.buf_compute.data();

    struct ggml_context * ctx0 = ggml_init(params);

    struct ggml_cgraph gf = {};
    gf.n_threads = n_threads;

    struct ggml_tensor * embd = ggml_new_tensor_1d(ctx0, GGML_TYPE_I32, N);
    memcpy(embd->data, tokens, N*ggml_element_size(embd));

    struct ggml_tensor * position = ggml_new_tensor_1d(ctx0, GGML_TYPE_I32, N);
    for (int i = 0; i < N; ++i) {
        ((int32_t *) position->data)[i] = n_past + i;
    }

    wctx.use_buf(ctx0, 3);

    // token encoding + position encoding
    struct ggml_tensor * cur =
        ggml_add(ctx0,
                ggml_get_rows(ctx0, model.d_te, embd),
                ggml_get_rows(ctx0, model.d_pe, position));

    struct ggml_tensor * inpL = cur;

    for (int il = 0; il < n_layer; ++il) {
        const auto & layer = model.layers_decoder[il];

        // norm
        {
            wctx.use_buf(ctx0, 0);

            cur = ggml_norm(ctx0, inpL);

            // cur = ln_0_w*cur + ln_0_b
            cur = ggml_add(ctx0,
                    ggml_mul(ctx0,
                        ggml_repeat(ctx0, layer.attn_ln_0_w, cur),
                        cur),
                    ggml_repeat(ctx0, layer.attn_ln_0_b, cur));
        }

        // self-attention
        {
            wctx.use_buf(ctx0, 1);

            struct ggml_tensor * Qcur = ggml_mul_mat(ctx0,
                    layer.attn_q_w,
                    cur);

            Qcur = ggml_add(ctx0,
                    ggml_repeat(ctx0,
                        layer.attn_q_b,
                        Qcur),
                    Qcur);

            Qcur = ggml_scale(ctx0, Qcur, ggml_new_f32(ctx0, pow(float(n_state)/n_head, -0.25)));

            // note: no bias for Key
            struct ggml_tensor * Kcur = ggml_mul_mat(ctx0,
                    layer.attn_k_w,
                    cur);

            Kcur = ggml_scale(ctx0, Kcur, ggml_new_f32(ctx0, pow(float(n_state)/n_head, -0.25)));

            struct ggml_tensor * Vcur = ggml_mul_mat(ctx0,
                    layer.attn_v_w,
                    cur);

            Vcur = ggml_add(ctx0,
                    ggml_repeat(ctx0,
                        layer.attn_v_b,
                        Vcur),
                    Vcur);

            // store key and value to memory
            {
                struct ggml_tensor * k = ggml_view_1d(ctx0, kv_self.k, N*n_state, (ggml_element_size(kv_self.k)*n_state)*(il*n_ctx + n_past));
                struct ggml_tensor * v = ggml_view_1d(ctx0, kv_self.v, N*n_state, (ggml_element_size(kv_self.v)*n_state)*(il*n_ctx + n_past));

                ggml_build_forward_expand(&gf, ggml_cpy(ctx0, Kcur, k));
                ggml_build_forward_expand(&gf, ggml_cpy(ctx0, Vcur, v));
            }

            // ------

            wctx.use_buf(ctx0, 0);

            struct ggml_tensor * Q =
                ggml_permute(ctx0,
                        ggml_cpy(ctx0,
                            Qcur,
                            ggml_new_tensor_3d(ctx0, GGML_TYPE_F32, n_state/n_head, n_head, N)),
                        0, 2, 1, 3);

            struct ggml_tensor * K =
                ggml_permute(ctx0,
                        ggml_reshape_3d(ctx0,
                            ggml_view_1d(ctx0, kv_self.k, (n_past + N)*n_state, il*n_ctx*ggml_element_size(kv_self.k)*n_state),
                            n_state/n_head, n_head, n_past + N),
                        0, 2, 1, 3);

            wctx.use_buf(ctx0, 1);

            // K * Q
            struct ggml_tensor * KQ = ggml_mul_mat(ctx0, K, Q);

            wctx.use_buf(ctx0, 0);

            //struct ggml_tensor * KQ_scaled =
            //    ggml_scale(ctx0,
            //            KQ,
            //            ggml_new_f32(ctx0, 1.0f/sqrt(float(n_state)/n_head))
            //            );

            struct ggml_tensor * KQ_masked = ggml_diag_mask_inf(ctx0, KQ, n_past);

            wctx.use_buf(ctx0, 1);

            struct ggml_tensor * KQ_soft_max = ggml_soft_max(ctx0, KQ_masked);

            wctx.use_buf(ctx0, 0);

            struct ggml_tensor * V_trans =
                ggml_permute(ctx0,
                        ggml_reshape_3d(ctx0,
                            ggml_view_1d(ctx0, kv_self.v, (n_past + N)*n_state, il*n_ctx*ggml_element_size(kv_self.v)*n_state),
                            n_state/n_head, n_head, n_past + N),
                        1, 2, 0, 3);

            wctx.use_buf(ctx0, 1);

            struct ggml_tensor * KQV = ggml_mul_mat(ctx0, V_trans, KQ_soft_max);

            struct ggml_tensor * KQV_merged = ggml_permute(ctx0, KQV, 0, 2, 1, 3);

            cur = ggml_cpy(ctx0,
                    KQV_merged,
                    ggml_new_tensor_2d(ctx0, GGML_TYPE_F32, n_state, N));
        }

        // projection
        {
            wctx.use_buf(ctx0, 0);

            cur = ggml_mul_mat(ctx0,
                    layer.attn_ln_1_w,
                    cur);

            wctx.use_buf(ctx0, 1);

            cur = ggml_add(ctx0,
                    ggml_repeat(ctx0, layer.attn_ln_1_b, cur),
                    cur);
        }

        wctx.use_buf(ctx0, 2);

        // add the input
        struct ggml_tensor * inpCA = ggml_add(ctx0, cur, inpL);

        // norm
        {
            wctx.use_buf(ctx0, 0);

            cur = ggml_norm(ctx0, inpCA); // note: we use inpCA here

            wctx.use_buf(ctx0, 1);

            // cur = ln_0_w*cur + ln_0_b
            cur = ggml_add(ctx0,
                    ggml_mul(ctx0,
                        ggml_repeat(ctx0, layer.cross_attn_ln_0_w, cur),
                        cur),
                    ggml_repeat(ctx0, layer.cross_attn_ln_0_b, cur));
        }

        // cross-attention
        {
            wctx.use_buf(ctx0, 0);

            struct ggml_tensor * Qcur = ggml_mul_mat(ctx0,
                    layer.cross_attn_q_w,
                    cur);

            Qcur = ggml_add(ctx0,
                    ggml_repeat(ctx0,
                        layer.cross_attn_q_b,
                        Qcur),
                    Qcur);

            Qcur = ggml_scale(ctx0, Qcur, ggml_new_f32(ctx0, pow(float(n_state)/n_head, -0.25)));

            // Kcross is already scaled
            struct ggml_tensor * Kcross =
                ggml_reshape_3d(ctx0,
                        ggml_view_1d(ctx0, wctx.kv_cross.k, M*n_state, il*M*ggml_element_size(wctx.kv_cross.k)*n_state),
                        n_state/n_head, n_head, M);

            struct ggml_tensor * Vcross =
                ggml_reshape_3d(ctx0,
                        ggml_view_1d(ctx0, wctx.kv_cross.v, M*n_state, il*M*ggml_element_size(wctx.kv_cross.v)*n_state),
                        n_state/n_head, n_head, M);

            struct ggml_tensor * V_trans = ggml_permute(ctx0, Vcross, 1, 2, 0, 3);

            // ------

            wctx.use_buf(ctx0, 1);

            struct ggml_tensor * Q =
                ggml_permute(ctx0,
                        ggml_cpy(ctx0,
                            Qcur,
                            ggml_new_tensor_3d(ctx0, GGML_TYPE_F32, n_state/n_head, n_head, N)),
                        0, 2, 1, 3);

            struct ggml_tensor * K = ggml_permute(ctx0, Kcross, 0, 2, 1, 3);

            wctx.use_buf(ctx0, 0);

            // K * Q
            struct ggml_tensor * KQ = ggml_mul_mat(ctx0, K, Q);

            //struct ggml_tensor * KQ_scaled =
            //    ggml_scale(ctx0,
            //            KQ,
            //            ggml_new_f32(ctx0, 1.0f/sqrt(float(n_state)/n_head))
            //            );

            // no masking for cross-attention
            //struct ggml_tensor * KQ_masked = ggml_diag_mask_inf(ctx0, KQ_scaled, n_past);

            wctx.use_buf(ctx0, 1);

            struct ggml_tensor * KQ_soft_max = ggml_soft_max(ctx0, KQ);

            wctx.use_buf(ctx0, 0);

            struct ggml_tensor * KQV = ggml_mul_mat(ctx0, V_trans, KQ_soft_max);

            wctx.use_buf(ctx0, 1);

            struct ggml_tensor * KQV_merged = ggml_permute(ctx0, KQV, 0, 2, 1, 3);

            // cur = KQV_merged.contiguous().view(n_state, N)
            cur = ggml_cpy(ctx0,
                    KQV_merged,
                    ggml_new_tensor_2d(ctx0, GGML_TYPE_F32, n_state, N));
        }

        // projection
        {
            wctx.use_buf(ctx0, 0);

            cur = ggml_mul_mat(ctx0,
                    layer.cross_attn_ln_1_w,
                    cur);

            wctx.use_buf(ctx0, 1);

            cur = ggml_add(ctx0,
                    ggml_repeat(ctx0, layer.cross_attn_ln_1_b, cur),
                    cur);
        }

        wctx.use_buf(ctx0, 2);

        // add the input
        cur = ggml_add(ctx0, cur, inpCA);

        struct ggml_tensor * inpFF = cur;

        // feed-forward network
        {
            // norm
            {
                wctx.use_buf(ctx0, 0);

                cur = ggml_norm(ctx0, inpFF);

                wctx.use_buf(ctx0, 1);

                // cur = mlp_ln_w*cur + mlp_ln_b
                cur = ggml_add(ctx0,
                        ggml_mul(ctx0,
                            ggml_repeat(ctx0, layer.mlp_ln_w, cur),
                            cur),
                        ggml_repeat(ctx0, layer.mlp_ln_b, cur));
            }

            wctx.use_buf(ctx0, 0);

            // fully connected
            cur = ggml_mul_mat(ctx0,
                    layer.mlp_0_w,
                    cur);

            wctx.use_buf(ctx0, 1);

            cur = ggml_add(ctx0,
                    ggml_repeat(ctx0, layer.mlp_0_b, cur),
                    cur);

            wctx.use_buf(ctx0, 0);

            // GELU activation
            cur = ggml_gelu(ctx0, cur);

            wctx.use_buf(ctx0, 1);

            // projection
            cur = ggml_mul_mat(ctx0,
                    layer.mlp_1_w,
                    cur);

            wctx.use_buf(ctx0, 0);

            cur = ggml_add(ctx0,
                    ggml_repeat(ctx0, layer.mlp_1_b, cur),
                    cur);
        }

        wctx.use_buf(ctx0, 3);

        inpL = ggml_add(ctx0, cur, inpFF);
    }

    cur = inpL;

    // norm
    {
        wctx.use_buf(ctx0, 0);

        cur = ggml_norm(ctx0, cur);

        wctx.use_buf(ctx0, 1);

        cur = ggml_add(ctx0,
                ggml_mul(ctx0,
                    ggml_repeat(ctx0, model.d_ln_w, cur),
                    cur),
                ggml_repeat(ctx0, model.d_ln_b, cur));
    }

    wctx.use_buf(ctx0, 0);

    // compute logits only for the last token
    // comment this line to compute logits for all N tokens
    // might be useful in the future
    cur = ggml_view_2d(ctx0, cur, cur->ne[0], 1, cur->nb[1], (cur->ne[1] - 1)*cur->nb[1]);

    struct ggml_tensor * logits = ggml_mul_mat(ctx0, model.d_te, cur);

    wctx.use_buf(ctx0, -1);

    // run the computation
    {
        ggml_build_forward_expand(&gf, logits);
        ggml_graph_compute       (ctx0, &gf);
    }

    // extract logits for all N tokens
    //logits_out.resize(N*n_vocab);
    //memcpy(logits_out.data(), ggml_get_data(logits), sizeof(float)*N*n_vocab);

    // extract logits only for the last token
    logits_out.resize(n_vocab);
    memcpy(logits_out.data(), ggml_get_data(logits), sizeof(float)*n_vocab);

    if (N > 1) {
        //printf("%s: used_mem = %f MB, %f MB, %f MB %f MB %f MB\n", __func__,
        //        ggml_used_mem(ctx0)/1024.0/1024.0,
        //        wctx.get_buf_max_mem(0)/1024.0/1024.0,
        //        wctx.get_buf_max_mem(1)/1024.0/1024.0,
        //        wctx.get_buf_max_mem(2)/1024.0/1024.0,
        //        wctx.get_buf_max_mem(3)/1024.0/1024.0);
    }

    ggml_free(ctx0);

    wctx.t_decode_us += ggml_time_us() - t_start_us;
    wctx.n_decode++;

    return true;
}

//  500 -> 00:05.000
// 6000 -> 01:00.000
static std::string to_timestamp(int64_t t, bool comma = false) {
    int64_t msec = t * 10;
    int64_t hr = msec / (1000 * 60 * 60);
    msec = msec - hr * (1000 * 60 * 60);
    int64_t min = msec / (1000 * 60);
    msec = msec - min * (1000 * 60);
    int64_t sec = msec / 1000;
    msec = msec - sec * 1000;

    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d%s%03d", (int) hr, (int) min, (int) sec, comma ? "," : ".", (int) msec);

    return std::string(buf);
}

// naive Discrete Fourier Transform
// input is real-valued
// output is complex-valued
static void dft(const std::vector<float> & in, std::vector<float> & out) {
    int N = in.size();

    out.resize(N*2);

    for (int k = 0; k < N; k++) {
        float re = 0;
        float im = 0;

        for (int n = 0; n < N; n++) {
            float angle = 2*M_PI*k*n/N;
            re += in[n]*cos(angle);
            im -= in[n]*sin(angle);
        }

        out[k*2 + 0] = re;
        out[k*2 + 1] = im;
    }
}

// Cooley-Tukey FFT
// poor man's implementation - use something better
// input is real-valued
// output is complex-valued
static void fft(const std::vector<float> & in, std::vector<float> & out) {
    out.resize(in.size()*2);

    int N = in.size();

    if (N == 1) {
        out[0] = in[0];
        out[1] = 0;
        return;
    }

    if (N%2 == 1) {
        dft(in, out);
        return;
    }

    std::vector<float> even;
    std::vector<float> odd;

    even.reserve(N/2);
    odd.reserve(N/2);

    for (int i = 0; i < N; i++) {
        if (i % 2 == 0) {
            even.push_back(in[i]);
        } else {
            odd.push_back(in[i]);
        }
    }

    std::vector<float> even_fft;
    std::vector<float> odd_fft;

    fft(even, even_fft);
    fft(odd, odd_fft);

    for (int k = 0; k < N/2; k++) {
        float theta = 2*M_PI*k/N;

        float re = cos(theta);
        float im = -sin(theta);

        float re_odd = odd_fft[2*k + 0];
        float im_odd = odd_fft[2*k + 1];

        out[2*k + 0] = even_fft[2*k + 0] + re*re_odd - im*im_odd;
        out[2*k + 1] = even_fft[2*k + 1] + re*im_odd + im*re_odd;

        out[2*(k + N/2) + 0] = even_fft[2*k + 0] - re*re_odd + im*im_odd;
        out[2*(k + N/2) + 1] = even_fft[2*k + 1] - re*im_odd - im*re_odd;
    }
}

// ref: https://github.com/openai/whisper/blob/main/whisper/audio.py#L92-L124
static bool log_mel_spectrogram(
        whisper_context & wctx,
            const float * samples,
              const int   n_samples,
              const int   /*sample_rate*/,
              const int   fft_size,
              const int   fft_step,
              const int   n_mel,
              const int   n_threads,
  const whisper_filters & filters,
             const bool   speed_up,
            whisper_mel & mel) {
    const int64_t t_start_us = ggml_time_us();

    // Hanning window
    std::vector<float> hann;
    hann.resize(fft_size);
    for (int i = 0; i < fft_size; i++) {
        hann[i] = 0.5*(1.0 - cos((2.0*M_PI*i)/(fft_size)));
    }

    mel.n_mel = n_mel;
    mel.n_len = (n_samples)/fft_step;
    mel.data.resize(mel.n_mel*mel.n_len);

    const int n_fft = 1 + (speed_up ? fft_size/4 : fft_size/2);

    //printf("%s: n_samples = %d, n_len = %d\n", __func__, n_samples, mel.n_len);
    //printf("%s: recording length: %f s\n", __func__, (float) n_samples/sample_rate);

    std::vector<std::thread> workers(n_threads);
    for (int iw = 0; iw < n_threads; ++iw) {
        workers[iw] = std::thread([&](int ith) {
            std::vector<float> fft_in;
            fft_in.resize(fft_size);
            for (int i = 0; i < fft_size; i++) {
                fft_in[i] = 0.0;
            }

            std::vector<float> fft_out;
            fft_out.resize(2*fft_size);

            for (int i = ith; i < mel.n_len; i += n_threads) {
                const int offset = i*fft_step;

                // apply Hanning window
                for (int j = 0; j < fft_size; j++) {
                    if (offset + j < n_samples) {
                        fft_in[j] = hann[j]*samples[offset + j];
                    } else {
                        fft_in[j] = 0.0;
                    }
                }

                // FFT -> mag^2
                fft(fft_in, fft_out);

                for (int j = 0; j < fft_size; j++) {
                    fft_out[j] = (fft_out[2*j + 0]*fft_out[2*j + 0] + fft_out[2*j + 1]*fft_out[2*j + 1]);
                }
                for (int j = 1; j < fft_size/2; j++) {
                    //if (i == 0) {
                    //    printf("%d: %f %f\n", j, fft_out[j], fft_out[fft_size - j]);
                    //}
                    fft_out[j] += fft_out[fft_size - j];
                }
                if (i == 0) {
                    //for (int j = 0; j < fft_size; j++) {
                    //    printf("%d: %e\n", j, fft_out[j]);
                    //}
                }

                if (speed_up) {
                    // scale down in the frequency domain results in a speed up in the time domain
                    for (int j = 0; j < n_fft; j++) {
                        fft_out[j] = 0.5*(fft_out[2*j] + fft_out[2*j + 1]);
                    }
                }

                // mel spectrogram
                for (int j = 0; j < mel.n_mel; j++) {
                    double sum = 0.0;

                    for (int k = 0; k < n_fft; k++) {
                        sum += fft_out[k]*filters.data[j*n_fft + k];
                    }
                    if (sum < 1e-10) {
                        sum = 1e-10;
                    }

                    sum = log10(sum);

                    mel.data[j*mel.n_len + i] = sum;
                }
            }
        }, iw);
    }

    for (int iw = 0; iw < n_threads; ++iw) {
        workers[iw].join();
    }

    // clamping and normalization
    double mmax = -1e20;
    for (int i = 0; i < mel.n_mel*mel.n_len; i++) {
        if (mel.data[i] > mmax) {
            mmax = mel.data[i];
        }
    }
    //printf("%s: max = %f\n", __func__, mmax);

    mmax -= 8.0;

    for (int i = 0; i < mel.n_mel*mel.n_len; i++) {
        if (mel.data[i] < mmax) {
            mel.data[i] = mmax;
        }

        mel.data[i] = (mel.data[i] + 4.0)/4.0;
    }

    wctx.t_mel_us += ggml_time_us() - t_start_us;

    return true;
}

// split text into tokens
//
// ref: https://github.com/openai/gpt-2/blob/a74da5d99abaaba920de8131d64da2862a8f213b/src/encoder.py#L53
//
// Regex (Python):
// r"""'s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+"""
//
// Regex (C++):
// R"('s|'t|'re|'ve|'m|'ll|'d| ?[[:alpha:]]+| ?[[:digit:]]+| ?[^\s[:alpha:][:digit:]]+|\s+(?!\S)|\s+)"
//
static std::vector<whisper_vocab::id> tokenize(const whisper_vocab & vocab, const std::string & text) {
    std::vector<std::string> words;

    // first split the text into words
    {
        std::string str = text;
        std::string pat = R"('s|'t|'re|'ve|'m|'ll|'d| ?[[:alpha:]]+| ?[[:digit:]]+| ?[^\s[:alpha:][:digit:]]+|\s+(?!\S)|\s+)";

        std::regex re(pat);
        std::smatch m;

        while (std::regex_search(str, m, re)) {
            for (auto x : m) {
                words.push_back(x);
            }
            str = m.suffix();
        }
    }

    // find the longest tokens that form the words:
    std::vector<whisper_vocab::id> tokens;
    for (const auto & word : words) {
        if (word.empty()) continue;

        int i = 0;
        int n = word.size();
        while (i < n) {
            int j = n;
            while (j > i) {
                auto it = vocab.token_to_id.find(word.substr(i, j-i));
                if (it != vocab.token_to_id.end()) {
                    tokens.push_back(it->second);
                    i = j;
                    break;
                }
                --j;
            }
            if (i == n) {
                break;
            }
            if (j == i) {
                auto sub = word.substr(i, 1);
                if (vocab.token_to_id.find(sub) != vocab.token_to_id.end()) {
                    tokens.push_back(vocab.token_to_id.at(sub));
                } else {
                    fprintf(stderr, "%s: unknown token '%s'\n", __func__, sub.data());
                }
                ++i;
            }
        }
    }

    return tokens;
}

//
// interface implementation
//

struct whisper_context * whisper_init_from_file(const char * path_model) {
    whisper_model_loader loader = {};

    fprintf(stderr, "%s: loading model from '%s'\n", __func__, path_model);

    auto fin = std::ifstream(path_model, std::ios::binary);
    if (!fin) {
        fprintf(stderr, "%s: failed to open '%s'\n", __func__, path_model);
        return nullptr;
    }

    loader.context = &fin;
    loader.read = [](void * ctx, void * output, size_t read_size) {
        std::ifstream * fin = (std::ifstream*)ctx;
        fin->read((char *)output, read_size);
        return read_size;
    };

    loader.eof = [](void * ctx) {
        std::ifstream * fin = (std::ifstream*)ctx;
        return fin->eof();
    };

    loader.close = [](void * ctx) {
        std::ifstream * fin = (std::ifstream*)ctx;
        fin->close();
    };

    return whisper_init(&loader);
}

struct whisper_context * whisper_init_from_buffer(void * buffer, size_t buffer_size) {
    struct buf_context {
        uint8_t* buffer;
        size_t size;
        size_t current_offset;
    };

    buf_context ctx = { reinterpret_cast<uint8_t*>(buffer), buffer_size, 0 };
    whisper_model_loader loader = {};

    fprintf(stderr, "%s: loading model from buffer\n", __func__);

    loader.context = &ctx;

    loader.read = [](void * ctx, void * output, size_t read_size) {
        buf_context * buf = reinterpret_cast<buf_context *>(ctx);

        size_t size_to_copy = buf->current_offset + read_size < buf->size ? read_size : buf->size - buf->current_offset;

        memcpy(output, buf->buffer + buf->current_offset, size_to_copy);
        buf->current_offset += size_to_copy;

        return size_to_copy;
    };

    loader.eof = [](void * ctx) {
        buf_context * buf = reinterpret_cast<buf_context *>(ctx);

        return buf->current_offset >= buf->size;
    };

    loader.close = [](void * /*ctx*/) { };

    return whisper_init(&loader);
}

struct whisper_context * whisper_init(struct whisper_model_loader * loader) {
    ggml_time_init();

    whisper_context * ctx = new whisper_context;

    if (!whisper_model_load(loader, *ctx)) {
        loader->close(loader->context);
        fprintf(stderr, "%s: failed to load model\n", __func__);
        delete ctx;
        return nullptr;
    }

    loader->close(loader->context);

    return ctx;
}

void whisper_free(struct whisper_context * ctx) {
    if (ctx) {
        if (ctx->model.ctx) {
            ggml_free(ctx->model.ctx);
        }
        if (ctx->model.buf) {
            delete ctx->model.buf;
        }
        if (ctx->kv_cross.ctx) {
            ggml_free(ctx->kv_cross.ctx);
        }
        for (int i = 0; i < WHISPER_MAX_DECODERS; ++i) {
            if (ctx->decoders[i].kv_self.ctx) {
                ggml_free(ctx->decoders[i].kv_self.ctx);
            }
        }
        delete ctx;
    }
}

int whisper_pcm_to_mel(struct whisper_context * ctx, const float * samples, int n_samples, int n_threads) {
    if (!log_mel_spectrogram(*ctx, samples, n_samples, WHISPER_SAMPLE_RATE, WHISPER_N_FFT, WHISPER_HOP_LENGTH, WHISPER_N_MEL, n_threads, ctx->model.filters, false, ctx->mel)) {
        fprintf(stderr, "%s: failed to compute mel spectrogram\n", __func__);
        return -1;
    }

    return 0;
}

// same as whisper_pcm_to_mel, but applies a Phase Vocoder to speed up the audio x2
int whisper_pcm_to_mel_phase_vocoder(struct whisper_context * ctx, const float * samples, int n_samples, int n_threads) {
    if (!log_mel_spectrogram(*ctx, samples, n_samples, WHISPER_SAMPLE_RATE, 2*WHISPER_N_FFT, 2*WHISPER_HOP_LENGTH, WHISPER_N_MEL, n_threads, ctx->model.filters, true, ctx->mel)) {
        fprintf(stderr, "%s: failed to compute mel spectrogram\n", __func__);
        return -1;
    }

    return 0;
}

int whisper_set_mel(
        struct whisper_context * ctx,
        const float * data,
        int n_len,
        int n_mel) {
    if (n_mel != WHISPER_N_MEL) {
        fprintf(stderr, "%s: invalid number of mel bands: %d (expected %d)\n", __func__, n_mel, WHISPER_N_MEL);
        return -1;
    }

    ctx->mel.n_len = n_len;
    ctx->mel.n_mel = n_mel;

    ctx->mel.data.resize(n_len*n_mel);
    memcpy(ctx->mel.data.data(), data, n_len*n_mel*sizeof(float));

    return 0;
}

int whisper_encode(struct whisper_context * ctx, int offset, int n_threads) {
    if (!whisper_encode(*ctx, offset, n_threads)) {
        fprintf(stderr, "%s: failed to eval\n", __func__);
        return -1;
    }

    return 0;
}

int whisper_decode(struct whisper_context * ctx, const whisper_token * tokens, int n_tokens, int n_past, int n_threads) {
    // TODO: add selected_decoder_id to context
    const int selected_decoder_id = 0;

    if (!whisper_decode(*ctx, ctx->decoders[selected_decoder_id], tokens, n_tokens, n_past, n_threads)) {
        fprintf(stderr, "%s: failed to eval\n", __func__);
        return 1;
    }

    return 0;
}

int whisper_tokenize(struct whisper_context * ctx, const char * text, whisper_token * tokens, int n_max_tokens) {
    const auto res = tokenize(ctx->vocab, text);

    if (n_max_tokens < (int) res.size()) {
        fprintf(stderr, "%s: too many resulting tokens: %d (max %d)\n", __func__, (int) res.size(), n_max_tokens);
        return -1;
    }

    for (int i = 0; i < (int) res.size(); i++) {
        tokens[i] = res[i];
    }

    return res.size();
}

int whisper_lang_max_id() {
    auto max_id = 0;
    for (const auto & kv : g_lang) {
        max_id = std::max(max_id, kv.second.first);
    }

    return max_id;
}

int whisper_lang_id(const char * lang) {
    if (!g_lang.count(lang)) {
        for (const auto & kv : g_lang) {
            if (kv.second.second == lang) {
                return kv.second.first;
            }
        }

        fprintf(stderr, "%s: unknown language '%s'\n", __func__, lang);
        return -1;
    }

    return g_lang.at(lang).first;
}

const char * whisper_lang_str(int id) {
    for (const auto & kv : g_lang) {
        if (kv.second.first == id) {
            return kv.first.c_str();
        }
    }

    fprintf(stderr, "%s: unknown language id %d\n", __func__, id);
    return nullptr;
}

int whisper_lang_auto_detect(
        struct whisper_context * ctx,
        int offset_ms,
        int n_threads,
        float * lang_probs) {
    const int seek = offset_ms/10;

    if (seek < 0) {
        fprintf(stderr, "%s: offset %dms is before the start of the audio\n", __func__, offset_ms);
        return -1;
    }

    if (seek >= ctx->mel.n_len) {
        fprintf(stderr, "%s: offset %dms is past the end of the audio (%dms)\n", __func__, offset_ms, ctx->mel.n_len*10);
        return -2;
    }

    // run the encoder
    if (whisper_encode(ctx, seek, n_threads) != 0) {
        fprintf(stderr, "%s: failed to encode\n", __func__);
        return -6;
    }

    const std::vector<whisper_token> prompt = { whisper_token_sot(ctx) };

    if (whisper_decode(ctx, prompt.data(), prompt.size(), 0, n_threads) != 0) {
        fprintf(stderr, "%s: failed to decode\n", __func__);
        return -7;
    }

    auto & logits_id = ctx->logits_id;
    logits_id.clear();

    for (const auto & kv : g_lang) {
        const auto token_lang = whisper_token_lang(ctx, kv.second.first);
        logits_id.emplace_back(ctx->logits[token_lang], kv.second.first);
    }

    // sort descending
    {
        using pair_type = std::remove_reference<decltype(logits_id)>::type::value_type;
        std::sort(logits_id.begin(), logits_id.end(), [](const pair_type & a, const pair_type & b) {
            return a.first > b.first;
        });
    }

    // softmax
    {
        const auto max = logits_id[0].first;

        double sum = 0.0f;
        for (auto & kv : logits_id) {
            kv.first = exp(kv.first - max);
            sum += kv.first;
        }

        for (auto & kv : logits_id) {
            kv.first /= sum;
        }
    }

    {
        for (const auto & prob : logits_id) {
            if (lang_probs) {
                lang_probs[prob.second] = prob.first;
            }

            //printf("%s: lang %2d (%3s): %f\n", __func__, prob.second, whisper_lang_str(prob.second), prob.first);
        }
    }

    return logits_id[0].second;
}

int whisper_n_len(struct whisper_context * ctx) {
    return ctx->mel.n_len;
}

int whisper_n_vocab(struct whisper_context * ctx) {
    return ctx->vocab.n_vocab;
}

int whisper_n_text_ctx(struct whisper_context * ctx) {
    return ctx->model.hparams.n_text_ctx;
}

int whisper_n_audio_ctx(struct whisper_context * ctx) {
    return ctx->model.hparams.n_audio_ctx;
}

int whisper_is_multilingual(struct whisper_context * ctx) {
    return ctx->vocab.is_multilingual() ? 1 : 0;
}

float * whisper_get_logits(struct whisper_context * ctx) {
    return ctx->logits.data();
}

const char * whisper_token_to_str(struct whisper_context * ctx, whisper_token token) {
    return ctx->vocab.id_to_token.at(token).c_str();
}

whisper_token whisper_token_eot(struct whisper_context * ctx) {
    return ctx->vocab.token_eot;
}

whisper_token whisper_token_sot(struct whisper_context * ctx) {
    return ctx->vocab.token_sot;
}

whisper_token whisper_token_prev(struct whisper_context * ctx) {
    return ctx->vocab.token_prev;
}

whisper_token whisper_token_solm(struct whisper_context * ctx) {
    return ctx->vocab.token_solm;
}

whisper_token whisper_token_not(struct whisper_context * ctx) {
    return ctx->vocab.token_not;
}

whisper_token whisper_token_beg(struct whisper_context * ctx) {
    return ctx->vocab.token_beg;
}

whisper_token whisper_token_lang(struct whisper_context * ctx, int lang_id) {
    return whisper_token_sot(ctx) + 1 + lang_id;
}

whisper_token whisper_token_translate(void) {
    return whisper_vocab::token_translate;
}

whisper_token whisper_token_transcribe(void) {
    return whisper_vocab::token_transcribe;
}

void whisper_print_timings(struct whisper_context * ctx) {
    const int64_t t_end_us = ggml_time_us();

    const int32_t n_sample = std::max(1, ctx->n_sample);
    const int32_t n_encode = std::max(1, ctx->n_encode);
    const int32_t n_decode = std::max(1, ctx->n_decode);

    fprintf(stderr, "\n");
    fprintf(stderr, "%s:     fallbacks = %3d p / %3d h\n", __func__, ctx->n_fail_p, ctx->n_fail_h);
    fprintf(stderr, "%s:     load time = %8.2f ms\n", __func__, ctx->t_load_us/1000.0f);
    fprintf(stderr, "%s:      mel time = %8.2f ms\n", __func__, ctx->t_mel_us/1000.0f);
    fprintf(stderr, "%s:   sample time = %8.2f ms / %5d runs (%8.2f ms per run)\n", __func__, 1e-3f*ctx->t_sample_us, n_sample, 1e-3f*ctx->t_sample_us/n_sample);
    fprintf(stderr, "%s:   encode time = %8.2f ms / %5d runs (%8.2f ms per run)\n", __func__, 1e-3f*ctx->t_encode_us, n_encode, 1e-3f*ctx->t_encode_us/n_encode);
    fprintf(stderr, "%s:   decode time = %8.2f ms / %5d runs (%8.2f ms per run)\n", __func__, 1e-3f*ctx->t_decode_us, n_decode, 1e-3f*ctx->t_decode_us/n_decode);
    fprintf(stderr, "%s:    total time = %8.2f ms\n", __func__, (t_end_us - ctx->t_start_us)/1000.0f);
}

void whisper_reset_timings(struct whisper_context * ctx) {
    ctx->t_sample_us = 0;
    ctx->t_encode_us = 0;
    ctx->t_decode_us = 0;
}

const char * whisper_print_system_info(void) {
    static std::string s;

    s  = "";
    s += "AVX = "       + std::to_string(ggml_cpu_has_avx())       + " | ";
    s += "AVX2 = "      + std::to_string(ggml_cpu_has_avx2())      + " | ";
    s += "AVX512 = "    + std::to_string(ggml_cpu_has_avx512())    + " | ";
    s += "FMA = "       + std::to_string(ggml_cpu_has_fma())       + " | ";
    s += "NEON = "      + std::to_string(ggml_cpu_has_neon())      + " | ";
    s += "ARM_FMA = "   + std::to_string(ggml_cpu_has_arm_fma())   + " | ";
    s += "F16C = "      + std::to_string(ggml_cpu_has_f16c())      + " | ";
    s += "FP16_VA = "   + std::to_string(ggml_cpu_has_fp16_va())   + " | ";
    s += "WASM_SIMD = " + std::to_string(ggml_cpu_has_wasm_simd()) + " | ";
    s += "BLAS = "      + std::to_string(ggml_cpu_has_blas())      + " | ";
    s += "SSE3 = "      + std::to_string(ggml_cpu_has_sse3())      + " | ";
    s += "VSX = "       + std::to_string(ggml_cpu_has_vsx())       + " | ";

    return s.c_str();
}

////////////////////////////////////////////////////////////////////////////

struct whisper_full_params whisper_full_default_params(enum whisper_sampling_strategy strategy) {
    struct whisper_full_params result = {
        /*.strategy         =*/ strategy,

        /*.n_threads        =*/ std::min(4, (int32_t) std::thread::hardware_concurrency()),
        /*.n_max_text_ctx   =*/ 16384,
        /*.offset_ms        =*/ 0,
        /*.duration_ms      =*/ 0,

        /*.translate        =*/ false,
        /*.no_context       =*/ false,
        /*.single_segment   =*/ false,
        /*.print_special    =*/ false,
        /*.print_progress   =*/ true,
        /*.print_realtime   =*/ false,
        /*.print_timestamps =*/ true,

        /*.token_timestamps =*/ false,
        /*.thold_pt         =*/ 0.01f,
        /*.thold_ptsum      =*/ 0.01f,
        /*.max_len          =*/ 0,
        /*.split_on_word    =*/ false,
        /*.max_tokens       =*/ 0,

        /*.speed_up         =*/ false,
        /*.audio_ctx        =*/ 0,

        /*.prompt_tokens    =*/ nullptr,
        /*.prompt_n_tokens  =*/ 0,

        /*.language         =*/ "en",

        /*.suppress_blank   =*/ true,
        /*.suppress_non_speech_tokens =*/ false,

        /*.temperature      =*/  0.0f,
        /*.max_initial_ts   =*/  1.0f,
        /*.length_penalty   =*/ -1.0f,

        /*.temperature_inc  =*/  0.2f,
        /*.entropy_thold    =*/  2.4f,
        /*.logprob_thold    =*/ -1.0f,
        /*.no_speech_thold  =*/  0.6f,

        /*.greedy           =*/ {
            /*.best_of   =*/ -1,
        },

        /*.beam_search      =*/ {
            /*.beam_size =*/ -1,

            /*.patience  =*/ -1.0f,
        },

        /*.new_segment_callback           =*/ nullptr,
        /*.new_segment_callback_user_data =*/ nullptr,

        /*.encoder_begin_callback           =*/ nullptr,
        /*.encoder_begin_callback_user_data =*/ nullptr,

        /*.logits_filter_callback           =*/ nullptr,
        /*.logits_filter_callback_user_data =*/ nullptr,
    };

    switch (strategy) {
        case WHISPER_SAMPLING_GREEDY:
            {
                result.greedy = {
                    /*.best_of   =*/ 1,
                };
            } break;
        case WHISPER_SAMPLING_BEAM_SEARCH:
            {
                result.beam_search = {
                    /*.beam_size =*/ 5,

                    /*.patience  =*/ -1.0f,
                };
            } break;
    }

    return result;
}

// forward declarations
static std::vector<float> get_signal_energy(const float * signal, int n_samples, int n_samples_per_half_window);
static void whisper_exp_compute_token_level_timestamps(
        struct whisper_context & ctx,
                           int   i_segment,
                         float   thold_pt,
                         float   thold_ptsum);

// trim from start (in place)
static inline void ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
}

// trim from end (in place)
static inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

// trim from both ends (in place)
static inline void trim(std::string &s) {
    rtrim(s);
    ltrim(s);
}

static inline bool should_split_on_word(const char * txt, bool split_on_word) {
    if (!split_on_word) return true;

    return txt[0] == ' ';
}

// wrap the last segment to max_len characters
// returns the number of new segments
static int whisper_wrap_segment(struct whisper_context & ctx, int max_len, bool split_on_word) {
    auto segment = ctx.result_all.back();

    int res = 1;
    int acc = 0;

    std::string text;

    for (int i = 0; i < (int) segment.tokens.size(); i++) {
        const auto & token = segment.tokens[i];
        if (token.id >= whisper_token_eot(&ctx)) {
            continue;
        }

        const auto txt = whisper_token_to_str(&ctx, token.id);
        const int cur = strlen(txt);

        if (acc + cur > max_len && i > 0 && should_split_on_word(txt, split_on_word)) {
            // split here
            if (split_on_word) {
                trim(text);
            }

            ctx.result_all.back().text = std::move(text);
            ctx.result_all.back().t1 = token.t0;
            ctx.result_all.back().tokens.resize(i);

            ctx.result_all.push_back({});
            ctx.result_all.back().t0 = token.t0;
            ctx.result_all.back().t1 = segment.t1;

            // add tokens [i, end] to the new segment
            ctx.result_all.back().tokens.insert(
                    ctx.result_all.back().tokens.end(),
                    segment.tokens.begin() + i,
                    segment.tokens.end());

            acc = 0;
            text = "";

            segment = ctx.result_all.back();
            i = -1;

            res++;
        } else {
            acc += cur;
            text += txt;
        }
    }

    if (split_on_word) {
        trim(text);
    }
    ctx.result_all.back().text = std::move(text);

    return res;
}

static const std::vector<std::string> non_speech_tokens = {
    "\"", "#", "(", ")", "*", "+", "/", ":", ";", "<", "=", ">", "@", "[", "\\", "]", "^",
    "_", "`", "{", "|", "}", "~", "「", "」", "『", "』", "<<", ">>", "<<<", ">>>", "--",
    "---", "-(", "-[", "('", "(\"", "((", "))", "(((", ")))", "[[", "]]", "{{", "}}", "♪♪",
    "♪♪♪","♩", "♪", "♫", "♬", "♭", "♮", "♯"
};

// process the logits for the selected decoder
// - applies logit filters
// - computes logprobs and probs
static void whisper_process_logits(
              struct whisper_context & ctx,
    const struct whisper_full_params   params,
              struct whisper_decoder & decoder,
                               float   temperature) {
    const auto & vocab      = ctx.vocab;
    const auto & tokens_cur = decoder.sequence.tokens;

    const bool is_initial = tokens_cur.size() == 0;
    const int  n_logits   = vocab.id_to_token.size();

    WHISPER_ASSERT(n_logits == ctx.vocab.n_vocab);

    // extract the logits for the last token
    // we will be mutating and therefore we don't want to use the ctx.logits buffer directly
    auto & probs    = decoder.probs;
    auto & logits   = decoder.logits;
    auto & logprobs = decoder.logprobs;
    {
        logits.resize(n_logits);
        memcpy(logits.data(), ctx.logits.data() + (ctx.logits.size() - n_logits), n_logits*sizeof(float));

        if (temperature > 0.0f) {
            for (int i = 0; i < n_logits; i++) {
                logits[i] /= temperature;
            }
        }

        // will be populated a bit later
        probs.resize(n_logits);
        logprobs.resize(n_logits);
    }

    // apply logit filters here
    // ref: https://github.com/openai/whisper/blob/0b1ba3d46ebf7fe6f953acfd8cad62a4f851b49f/whisper/decoding.py#L480-L493
    {
        // suppress blank
        // https://github.com/openai/whisper/blob/0b1ba3d46ebf7fe6f953acfd8cad62a4f851b49f/whisper/decoding.py#L388-L390
        if (params.suppress_blank) {
            if (is_initial) {
                logits[vocab.token_eot]           = -INFINITY;
                logits[vocab.token_to_id.at(" ")] = -INFINITY;
            }
        }

        // suppress <|notimestamps|> token
        // ref: https://github.com/openai/whisper/blob/0b1ba3d46ebf7fe6f953acfd8cad62a4f851b49f/whisper/decoding.py#L410-L412
        logits[vocab.token_not] = -INFINITY;

        // suppress sot and solm tokens
        logits[vocab.token_sot]  = -INFINITY;
        logits[vocab.token_solm] = -INFINITY;

        // suppress task tokens
        logits[vocab.token_translate]  = -INFINITY;
        logits[vocab.token_transcribe] = -INFINITY;

        if (params.logits_filter_callback) {
            params.logits_filter_callback(&ctx, tokens_cur.data(), tokens_cur.size(), logits.data(), params.logits_filter_callback_user_data);
        }

        // suppress non-speech tokens
        // ref: https://github.com/openai/whisper/blob/7858aa9c08d98f75575035ecd6481f462d66ca27/whisper/tokenizer.py#L224-L253
        if (params.suppress_non_speech_tokens) {
            for (const std::string & token : non_speech_tokens) {
                const std::string suppress_tokens[] = {token, " " + token};
                for (const std::string & suppress_token : suppress_tokens) {
                    if (vocab.token_to_id.find(suppress_token) != vocab.token_to_id.end()) {
                        logits[vocab.token_to_id.at(suppress_token)] = -INFINITY;
                    }
                }
            }

            // allow hyphens "-" and single quotes "'" between words, but not at the beginning of a word
            if (vocab.token_to_id.find(" -") != vocab.token_to_id.end()) {
                logits[vocab.token_to_id.at(" -")] = -INFINITY;
            }
            if (vocab.token_to_id.find(" '") != vocab.token_to_id.end()) {
                logits[vocab.token_to_id.at(" '")] = -INFINITY;
            }
        }

        // timestamps have to appear in pairs, except directly before EOT; mask logits accordingly
        // https://github.com/openai/whisper/blob/0b1ba3d46ebf7fe6f953acfd8cad62a4f851b49f/whisper/decoding.py#L414-L424
        {
            const bool last_was_timestamp        = tokens_cur.size() > 0 && tokens_cur.back().id >= vocab.token_beg;
            const bool penultimate_was_timestamp = tokens_cur.size() < 2 || tokens_cur[tokens_cur.size() - 2].id >= vocab.token_beg;

            //fprintf(stderr, "last_was_timestamp=%d penultimate_was_timestamp=%d\n", last_was_timestamp, penultimate_was_timestamp);

            if (last_was_timestamp) {
                if (penultimate_was_timestamp) {
                    for (int i = vocab.token_beg; i < n_logits; ++i) {
                        logits[i] = -INFINITY;
                    }
                } else {
                    for (int i = 0; i < vocab.token_eot; ++i) {
                        logits[i] = -INFINITY;
                    }
                }
            }
        }

        // the initial timestamp cannot be larger than max_initial_ts
        // ref: https://github.com/openai/whisper/blob/0b1ba3d46ebf7fe6f953acfd8cad62a4f851b49f/whisper/decoding.py#L426-L429
        if (is_initial && params.max_initial_ts > 0.0f) {
            const float precision = float(WHISPER_CHUNK_SIZE)/ctx.model.hparams.n_audio_ctx;
            const int   tid0      = std::round(params.max_initial_ts/precision);

            for (int i = vocab.token_beg + tid0 + 1; i < n_logits; ++i) {
                logits[i] = -INFINITY;
            }
        }

        // condition timestamp tokens to be increasing
        // ref: https://github.com/openai/whisper/pull/831#issuecomment-1385910556
        if (decoder.has_ts) {
            const int tid0 = decoder.seek_delta/2;

            for (int i = vocab.token_beg; i < vocab.token_beg + tid0; ++i) {
                logits[i] = -INFINITY;
            }
        }

        // populate the logprobs array (log_softmax)
        {
            const float logit_max = *std::max_element(logits.begin(), logits.end());
            float logsumexp = 0.0f;
            for (int i = 0; i < n_logits; ++i) {
                if (logits[i] > -INFINITY) {
                    logsumexp += expf(logits[i] - logit_max);
                }
            }
            logsumexp = logf(logsumexp) + logit_max;

            for (int i = 0; i < n_logits; ++i) {
                if (logits[i] > -INFINITY) {
                    logprobs[i] = logits[i] - logsumexp;
                } else {
                    logprobs[i] = -INFINITY;
                }
            }
        }

        // if sum of probability over timestamps is above any other token, sample timestamp
        // ref: https://github.com/openai/whisper/blob/0b1ba3d46ebf7fe6f953acfd8cad62a4f851b49f/whisper/decoding.py#L431-L437
        {
            // logsumexp over timestamps
            float timestamp_logprob = -INFINITY;
            {
                float logsumexp = 0.0f;
                const float logprob_max = *std::max_element(logprobs.begin() + vocab.token_beg, logprobs.end());
                for (int i = vocab.token_beg; i < n_logits; ++i) {
                    if (logprobs[i] > -INFINITY) {
                        logsumexp += expf(logprobs[i] - logprob_max);
                    }
                }
                if (logsumexp > 0.0f) {
                    timestamp_logprob = logf(logsumexp) + logprob_max;
                }
            }

            const float max_text_token_logprob = *std::max_element(logprobs.begin(), logprobs.begin() + vocab.token_beg);

            //fprintf(stderr, "timestamp_logprob=%f max_text_token_logprob=%f\n", timestamp_logprob, max_text_token_logprob);

            if (timestamp_logprob > max_text_token_logprob) {
                for (int i = 0; i < vocab.token_beg; ++i) {
                    logits[i]   = -INFINITY;
                    logprobs[i] = -INFINITY;
                }
            }
        }
    }

    // compute probs
    {
        for (int i = 0; i < n_logits; ++i) {
            if (logits[i] == -INFINITY) {
                probs[i] = 0.0f;
            } else {
                probs[i] = expf(logprobs[i]);
            }
        }
    }

#if 0
    // print first 100 logits - token string : logit
    for (int i = 0; i < 100; i++) {
        const auto token   = vocab.id_to_token.at(i);
        const auto prob    = probs[i];
        const auto logit   = logits[i];
        const auto logprob = logprobs[i];
        printf("%s : prob=%9.5f logit=%9.5f logprob=%9.5f\n", token.c_str(), prob, logit, logprob);
    }

    // "And", "and", " And", " and"
    printf("logits[\"and\"]  = %f\n", logits[vocab.token_to_id.at("and")]);
    printf("logits[\"And\"]  = %f\n", logits[vocab.token_to_id.at("And")]);
    printf("logits[\" and\"] = %f\n", logits[vocab.token_to_id.at(" and")]);
    printf("logits[\" And\"] = %f\n", logits[vocab.token_to_id.at(" And")]);
    printf("logits[\" so\"]  = %f\n", logits[vocab.token_to_id.at(" so")]);

    printf("logprobs[\"and\"]  = %f\n", logprobs[vocab.token_to_id.at("and")]);
    printf("logprobs[\"And\"]  = %f\n", logprobs[vocab.token_to_id.at("And")]);
    printf("logprobs[\" and\"] = %f\n", logprobs[vocab.token_to_id.at(" and")]);
    printf("logprobs[\" And\"] = %f\n", logprobs[vocab.token_to_id.at(" And")]);
    printf("logprobs[\" so\"]  = %f\n", logprobs[vocab.token_to_id.at(" so")]);

    printf("probs[\"and\"]  = %f\n", probs[vocab.token_to_id.at("and")]);
    printf("probs[\"And\"]  = %f\n", probs[vocab.token_to_id.at("And")]);
    printf("probs[\" and\"] = %f\n", probs[vocab.token_to_id.at(" and")]);
    printf("probs[\" And\"] = %f\n", probs[vocab.token_to_id.at(" And")]);
    printf("probs[\" so\"]  = %f\n", probs[vocab.token_to_id.at(" so")]);
#endif
}

static whisper_token_data whisper_sample_token(
            whisper_context & ctx,
      const whisper_decoder & decoder,
                       bool   best) {
    whisper_token_data result = {
        0, 0, 0.0f, 0.0f, 0.0f, 0.0f, -1, -1, 0.0f,
    };

    const auto & vocab = ctx.vocab;

    const auto & probs    = decoder.probs;
    const auto & logprobs = decoder.logprobs;

    const int n_logits = vocab.n_vocab;

    {
        double sum_ts = 0.0;
        double max_ts = 0.0;

        for (int i = vocab.token_beg; i < n_logits; i++) {
            if (probs[i] == -INFINITY) {
                continue;
            }

            sum_ts += probs[i];
            if (max_ts < probs[i]) {
                max_ts = probs[i];
                result.tid = i;
            }
        }

        result.pt    = max_ts/(sum_ts + 1e-10);
        result.ptsum = sum_ts;
    }

    if (best) {
        for (int i = 0; i < n_logits; ++i) {
            if (result.p < probs[i]) {
                result.id   = i;
                result.p    = probs[i];
                result.plog = logprobs[i];
            }
        }
    } else {
        std::discrete_distribution<> dist(probs.begin(), probs.end());

        result.id   = dist(ctx.rng);
        result.p    = probs[result.id];
        result.plog = logprobs[result.id];
    }

    if (result.id >= vocab.token_beg) {
        result.tid = result.id;
        result.pt  = result.p;
    }

    ctx.n_sample++;

    return result;
}

static std::vector<whisper_token_data> whisper_sample_token_topk(
            whisper_context & ctx,
      const whisper_decoder & decoder,
                        int   k) {
    const auto & vocab = ctx.vocab;

    const auto & probs    = decoder.probs;
    const auto & logits   = decoder.logits;
    const auto & logprobs = decoder.logprobs;

    const int n_logits = vocab.n_vocab;

    auto & logits_id = ctx.logits_id;

    logits_id.clear();
    for (int i = 0; i < n_logits; ++i) {
        logits_id.push_back({ logits[i], i });
    }

    std::partial_sort(
            logits_id.begin(),
            logits_id.begin() + k, logits_id.end(),
            [](const std::pair<double, whisper_token> & a, const std::pair<double, whisper_token> & b) {
                return a.first > b.first;
            });

    std::vector<whisper_token_data> result;
    result.reserve(k);

    whisper_token tid = vocab.token_beg;

    float pt    = 0.0;
    float ptsum = 0.0;

    {
        double sum_ts = 0.0;
        double max_ts = 0.0;

        for (int i = vocab.token_beg; i < n_logits; i++) {
            if (probs[i] == -INFINITY) {
                continue;
            }

            sum_ts += probs[i];
            if (max_ts < probs[i]) {
                max_ts = probs[i];
                tid = i;
            }
        }

        pt    = max_ts/(sum_ts + 1e-10);
        ptsum = sum_ts;
    }

    for (int i = 0; i < k; ++i) {
        const auto id = logits_id[i].second;

        result.push_back({ id, tid, probs[id], logprobs[id], pt, ptsum, -1, -1, 0.0f, });

        if (result[i].id >= vocab.token_beg) {
            result[i].tid = result[i].id;
            result[i].pt  = result[i].p;
        }
    }

    ctx.n_sample++;

    return result;
}

// ref: https://github.com/openai/whisper/blob/0b1ba3d46ebf7fe6f953acfd8cad62a4f851b49f/whisper/decoding.py#L178-L192
static void whisper_sequence_score(
        const struct whisper_full_params & params,
                        whisper_sequence & sequence) {
    if (sequence.result_len == 0) {
        return;
    }

    double result = 0.0f;

    for (int i = 0; i < sequence.result_len; ++i) {
        result += sequence.tokens[i].plog;
    }

    sequence.sum_logprobs = result;
    sequence.avg_logprobs = result/sequence.result_len;

    double penalty = sequence.result_len;

    if (params.length_penalty > 0.0f) {
        penalty = pow((5.0 + penalty)/6.0, params.length_penalty);
    }

    sequence.score = result/penalty;

    // compute the entropy of the sequence of the last 32 tokens
    {
        const int n = 32;

        int cnt = 0;
        double entropy = 0.0f;

        std::map<whisper_token, int> token_counts;
        for (int i = std::max(0, sequence.result_len - n); i < sequence.result_len; ++i) {
            token_counts[sequence.tokens[i].id]++;
            cnt++;
        }

        for (const auto & kv : token_counts) {
            const auto p = kv.second/(double)cnt;
            entropy -= p*log(p);

            //WHISPER_PRINT_DEBUG("entropy: %d %f %f, count %d\n", kv.first, p, log(p), kv.second);
        }

        sequence.entropy = entropy;
    }
}

int whisper_full(
        struct whisper_context * ctx,
        struct whisper_full_params params,
        const float * samples,
        int n_samples) {
    // clear old results
    auto & result_all = ctx->result_all;

    result_all.clear();

    // compute log mel spectrogram
    if (params.speed_up) {
        if (whisper_pcm_to_mel_phase_vocoder(ctx, samples, n_samples, params.n_threads) != 0) {
            fprintf(stderr, "%s: failed to compute log mel spectrogram\n", __func__);
            return -1;
        }
    } else {
        if (whisper_pcm_to_mel(ctx, samples, n_samples, params.n_threads) != 0) {
            fprintf(stderr, "%s: failed to compute log mel spectrogram\n", __func__);
            return -2;
        }
    }

    // auto-detect language if not specified
    if (params.language == nullptr || strlen(params.language) == 0 || strcmp(params.language, "auto") == 0) {
        std::vector<float> probs(whisper_lang_max_id() + 1, 0.0f);

        const auto lang_id = whisper_lang_auto_detect(ctx, 0, params.n_threads, probs.data());
        if (lang_id < 0) {
            fprintf(stderr, "%s: failed to auto-detect language\n", __func__);
            return -3;
        }
        ctx->lang_id = lang_id;
        params.language = whisper_lang_str(lang_id);

        fprintf(stderr, "%s: auto-detected language: %s (p = %f)\n", __func__, params.language, probs[whisper_lang_id(params.language)]);
    }

    if (params.token_timestamps) {
        ctx->t_beg    = 0;
        ctx->t_last   = 0;
        ctx->tid_last = 0;
        ctx->energy = get_signal_energy(samples, n_samples, 32);
    }

    const int seek_start = params.offset_ms/10;
    const int seek_end = seek_start + (params.duration_ms == 0 ? whisper_n_len(ctx) : params.duration_ms/10);

    // if length of spectrogram is less than 1s (100 samples), then return
    // basically don't process anything that is less than 1s
    // see issue #39: https://github.com/ggerganov/whisper.cpp/issues/39
    if (seek_end < seek_start + (params.speed_up ? 50 : 100)) {
        return 0;
    }

    // a set of temperatures to use
    // [ t0, t0 + delta, t0 + 2*delta, ..., < 1.0f + 1e-6f ]
    std::vector<float> temperatures;
    if (params.temperature_inc > 0.0f) {
        for (float t = params.temperature; t < 1.0f + 1e-6f; t += params.temperature_inc) {
            temperatures.push_back(t);
        }
    } else {
        temperatures.push_back(params.temperature);
    }

    // initialize the decoders
    int n_decoders = 1;

    switch (params.strategy) {
        case WHISPER_SAMPLING_GREEDY:
            {
                n_decoders = params.greedy.best_of;
            } break;
        case WHISPER_SAMPLING_BEAM_SEARCH:
            {
                n_decoders = std::max(params.greedy.best_of, params.beam_search.beam_size);
            } break;
    };

    n_decoders = std::max(1, n_decoders);

    // TAGS: WHISPER_DECODER_INIT
    for (int j = 1; j < n_decoders; j++) {
        auto & decoder = ctx->decoders[j];

        if (decoder.kv_self.ctx == nullptr) {
            decoder.kv_self = ctx->decoders[0].kv_self;
            if (!kv_cache_reinit(decoder.kv_self)) {
                fprintf(stderr, "%s: kv_cache_reinit() failed for self-attention, decoder %d\n", __func__, j);
                return -4;
            }

            WHISPER_PRINT_DEBUG("%s: initialized self-attention kv cache, decoder %d\n", __func__, j);

            decoder.sequence.tokens.reserve(ctx->decoders[0].sequence.tokens.capacity());

            decoder.probs.resize   (ctx->vocab.n_vocab);
            decoder.logits.resize  (ctx->vocab.n_vocab);
            decoder.logprobs.resize(ctx->vocab.n_vocab);
        }
    }

    // the accumulated text context so far
    auto & prompt_past = ctx->prompt_past;
    if (params.no_context) {
        prompt_past.clear();
    }

    // prepend the prompt tokens to the prompt_past
    if (params.prompt_tokens && params.prompt_n_tokens > 0) {
        // parse tokens from the pointer
        for (int i = 0; i < params.prompt_n_tokens; i++) {
            prompt_past.push_back(params.prompt_tokens[i]);
        }
        std::rotate(prompt_past.begin(), prompt_past.end() - params.prompt_n_tokens, prompt_past.end());
    }

    // overwrite audio_ctx, max allowed is hparams.n_audio_ctx
    if (params.audio_ctx > whisper_n_audio_ctx(ctx)) {
        fprintf(stderr, "%s: audio_ctx is larger than the maximum allowed (%d > %d)\n", __func__, params.audio_ctx, whisper_n_audio_ctx(ctx));
        return -5;
    }
    ctx->exp_n_audio_ctx = params.audio_ctx;

    // these tokens determine the task that will be performed
    std::vector<whisper_token> prompt_init = { whisper_token_sot(ctx) };
    if (whisper_is_multilingual(ctx)) {
        const int lang_id = whisper_lang_id(params.language);
        ctx->lang_id = lang_id;
        prompt_init.push_back(whisper_token_lang(ctx, lang_id));
        if (params.translate) {
            prompt_init.push_back(whisper_token_translate());
        } else {
            prompt_init.push_back(whisper_token_transcribe());
        }
    }

    int progress_prev = 0;
    int progress_step = 5;

    int seek = seek_start;

    std::vector<whisper_token> prompt;
    prompt.reserve(whisper_n_text_ctx(ctx));

    // beam-search helpers
    struct kv_buf {
        std::vector<uint8_t> k;
        std::vector<uint8_t> v;
    };

    std::vector<kv_buf> kv_bufs;

    struct beam_candidate {
        int decoder_idx;
        int seek_delta;

        bool has_ts;

        whisper_sequence sequence;
    };

    std::vector<beam_candidate> beam_candidates;

    // main loop
    while (true) {
        const int progress_cur = (100*(seek - seek_start))/(seek_end - seek_start);
        while (progress_cur >= progress_prev + progress_step) {
            progress_prev += progress_step;
            if (params.print_progress) {
                fprintf(stderr, "%s: progress = %3d%%\n", __func__, progress_prev);
            }
        }

        // of only 1 second left, then stop
        if (seek + 100 >= seek_end) {
            break;
        }

        if (params.encoder_begin_callback) {
            if (params.encoder_begin_callback(ctx, params.encoder_begin_callback_user_data) == false) {
                fprintf(stderr, "%s: encoder_begin_callback returned false - aborting\n", __func__);
                break;
            }
        }

        // encode audio features starting at offset seek
        if (!whisper_encode(*ctx, seek, params.n_threads)) {
            fprintf(stderr, "%s: failed to encode\n", __func__);
            return -6;
        }

        // if there is a very short audio segment left to process, we remove any past prompt since it tends
        // to confuse the decoder and often make it repeat or hallucinate stuff
        if (seek > seek_start && seek + 500 >= seek_end) {
            prompt_past.clear();
        }

        int best_decoder_id = 0;

        for (int it = 0; it < (int) temperatures.size(); ++it) {
            const float t_cur = temperatures[it];

            int n_decoders_cur = 1;

            switch (params.strategy) {
                case whisper_sampling_strategy::WHISPER_SAMPLING_GREEDY:
                    {
                        if (t_cur > 0.0f) {
                            n_decoders_cur = params.greedy.best_of;
                        }
                    } break;
                case whisper_sampling_strategy::WHISPER_SAMPLING_BEAM_SEARCH:
                    {
                        if (t_cur > 0.0f) {
                            n_decoders_cur = params.greedy.best_of;
                        } else {
                            n_decoders_cur = params.beam_search.beam_size;
                        }
                    } break;
            };

            n_decoders_cur = std::max(1, n_decoders_cur);

            WHISPER_PRINT_DEBUG("\n%s: decoding with %d decoders, temperature = %.2f\n", __func__, n_decoders_cur, t_cur);

            // TAGS: WHISPER_DECODER_INIT
            for (int j = 0; j < n_decoders_cur; ++j) {
                auto & decoder = ctx->decoders[j];

                decoder.kv_self.n = 0;

                decoder.sequence.tokens.clear();
                decoder.sequence.result_len       = 0;
                decoder.sequence.sum_logprobs_all = 0.0;
                decoder.sequence.sum_logprobs     = -INFINITY;
                decoder.sequence.avg_logprobs     = -INFINITY;
                decoder.sequence.entropy          = 0.0;
                decoder.sequence.score            = -INFINITY;

                decoder.seek_delta = 100*WHISPER_CHUNK_SIZE;

                decoder.failed    = false;
                decoder.completed = false;
                decoder.has_ts    = false;
            }

            // init prompt and kv cache for the current iteration
            // run whisper_decoder() only for decoder 0 and copy the results for the other decoders
            {
                prompt.clear();

                // if we have already generated some text, use it as a prompt to condition the next generation
                if (!prompt_past.empty() && t_cur < 0.5f && params.n_max_text_ctx > 0) {
                    int n_take = std::min(std::min(params.n_max_text_ctx, whisper_n_text_ctx(ctx)/2), int(prompt_past.size()));

                    prompt = { whisper_token_prev(ctx) };
                    prompt.insert(prompt.begin() + 1, prompt_past.end() - n_take, prompt_past.end());
                }

                // init new transcription with sot, language (opt) and task tokens
                prompt.insert(prompt.end(), prompt_init.begin(), prompt_init.end());

                // print the prompt
                WHISPER_PRINT_DEBUG("\n\n");
                for (int i = 0; i < (int) prompt.size(); i++) {
                    WHISPER_PRINT_DEBUG("%s: prompt[%d] = %s\n", __func__, i, ctx->vocab.id_to_token.at(prompt[i]).c_str());
                }
                WHISPER_PRINT_DEBUG("\n\n");

                if (!whisper_decode(*ctx, ctx->decoders[0], prompt.data(), prompt.size(), 0, params.n_threads)) {
                    fprintf(stderr, "%s: failed to decode\n", __func__);
                    return -7;
                }

                {
                    const int64_t t_start_sample_us = ggml_time_us();

                    whisper_process_logits(*ctx, params, ctx->decoders[0], t_cur);

                    ctx->decoders[0].kv_self.n += prompt.size();

                    for (int j = 1; j < n_decoders_cur; ++j) {
                        auto & decoder = ctx->decoders[j];

                        memcpy(decoder.kv_self.k->data, ctx->decoders[0].kv_self.k->data, ggml_nbytes(decoder.kv_self.k));
                        memcpy(decoder.kv_self.v->data, ctx->decoders[0].kv_self.v->data, ggml_nbytes(decoder.kv_self.v));

                        decoder.kv_self.n += prompt.size();

                        memcpy(decoder.probs.data(),    ctx->decoders[0].probs.data(),    decoder.probs.size()*sizeof(decoder.probs[0]));
                        memcpy(decoder.logits.data(),   ctx->decoders[0].logits.data(),   decoder.logits.size()*sizeof(decoder.logits[0]));
                        memcpy(decoder.logprobs.data(), ctx->decoders[0].logprobs.data(), decoder.logprobs.size()*sizeof(decoder.logprobs[0]));
                    }

                    ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
                }
            }

            for (int i = 0, n_max = whisper_n_text_ctx(ctx)/2 - 4; i < n_max; ++i) {
                const int64_t t_start_sample_us = ggml_time_us();

                // store the KV caches of all decoders when doing beam-search
                if (params.strategy == whisper_sampling_strategy::WHISPER_SAMPLING_BEAM_SEARCH) {
                    kv_bufs.resize(n_decoders_cur);
                    for (int j = 0; j < n_decoders_cur; ++j) {
                        auto & decoder = ctx->decoders[j];

                        if (decoder.completed || decoder.failed) {
                            continue;
                        }

                        kv_bufs[j].k.resize(ggml_nbytes(decoder.kv_self.k));
                        kv_bufs[j].v.resize(ggml_nbytes(decoder.kv_self.v));

                        memcpy(kv_bufs[j].k.data(), decoder.kv_self.k->data, kv_bufs[j].k.size());
                        memcpy(kv_bufs[j].v.data(), decoder.kv_self.v->data, kv_bufs[j].v.size());
                    }

                    beam_candidates.clear();
                }

                // generate new sequence candidates for each decoder
                for (int j = 0; j < n_decoders_cur; ++j) {
                    auto & decoder = ctx->decoders[j];

                    if (decoder.completed || decoder.failed) {
                        continue;
                    }

                    switch (params.strategy) {
                        case whisper_sampling_strategy::WHISPER_SAMPLING_GREEDY:
                            {
                                if (t_cur < 1e-6f) {
                                    decoder.sequence.tokens.push_back(whisper_sample_token(*ctx, decoder, true));
                                } else {
                                    decoder.sequence.tokens.push_back(whisper_sample_token(*ctx, decoder, false));
                                }

                                decoder.sequence.sum_logprobs_all += decoder.sequence.tokens.back().plog;
                            } break;
                        case whisper_sampling_strategy::WHISPER_SAMPLING_BEAM_SEARCH:
                            {
                                const auto tokens_new = whisper_sample_token_topk(*ctx, decoder, params.beam_search.beam_size);

                                for (const auto & token : tokens_new) {
                                    beam_candidates.push_back({ j, decoder.seek_delta, decoder.has_ts, decoder.sequence });
                                    beam_candidates.back().sequence.tokens.push_back(token);
                                    beam_candidates.back().sequence.sum_logprobs_all += token.plog;

                                    //WHISPER_PRINT_DEBUG("%s: beam candidate: %s (%f, %f)\n", __func__, ctx->vocab.id_to_token.at(token.id).c_str(), token.plog, beam_candidates.back().sequence.sum_logprobs_all);
                                }
                            } break;
                    };
                }

                // for beam-search, choose the top candidates and update the KV caches
                if (params.strategy == whisper_sampling_strategy::WHISPER_SAMPLING_BEAM_SEARCH) {
                    std::sort(
                            beam_candidates.begin(),
                            beam_candidates.end(),
                            [](const beam_candidate & a, const beam_candidate & b) {
                        return a.sequence.sum_logprobs_all > b.sequence.sum_logprobs_all;
                    });

                    uint32_t cur_c = 0;

                    for (int j = 0; j < n_decoders_cur; ++j) {
                        auto & decoder = ctx->decoders[j];

                        if (decoder.completed || decoder.failed) {
                            continue;
                        }

                        auto & cur = beam_candidates[cur_c++];

                        while (beam_candidates.size() > cur_c && beam_candidates[cur_c].sequence.sum_logprobs_all == cur.sequence.sum_logprobs_all && i > 0) {
                            ++cur_c;
                        }

                        decoder.sequence   = cur.sequence;
                        decoder.seek_delta = cur.seek_delta;
                        decoder.has_ts     = cur.has_ts;

                        memcpy(decoder.kv_self.k->data, kv_bufs[cur.decoder_idx].k.data(), kv_bufs[cur.decoder_idx].k.size());
                        memcpy(decoder.kv_self.v->data, kv_bufs[cur.decoder_idx].v.data(), kv_bufs[cur.decoder_idx].v.size());

                        WHISPER_PRINT_DEBUG("%s: beam search: decoder %d: from decoder %d: token = %10s, plog = %8.5f, sum_logprobs = %8.5f\n",
                                __func__, j, cur.decoder_idx, ctx->vocab.id_to_token.at(decoder.sequence.tokens.back().id).c_str(), decoder.sequence.tokens.back().plog, decoder.sequence.sum_logprobs_all);
                    }
                }

                // update the decoder state
                // - check if the sequence is completed
                // - check if the sequence is failed
                // - update sliding window based on timestamp tokens
                for (int j = 0; j < n_decoders_cur; ++j) {
                    auto & decoder = ctx->decoders[j];

                    if (decoder.completed || decoder.failed) {
                        continue;
                    }

                    auto & has_ts     = decoder.has_ts;
                    auto & failed     = decoder.failed;
                    auto & completed  = decoder.completed;
                    auto & seek_delta = decoder.seek_delta;
                    auto & result_len = decoder.sequence.result_len;

                    {
                        const auto & token = decoder.sequence.tokens.back();

                        // timestamp token - update sliding window
                        if (token.id > whisper_token_beg(ctx)) {
                            const int seek_delta_new = 2*(token.id - whisper_token_beg(ctx));

                            // do not allow to go back in time
                            if (has_ts && seek_delta > seek_delta_new && result_len < i) {
                                failed = true; // TODO: maybe this is not a failure ?
                                continue;
                            }

                            seek_delta = seek_delta_new;
                            result_len = i + 1;
                            has_ts = true;
                        }

#ifdef WHISPER_DEBUG
                        {
                            const auto tt = token.pt > 0.10 ? ctx->vocab.id_to_token.at(token.tid) : "[?]";
                            WHISPER_PRINT_DEBUG("%s: id = %3d, decoder = %d, token = %6d, p = %6.3f, ts = %10s, %6.3f, result_len = %4d '%s'\n",
                                    __func__, i, j, token.id, token.p, tt.c_str(), token.pt, result_len, ctx->vocab.id_to_token.at(token.id).c_str());
                        }
#endif

                        // end of segment
                        if (token.id == whisper_token_eot(ctx) ||               // end of text token
                           (params.max_tokens > 0 && i >= params.max_tokens) || // max tokens per segment reached
                           (has_ts && seek + seek_delta + 100 >= seek_end)      // end of audio reached
                           ) {
                            if (result_len == 0) {
                                if (seek + seek_delta + 100 >= seek_end) {
                                    result_len = i + 1;
                                } else {
                                    failed = true;
                                    continue;
                                }
                            }

                            if (params.single_segment) {
                                result_len = i + 1;
                                seek_delta = 100*WHISPER_CHUNK_SIZE;
                            }

                            completed = true;
                            continue;
                        }

                        // TESTS: if no tensors are loaded, it means we are running tests
                        if (ctx->model.n_loaded == 0) {
                            seek_delta = 100*WHISPER_CHUNK_SIZE;
                            completed = true;
                            continue;
                        }
                    }

                    // sometimes, the decoding can get stuck in a repetition loop
                    // this is an attempt to mitigate such cases - we flag the decoding as failed and use a fallback strategy
                    if (i == n_max - 1 && (result_len == 0 || seek_delta < 100*WHISPER_CHUNK_SIZE/2)) {
                        failed = true;
                        continue;
                    }
                }

                // check if all decoders have finished (i.e. completed or failed)
                {
                    bool completed_all = true;

                    for (int j = 0; j < n_decoders_cur; ++j) {
                        auto & decoder = ctx->decoders[j];

                        if (decoder.completed || decoder.failed) {
                            continue;
                        }

                        completed_all = false;
                    }

                    if (completed_all) {
                        break;
                    }
                }

                ctx->t_sample_us += ggml_time_us() - t_start_sample_us;

                // obtain logits for the next token
                for (int j = 0; j < n_decoders_cur; ++j) {
                    auto & decoder = ctx->decoders[j];

                    if (decoder.failed || decoder.completed) {
                        continue;
                    }

                    decoder.tokens_tmp.resize(1);
                    decoder.tokens_tmp[0] = decoder.sequence.tokens.back().id;

                    //WHISPER_PRINT_DEBUG("%s: decoder %d: token %d, kv_self.n %d, seek_delta %d\n", __func__, j, decoder.tokens_tmp[0], decoder.kv_self.n, decoder.seek_delta);

                    if (!whisper_decode(*ctx, decoder, decoder.tokens_tmp.data(), decoder.tokens_tmp.size(), decoder.kv_self.n, params.n_threads)) {
                        fprintf(stderr, "%s: failed to decode\n", __func__);
                        return -8;
                    }

                    {
                        const int64_t t_start_sample_us = ggml_time_us();

                        whisper_process_logits(*ctx, params, decoder, t_cur);

                        ++decoder.kv_self.n;

                        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
                    }
                }
            }

            // rank the resulting sequences and select the best one
            {
                double best_score = -INFINITY;

                for (int j = 0; j < n_decoders_cur; ++j) {
                    auto & decoder = ctx->decoders[j];

                    if (decoder.failed) {
                        continue;
                    }

                    decoder.sequence.tokens.resize(decoder.sequence.result_len);
                    whisper_sequence_score(params, decoder.sequence);

                    WHISPER_PRINT_DEBUG("%s: decoder %2d: score = %8.5f, result_len = %3d, avg_logprobs = %8.5f, entropy = %8.5f\n",
                            __func__, j, decoder.sequence.score, decoder.sequence.result_len, decoder.sequence.avg_logprobs, decoder.sequence.entropy);

                    if (decoder.sequence.result_len > 32 && decoder.sequence.entropy < params.entropy_thold) {
                        WHISPER_PRINT_DEBUG("%s: decoder %2d: failed due to entropy %8.5f < %8.5f\n",
                                __func__, j, decoder.sequence.entropy, params.entropy_thold);

                        decoder.failed = true;
                        ctx->n_fail_h++;

                        continue;
                    }

                    if (best_score < decoder.sequence.score) {
                        best_score = decoder.sequence.score;
                        best_decoder_id = j;
                    }
                }

                WHISPER_PRINT_DEBUG("%s: best decoder = %d\n", __func__, best_decoder_id);
            }

            // was the decoding successful for the current temperature?
            {
                bool success = true;

                const auto & decoder = ctx->decoders[best_decoder_id];

                if (decoder.failed || decoder.sequence.avg_logprobs < params.logprob_thold) {
                    success = false;
                    ctx->n_fail_p++;
                }

                if (success) {
                    //for (auto & token : ctx->decoders[best_decoder_id].sequence.tokens) {
                    //    WHISPER_PRINT_DEBUG("%s: token = %d, p = %6.3f, pt = %6.3f, ts = %s, str = %s\n", __func__, token.id, token.p, token.pt, ctx->vocab.id_to_token.at(token.tid).c_str(), ctx->vocab.id_to_token.at(token.id).c_str());
                    //}

                    break;
                }
            }

            WHISPER_PRINT_DEBUG("\n%s: failed to decode with temperature = %.2f\n", __func__, t_cur);
        }

        // output results through a user-provided callback
        {
            const auto & best_decoder = ctx->decoders[best_decoder_id];

            const auto seek_delta = best_decoder.seek_delta;
            const auto result_len = best_decoder.sequence.result_len;

            const auto & tokens_cur = best_decoder.sequence.tokens;

            //WHISPER_PRINT_DEBUG("prompt_init.size() = %d, prompt.size() = %d, result_len = %d, seek_delta = %d\n", prompt_init.size(), prompt.size(), result_len, seek_delta);

            // update prompt_past
            prompt_past.clear();
            if (prompt.front() == whisper_token_prev(ctx)) {
                prompt_past.insert(prompt_past.end(), prompt.begin() + 1, prompt.end() - prompt_init.size());
            }

            for (int i = 0; i < result_len; ++i) {
                prompt_past.push_back(tokens_cur[i].id);
            }

            // store the text from this iteration
            if (!tokens_cur.empty() && ctx->model.n_loaded > 0) {
                int  i0 = 0;
                auto t0 = seek + 2*(tokens_cur.front().tid - whisper_token_beg(ctx));

                std::string text;

                for (int i = 0; i < (int) tokens_cur.size(); i++) {
                    //printf("%s: %18s %6.3f %18s %6.3f\n", __func__,
                    //        ctx->vocab.id_to_token[tokens_cur[i].id].c_str(), tokens_cur[i].p,
                    //        ctx->vocab.id_to_token[tokens_cur[i].tid].c_str(), tokens_cur[i].pt);

                    if (params.print_special == false && tokens_cur[i].id >= whisper_token_eot(ctx)) {
                    } else {
                        text += whisper_token_to_str(ctx, tokens_cur[i].id);
                    }

                    if (tokens_cur[i].id > whisper_token_beg(ctx) && !params.single_segment) {
                        const auto t1 = seek + 2*(tokens_cur[i].tid - whisper_token_beg(ctx));

                        if (!text.empty()) {
                            const auto tt0 = params.speed_up ? 2*t0 : t0;
                            const auto tt1 = params.speed_up ? 2*t1 : t1;

                            if (params.print_realtime) {
                                if (params.print_timestamps) {
                                    printf("[%s --> %s]  %s\n", to_timestamp(tt0).c_str(), to_timestamp(tt1).c_str(), text.c_str());
                                } else {
                                    printf("%s", text.c_str());
                                    fflush(stdout);
                                }
                            }

                            //printf("tt0 = %d, tt1 = %d, text = %s, token = %s, token_id = %d, tid = %d\n", tt0, tt1, text.c_str(), ctx->vocab.id_to_token[tokens_cur[i].id].c_str(), tokens_cur[i].id, tokens_cur[i].tid);

                            result_all.push_back({ tt0, tt1, text, {} });
                            for (int j = i0; j <= i; j++) {
                                result_all.back().tokens.push_back(tokens_cur[j]);
                            }

                            int n_new = 1;

                            if (params.token_timestamps) {
                                whisper_exp_compute_token_level_timestamps(
                                        *ctx, result_all.size() - 1, params.thold_pt, params.thold_ptsum);

                                if (params.max_len > 0) {
                                    n_new = whisper_wrap_segment(*ctx, params.max_len, params.split_on_word);
                                }
                            }
                            if (params.new_segment_callback) {
                                params.new_segment_callback(ctx, n_new, params.new_segment_callback_user_data);
                            }
                        }
                        text = "";
                        while (i < (int) tokens_cur.size() && tokens_cur[i].id > whisper_token_beg(ctx)) {
                            i++;
                        }
                        i--;
                        t0 = t1;
                        i0 = i + 1;
                    }
                }

                if (!text.empty()) {
                    const auto t1 = seek + seek_delta;

                    const auto tt0 = params.speed_up ? 2*t0 : t0;
                    const auto tt1 = params.speed_up ? 2*t1 : t1;

                    if (params.print_realtime) {
                        if (params.print_timestamps) {
                            printf("[%s --> %s]  %s\n", to_timestamp(tt0).c_str(), to_timestamp(tt1).c_str(), text.c_str());
                        } else {
                            printf("%s", text.c_str());
                            fflush(stdout);
                        }
                    }

                    result_all.push_back({ tt0, tt1, text, {} });
                    for (int j = i0; j < (int) tokens_cur.size(); j++) {
                        result_all.back().tokens.push_back(tokens_cur[j]);
                    }

                    int n_new = 1;

                    if (params.token_timestamps) {
                        whisper_exp_compute_token_level_timestamps(
                                *ctx, result_all.size() - 1, params.thold_pt, params.thold_ptsum);

                        if (params.max_len > 0) {
                            n_new = whisper_wrap_segment(*ctx, params.max_len, params.split_on_word);
                        }
                    }
                    if (params.new_segment_callback) {
                        params.new_segment_callback(ctx, n_new, params.new_segment_callback_user_data);
                    }
                }
            }

            // update audio window
            seek += seek_delta;

            WHISPER_PRINT_DEBUG("seek = %d, seek_delta = %d\n", seek, seek_delta);
        }
    }

    return 0;
}

int whisper_full_parallel(
        struct whisper_context * ctx,
        struct whisper_full_params params,
        const float * samples,
        int n_samples,
        int n_processors) {
    if (n_processors == 1) {
        return whisper_full(ctx, params, samples, n_samples);
    }

    int ret = 0;

    // prepare separate contexts for each thread
    std::vector<struct whisper_context> ctxs(n_processors - 1);

    for (int i = 0; i < n_processors - 1; ++i) {
        auto & ctx_p = ctxs[i];

        ctx_p = *ctx;

        ctx_p.logits.reserve(ctx_p.vocab.n_vocab*ctx_p.model.hparams.n_text_ctx);

        ctx_p.logits_id.reserve(ctx_p.vocab.n_vocab);

        if (!kv_cache_reinit(ctx_p.kv_cross)) {
            fprintf(stderr, "%s: kv_cache_reinit() failed for cross-attention, processor %d\n", __func__, i);
            return false;
        }

        // TAGS: WHISPER_DECODER_INIT
        for (int j = 0; j < WHISPER_MAX_DECODERS; ++j) {
            if (ctx_p.decoders[j].kv_self.ctx && !kv_cache_reinit(ctx_p.decoders[j].kv_self)) {
                fprintf(stderr, "%s: kv_cache_reinit() failed for self-attention, decoder %d, processor %d\n", __func__, j, i);
                return false;
            }

            ctx_p.decoders[j].sequence.tokens.reserve(ctx_p.model.hparams.n_text_ctx);

            ctx_p.decoders[j].probs.reserve   (ctx_p.vocab.n_vocab);
            ctx_p.decoders[j].logits.reserve  (ctx_p.vocab.n_vocab);
            ctx_p.decoders[j].logprobs.reserve(ctx_p.vocab.n_vocab);
        }
    }

    const int offset_samples = (WHISPER_SAMPLE_RATE*params.offset_ms)/1000;
    const int n_samples_per_processor = (n_samples - offset_samples)/n_processors;

    // the calling thread will process the first chunk
    // while the other threads will process the remaining chunks

    std::vector<std::thread> workers(n_processors - 1);
    for (int i = 0; i < n_processors - 1; ++i) {
        const int start_samples = offset_samples + (i + 1)*n_samples_per_processor;
        const int n_samples_cur = (i == n_processors - 2) ? n_samples - start_samples : n_samples_per_processor;

        auto params_cur = params;

        params_cur.offset_ms = 0;
        params_cur.print_progress = false;
        params_cur.print_realtime = false;

        params_cur.new_segment_callback = nullptr;
        params_cur.new_segment_callback_user_data = nullptr;

        workers[i] = std::thread(whisper_full, &ctxs[i], std::move(params_cur), samples + start_samples, n_samples_cur);
    }

    {
        auto params_cur = params;

        ret = whisper_full(ctx, std::move(params_cur), samples, offset_samples + n_samples_per_processor);
    }

    for (int i = 0; i < n_processors - 1; ++i) {
        workers[i].join();
    }

    const int64_t offset_t = (int64_t) params.offset_ms/10.0;

    // combine results into ctx->result_all
    for (int i = 0; i < n_processors - 1; ++i) {
        auto & results_i = ctxs[i].result_all;

        for (auto & result : results_i) {
            // correct the segment timestamp taking into account the offset
            result.t0 += 100*((i + 1)*n_samples_per_processor)/WHISPER_SAMPLE_RATE + offset_t;
            result.t1 += 100*((i + 1)*n_samples_per_processor)/WHISPER_SAMPLE_RATE + offset_t;

            // make sure that segments are not overlapping
            if (!ctx->result_all.empty()) {
                result.t0 = std::max(result.t0, ctx->result_all.back().t1);
            }

            ctx->result_all.push_back(std::move(result));

            // call the new_segment_callback for each segment
            if (params.new_segment_callback) {
                params.new_segment_callback(ctx, 1, params.new_segment_callback_user_data);
            }
        }

        ctx->t_mel_us    += ctxs[i].t_mel_us;
        ctx->t_sample_us += ctxs[i].t_sample_us;
        ctx->t_encode_us += ctxs[i].t_encode_us;
        ctx->t_decode_us += ctxs[i].t_decode_us;

        kv_cache_free(ctx->kv_cross);

        for (int j = 0; j < WHISPER_MAX_DECODERS; ++j) {
            kv_cache_free(ctx->decoders[j].kv_self);
        }
    }

    // average the timings
    ctx->t_mel_us    /= n_processors;
    ctx->t_sample_us /= n_processors;
    ctx->t_encode_us /= n_processors;
    ctx->t_decode_us /= n_processors;

    // print information about the audio boundaries
    fprintf(stderr, "\n");
    fprintf(stderr, "%s: the audio has been split into %d chunks at the following times:\n", __func__, n_processors);
    for (int i = 0; i < n_processors - 1; ++i) {
        fprintf(stderr, "%s: split %d - %s\n", __func__, (i + 1), to_timestamp(100*((i + 1)*n_samples_per_processor)/WHISPER_SAMPLE_RATE + offset_t).c_str());
    }
    fprintf(stderr, "%s: the transcription quality may be degraded near these boundaries\n", __func__);

    return ret;
}

int whisper_full_n_segments(struct whisper_context * ctx) {
    return ctx->result_all.size();
}

int whisper_full_lang_id(struct whisper_context * ctx) {
    return ctx->lang_id;
}

int64_t whisper_full_get_segment_t0(struct whisper_context * ctx, int i_segment) {
    return ctx->result_all[i_segment].t0;
}

int64_t whisper_full_get_segment_t1(struct whisper_context * ctx, int i_segment) {
    return ctx->result_all[i_segment].t1;
}

const char * whisper_full_get_segment_text(struct whisper_context * ctx, int i_segment) {
    return ctx->result_all[i_segment].text.c_str();
}

int whisper_full_n_tokens(struct whisper_context * ctx, int i_segment) {
    return ctx->result_all[i_segment].tokens.size();
}

const char * whisper_full_get_token_text(struct whisper_context * ctx, int i_segment, int i_token) {
    return ctx->vocab.id_to_token[ctx->result_all[i_segment].tokens[i_token].id].c_str();
}

whisper_token whisper_full_get_token_id(struct whisper_context * ctx, int i_segment, int i_token) {
    return ctx->result_all[i_segment].tokens[i_token].id;
}

struct whisper_token_data whisper_full_get_token_data(struct whisper_context * ctx, int i_segment, int i_token) {
    return ctx->result_all[i_segment].tokens[i_token];
}

float whisper_full_get_token_p(struct whisper_context * ctx, int i_segment, int i_token) {
    return ctx->result_all[i_segment].tokens[i_token].p;
}

// =================================================================================================

//
// Temporary interface needed for exposing ggml interface
// Will be removed in the future when ggml becomes a separate library
//

WHISPER_API int whisper_bench_memcpy(int n_threads) {
    ggml_time_init();

    size_t n    = 50;
    size_t arr  = n_threads > 0 ? 1024 : n_threads; // trick to avoid compiler optimizations

    // 1 GB array
    const size_t size = arr*1024llu*1024llu;

    char * src = (char *) malloc(size);
    char * dst = (char *) malloc(size);

    for (size_t i = 0; i < size; i++) src[i] = i;

    memcpy(dst, src, size); // heat-up

    double tsum = 0.0;

    for (size_t i = 0; i < n; i++) {
        const int64_t t0 = ggml_time_us();

        memcpy(dst, src, size);

        const int64_t t1 = ggml_time_us();

        tsum += (t1 - t0)*1e-6;

        src[0] = rand();
    }

    fprintf(stderr, "memcpy: %.2f GB/s\n", (double) (n*size)/(tsum*1024llu*1024llu*1024llu));

    // needed to prevent the compile from optimizing the memcpy away
    {
        double sum = 0.0;

        for (size_t i = 0; i < size; i++) sum += dst[i];

        fprintf(stderr, "sum:    %s %f\n", sum == -536870910.00 ? "ok" : "error", sum);
    }

    free(src);
    free(dst);

    return 0;
}

WHISPER_API int whisper_bench_ggml_mul_mat(int n_threads) {
    ggml_time_init();

    const int n_max = 128;

    const std::vector<size_t> sizes = {
        64, 128, 256, 512, 1024, 2048, 4096,
    };

    const size_t N_max = sizes.back();

    // a: N*N*sizeof(float)
    // b: N*N*sizeof(float)
    // c: N*N*sizeof(float)
    // when F16 is used, there is an extra work buffer of size N*N*sizeof(float)
    std::vector<char> buf(4llu*N_max*N_max*sizeof(float) + 4*256);

    for (size_t i = 0; i < buf.size(); i++) buf[i] = i;

    for (int j = 0; j < (int) sizes.size(); j++) {
        int n_fp16 = 0;
        int n_fp32 = 0;

        // GFLOPS/s
        double s_fp16 = 0.0;
        double s_fp32 = 0.0;

        const size_t N = sizes[j];

        for (int k = 0; k < 2; ++k) {
            const ggml_type wtype = k == 0 ? GGML_TYPE_F16 : GGML_TYPE_F32;

            double & s = k == 0 ? s_fp16 : s_fp32;
            int    & n = k == 0 ? n_fp16   : n_fp32;

            struct ggml_init_params gparams = {
                /*.mem_size   =*/ buf.size(),
                /*.mem_buffer =*/ buf.data(),
            };

            struct ggml_context * ctx0 = ggml_init(gparams);

            struct ggml_tensor * a = ggml_new_tensor_2d(ctx0, wtype,         N, N);
            struct ggml_tensor * b = ggml_new_tensor_2d(ctx0, GGML_TYPE_F32, N, N);

            struct ggml_tensor * c = ggml_mul_mat(ctx0, a, b);

            struct ggml_cgraph gf = ggml_build_forward(c);

            gf.n_threads = n_threads;

            double tsum = 0.0;

            // heat-up
            ggml_graph_compute(ctx0, &gf);

            for (int i = 0; i < n_max; ++i) {
                const int64_t t0 = ggml_time_us();

                ggml_graph_compute(ctx0, &gf);

                const int64_t t1 = ggml_time_us();

                tsum += (t1 - t0)*1e-6;
                n++;

                if (tsum > 1.0 && n >= 3) {
                    break;
                }
            }

            ggml_free(ctx0);

            s = ((2.0*N*N*N*n)/tsum)*1e-9;
        }

        fprintf(stderr, "ggml_mul_mat: %5zu x %5zu: F16 %8.1f GFLOPS (%3d runs) / F32 %8.1f GFLOPS (%3d runs)\n",
            N, N, s_fp16, n_fp16, s_fp32, n_fp32);
    }

    return 0;
}

// =================================================================================================

// =================================================================================================

//
// Experimental stuff below
//
// Not sure if these should be part of the library at all, because the quality of the results is not
// guaranteed. Might get removed at some point unless a robust algorithm implementation is found
//

// =================================================================================================

//
// token-level timestamps
//

static int timestamp_to_sample(int64_t t, int n_samples) {
    return std::max(0, std::min((int) n_samples - 1, (int) ((t*WHISPER_SAMPLE_RATE)/100)));
}

static int64_t sample_to_timestamp(int i_sample) {
    return (100ll*i_sample)/WHISPER_SAMPLE_RATE;
}

// a cost-function / heuristic that is high for text that takes longer to pronounce
// obviously, can be improved
static float voice_length(const std::string & text) {
    float res = 0.0f;

    for (char c : text) {
        if (c == ' ') {
            res += 0.01f;
        } else if (c == ',') {
            res += 2.00f;
        } else if (c == '.') {
            res += 3.00f;
        } else if (c == '!') {
            res += 3.00f;
        } else if (c == '?') {
            res += 3.00f;
        } else if (c >= '0' && c <= '9') {
            res += 3.00f;
        } else {
            res += 1.00f;
        }
    }

    return res;
}

// average the fabs of the signal
static std::vector<float> get_signal_energy(const float * signal, int n_samples, int n_samples_per_half_window) {
    const int hw = n_samples_per_half_window;

    std::vector<float> result(n_samples);

    for (int i = 0; i < n_samples; i++) {
        float sum = 0;
        for (int j = -hw; j <= hw; j++) {
            if (i + j >= 0 && i + j < n_samples) {
                sum += fabs(signal[i + j]);
            }
        }
        result[i] = sum/(2*hw + 1);
    }

    return result;
}

static void whisper_exp_compute_token_level_timestamps(
        struct whisper_context & ctx,
                           int   i_segment,
                         float   thold_pt,
                         float   thold_ptsum) {
    auto & segment = ctx.result_all[i_segment];
    auto & tokens  = segment.tokens;

    const int n_samples = ctx.energy.size();

    if (n_samples == 0) {
        fprintf(stderr, "%s: no signal data available\n", __func__);
        return;
    }

    const int64_t t0 = segment.t0;
    const int64_t t1 = segment.t1;

    const int n = tokens.size();

    if (n == 0) {
        return;
    }

    if (n == 1) {
        tokens[0].t0 = t0;
        tokens[0].t1 = t1;

        return;
    }

    auto & t_beg    = ctx.t_beg;
    auto & t_last   = ctx.t_last;
    auto & tid_last = ctx.tid_last;

    for (int j = 0; j < n; ++j) {
        auto & token = tokens[j];

        if (j == 0) {
            if (token.id == whisper_token_beg(&ctx)) {
                tokens[j    ].t0 = t0;
                tokens[j    ].t1 = t0;
                tokens[j + 1].t0 = t0;

                t_beg    = t0;
                t_last   = t0;
                tid_last = whisper_token_beg(&ctx);
            } else {
                tokens[j    ].t0 = t_last;
            }
        }

        const int64_t tt = t_beg + 2*(token.tid - whisper_token_beg(&ctx));

        tokens[j].id    = token.id;
        tokens[j].tid   = token.tid;
        tokens[j].p     = token.p;
        tokens[j].pt    = token.pt;
        tokens[j].ptsum = token.ptsum;

        tokens[j].vlen = voice_length(whisper_token_to_str(&ctx, token.id));

        if (token.pt > thold_pt && token.ptsum > thold_ptsum && token.tid > tid_last && tt <= t1) {
            if (j > 0) {
                tokens[j - 1].t1 = tt;
            }
            tokens[j].t0 = tt;
            tid_last = token.tid;
        }
    }

    tokens[n - 2].t1 = t1;
    tokens[n - 1].t0 = t1;
    tokens[n - 1].t1 = t1;

    t_last = t1;

    // find intervals of tokens with unknown timestamps
    // fill the timestamps by proportionally splitting the interval based on the token voice lengths
    {
        int p0 = 0;
        int p1 = 0;

        while (true) {
            while (p1 < n && tokens[p1].t1 < 0) {
                p1++;
            }

            if (p1 >= n) {
                p1--;
            }

            //printf("p0=%d p1=%d t0=%lld t1=%lld\n", p0, p1, tokens[p0].t0, tokens[p1].t1);

            if (p1 > p0) {
                double psum = 0.0;
                for (int j = p0; j <= p1; j++) {
                    psum += tokens[j].vlen;
                }

                //printf("analyzing %d - %d, psum = %f\n", p0, p1, psum);

                const double dt = tokens[p1].t1 - tokens[p0].t0;

                // split the time proportionally to the voice length
                for (int j = p0 + 1; j <= p1; j++) {
                    const double ct = tokens[j - 1].t0 + dt*tokens[j - 1].vlen/psum;

                    tokens[j - 1].t1 = ct;
                    tokens[j    ].t0 = ct;
                }
            }

            p1++;
            p0 = p1;
            if (p1 >= n) {
                break;
            }
        }
    }

    // fix up (just in case)
    for (int j = 0; j < n - 1; j++) {
        if (tokens[j].t1 < 0) {
            tokens[j + 1].t0 = tokens[j].t1;
        }

        if (j > 0) {
            if (tokens[j - 1].t1 > tokens[j].t0) {
                tokens[j].t0 = tokens[j - 1].t1;
                tokens[j].t1 = std::max(tokens[j].t0, tokens[j].t1);
            }
        }
    }

    // VAD
    // expand or contract tokens based on voice activity
    {
        const int hw = WHISPER_SAMPLE_RATE/8;

        for (int j = 0; j < n; j++) {
            if (tokens[j].id >= whisper_token_eot(&ctx)) {
                continue;
            }

            int s0 = timestamp_to_sample(tokens[j].t0, n_samples);
            int s1 = timestamp_to_sample(tokens[j].t1, n_samples);

            const int ss0 = std::max(s0 - hw, 0);
            const int ss1 = std::min(s1 + hw, n_samples);

            const int ns = ss1 - ss0;

            float sum = 0.0f;

            for (int k = ss0; k < ss1; k++) {
                sum += ctx.energy[k];
            }

            const float thold = 0.5*sum/ns;

            {
                int k = s0;
                if (ctx.energy[k] > thold && j > 0) {
                    while (k > 0 && ctx.energy[k] > thold) {
                        k--;
                    }
                    tokens[j].t0 = sample_to_timestamp(k);
                    if (tokens[j].t0 < tokens[j - 1].t1) {
                        tokens[j].t0 = tokens[j - 1].t1;
                    } else {
                        s0 = k;
                    }
                } else {
                    while (ctx.energy[k] < thold && k < s1) {
                        k++;
                    }
                    s0 = k;
                    tokens[j].t0 = sample_to_timestamp(k);
                }
            }

            {
                int k = s1;
                if (ctx.energy[k] > thold) {
                    while (k < n_samples - 1 && ctx.energy[k] > thold) {
                        k++;
                    }
                    tokens[j].t1 = sample_to_timestamp(k);
                    if (j < ns - 1 && tokens[j].t1 > tokens[j + 1].t0) {
                        tokens[j].t1 = tokens[j + 1].t0;
                    } else {
                        s1 = k;
                    }
                } else {
                    while (ctx.energy[k] < thold && k > s0) {
                        k--;
                    }
                    s1 = k;
                    tokens[j].t1 = sample_to_timestamp(k);
                }
            }
        }
    }

    // fixed token expand (optional)
    //{
    //    const int t_expand = 0;

    //    for (int j = 0; j < n; j++) {
    //        if (j > 0) {
    //            tokens[j].t0 = std::max(0, (int) (tokens[j].t0 - t_expand));
    //        }
    //        if (j < n - 1) {
    //            tokens[j].t1 = tokens[j].t1 + t_expand;
    //        }
    //    }
    //}

    // debug info
    //for (int j = 0; j < n; ++j) {
    //    const auto & token = tokens[j];
    //    const auto tt = token.pt > thold_pt && token.ptsum > 0.01 ? whisper_token_to_str(&ctx, token.tid) : "[?]";
    //    printf("%s: %10s %6.3f %6.3f %6.3f %6.3f %5d %5d '%s'\n", __func__,
    //            tt, token.p, token.pt, token.ptsum, token.vlen, (int) token.t0, (int) token.t1, whisper_token_to_str(&ctx, token.id));

    //    if (tokens[j].id >= whisper_token_eot(&ctx)) {
    //        continue;
    //    }
    //}
}
