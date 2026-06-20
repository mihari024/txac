/*
TXAC Player v14.0 - 14-bit Packed Buffer + On-the-fly Float Conversion
- Buffer de audio armazenado como inteiros empacotados de 14 bits (1.75 bytes/sample)
- Conversão 14bit → float ocorre ao vivo no callback do sokol, sem armazenar float na RAM
- Delta decoding produz int32 → clampado para 14 bits → empacotado no bitstream
- ~56% de economia de RAM em relação ao buffer float anterior (4 bytes/sample → 1.75)
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <immintrin.h>

#define SOKOL_AUDIO_IMPL
#include "sokol_audio.h"


#if defined(_WIN32)
    #include <windows.h>
    #include <conio.h>
    #define THREAD_SLEEP_MS(ms) Sleep(ms)
    #define getch _getch  // Resolve o erro de 'undeclared getch'
    typedef HANDLE thread_ptr;
    #define CREATE_THREAD(ptr, func, arg) *(ptr) = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)func, arg, 0, NULL)
    #define JOIN_THREAD(ptr) (WaitForSingleObject(ptr, INFINITE), CloseHandle(ptr))
#else
    #include <pthread.h>
    #include <unistd.h>
    #include <termios.h>
    #define THREAD_SLEEP_MS(ms) usleep((ms) * 1000)
    typedef pthread_t thread_ptr;
    #define CREATE_THREAD(ptr, func, arg) pthread_create(ptr, NULL, func, arg)
    #define JOIN_THREAD(ptr) pthread_join(ptr, NULL)

    int getch(void) {
        struct termios oldattr, newattr;
        int ch;
        tcgetattr(STDIN_FILENO, &oldattr);
        newattr = oldattr;
        newattr.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newattr);
        ch = getchar();
        tcsetattr(STDIN_FILENO, TCSANOW, &oldattr);
        return ch;
    }
#endif

#define TXAC_MAGIC "TXAC"
#define MAX_CHANNELS 32
#define DB_AMPLIFICATION 110.0  // Mesmo valor do encoder, mas invertido

const char simbolos[16] = {
    ',','-','1','2','3','4','5','6','7','8','9','0',
    '^', '~', '(', ')'
};

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t flags;
    uint64_t total_samples;
} TXACHeader;

// ============================================================================
// BUFFER INT32 INTERMEDIÁRIO (usado internamente no parsing, não armazenado)
// ============================================================================
typedef struct {
    int32_t *data;        
    uint64_t capacity;
    uint64_t count;
} BufferInt32;

void init_buffer_int32(BufferInt32 *buf, uint64_t initial_capacity) {
    buf->capacity = initial_capacity * 4;
    if (buf->capacity < 10000000) {
        buf->capacity = 10000000;
    }
    buf->count = 0;
    buf->data = (int32_t*)malloc(buf->capacity * sizeof(int32_t));
    if (!buf->data) {
        fprintf(stderr, "Error: Failed to allocate RAM for int32 buffer\n");
        exit(1);
    }
}

void ensure_buffer_capacity_int32(BufferInt32 *buf, uint64_t required) {
    if (buf->count + required >= buf->capacity) {
        uint64_t new_cap = buf->capacity * 2;
        while (buf->count + required >= new_cap) new_cap *= 2;
        
        int32_t *new_ptr = (int32_t*)realloc(buf->data, new_cap * sizeof(int32_t));
        if (!new_ptr) {
            fprintf(stderr, "Error: Insufficient memory (RAM is full)\n");
            exit(1);
        }
        buf->data = new_ptr;
        buf->capacity = new_cap;
    }
}

void push_value_int32(BufferInt32 *buf, int32_t value) {
    ensure_buffer_capacity_int32(buf, 1);
    buf->data[buf->count++] = value;
}

// ============================================================================
// BUFFER 14-BIT EMPACOTADO
// Cada amostra ocupa exatamente 14 bits no bitstream (1.75 bytes/sample).
// Signed: range -8192 a 8191 (complemento de 2 com 14 bits).
// Packing: bit_pos = idx * 14, espalhado em até 3 bytes consecutivos.
// ============================================================================
typedef struct {
    uint8_t *data;       // Bitstream empacotado
    uint64_t capacity;   // Capacidade em amostras
    uint64_t count;      // Amostras armazenadas
} Buffer14Bit;

// Quantos bytes são necessários para armazenar 'samples' amostras de 14 bits
static inline uint64_t bytes_for_14bit(uint64_t samples) {
    return (samples * 14 + 7) / 8;
}

void init_buffer_14bit(Buffer14Bit *buf, uint64_t initial_capacity) {
    buf->capacity = initial_capacity ? initial_capacity : 1024;
    buf->count = 0;
    // +4 bytes de margem para que unpack14 nunca leia fora do buffer no último sample
    buf->data = (uint8_t*)calloc(bytes_for_14bit(buf->capacity) + 4, 1);
    if (!buf->data) {
        fprintf(stderr, "Error: Failed to allocate RAM for 14-bit buffer\n");
        exit(1);
    }
}

void ensure_buffer_capacity_14bit(Buffer14Bit *buf, uint64_t required) {
    if (buf->count + required >= buf->capacity) {
        uint64_t new_cap = buf->capacity * 2;
        while (buf->count + required >= new_cap) new_cap *= 2;

        uint64_t old_byte_size = bytes_for_14bit(buf->capacity) + 4;
        uint64_t new_byte_size = bytes_for_14bit(new_cap) + 4;

        uint8_t *new_ptr = (uint8_t*)realloc(buf->data, new_byte_size);
        if (!new_ptr) {
            fprintf(stderr, "Error: Insufficient memory (RAM is full)\n");
            exit(1);
        }
        // Zera os bytes novos para que o OR do pack14 funcione corretamente
        memset(new_ptr + old_byte_size, 0, new_byte_size - old_byte_size);
        buf->data = new_ptr;
        buf->capacity = new_cap;
    }
}

// Escreve um valor int32 (clampado para signed 14-bit) na posição idx do bitstream
static inline void pack14(uint8_t *buf, uint64_t idx, int32_t value) {
    // Clamp para o range de 14 bits com sinal: -8192 a 8191
    if (value >  8191) value =  8191;
    if (value < -8192) value = -8192;

    uint16_t bits = (uint16_t)(value & 0x3FFF);  // 14 bits em dois-complemento
    uint64_t bit_pos = idx * 14;
    uint64_t byte_idx = bit_pos / 8;
    int      bit_off  = (int)(bit_pos % 8);

    // Os 14 bits ocupam de bit_off até bit_off+13, podendo cruzar até 3 bytes
    uint32_t word = (uint32_t)bits << bit_off;
    uint32_t mask = (uint32_t)0x3FFF << bit_off;

    buf[byte_idx]     = (uint8_t)((buf[byte_idx]     & ~(uint8_t)(mask & 0xFF))         | (uint8_t)(word & 0xFF));
    buf[byte_idx + 1] = (uint8_t)((buf[byte_idx + 1] & ~(uint8_t)((mask >> 8) & 0xFF))  | (uint8_t)((word >> 8) & 0xFF));
    if ((bit_off + 14) > 16) {
        buf[byte_idx + 2] = (uint8_t)((buf[byte_idx + 2] & ~(uint8_t)((mask >> 16) & 0xFF)) | (uint8_t)((word >> 16) & 0xFF));
    }
}

// Lê o valor signed 14-bit da posição idx e retorna como int32_t (com sign-extend)
static inline int32_t unpack14(const uint8_t *buf, uint64_t idx) {
    uint64_t bit_pos  = idx * 14;
    uint64_t byte_idx = bit_pos / 8;
    int      bit_off  = (int)(bit_pos % 8);

    uint32_t word = (uint32_t)buf[byte_idx] | ((uint32_t)buf[byte_idx + 1] << 8);
    if ((bit_off + 14) > 16) {
        word |= ((uint32_t)buf[byte_idx + 2] << 16);
    }
    uint32_t bits = (word >> bit_off) & 0x3FFF;

    // Sign-extend de 14 bits para 32 bits
    if (bits & 0x2000) bits |= 0xFFFFC000u;
    return (int32_t)bits;
}

static inline void push_value_14bit(Buffer14Bit *buf, int32_t value) {
    ensure_buffer_capacity_14bit(buf, 1);
    pack14(buf->data, buf->count, value);
    buf->count++;
}

// ============================================================================
// PARSER DIRETO DE 4-BIT → 14-BIT
// ============================================================================
typedef struct {
    uint8_t *raw_data;
    size_t byte_count;
    size_t bit_pos; // Mudamos de byte_pos para bit_pos
} Stream4Bit;

void init_stream(Stream4Bit *s, uint8_t *data, size_t size) {
    s->raw_data = data;
    s->byte_count = size;
    s->bit_pos = 0;
}

// Lê 1 bit do buffer e avança o cursor
static inline int read_bit(Stream4Bit *s) {
    if (s->bit_pos >= s->byte_count * 8) return -1;
    
    int byte_idx = s->bit_pos / 8;
    int bit_off  = 7 - (s->bit_pos % 8); // MSB para LSB
    int bit = (s->raw_data[byte_idx] >> bit_off) & 1;
    
    s->bit_pos++;
    return bit;
}

// Lê 'count' bits e monta um valor
static inline uint32_t read_bits(Stream4Bit *s, int count) {
    uint32_t value = 0;
    for (int i = 0; i < count; i++) {
        int bit = read_bit(s);
        if (bit == -1) break;
        value = (value << 1) | bit;
    }
    return value;
}

char read_next_char(Stream4Bit *s) {
    if (s->bit_pos >= s->byte_count * 8) return '\0';

    int k = 1; // DEVE ser o mesmo k usado no encoder
    
    // 1. Decodifica o Quociente (Unário: sequência de 1s terminada em 0)
    uint32_t q = 0;
    int bit;
    while ((bit = read_bit(s)) == 1) {
        q++;
    }
    
    // Se bit for -1, chegamos ao fim prematuro do stream
    if (bit == -1) return '\0';

    // 2. Decodifica o Resto (Binário de k bits)
    uint32_t r = read_bits(s, k);
    
    // 3. Reconstrói o valor original (índice do símbolo)
    uint32_t symbol_idx = (q << k) | r;

    // Proteção contra overflow ou dados corrompidos
    if (symbol_idx >= 16) return ','; 

    return simbolos[symbol_idx];
}

typedef struct {
    int32_t value;
    uint32_t argument;
    char operation;
} ParsedToken;

/* Analisa o texto lógico diretamente, sem string temporária nem sscanf. */
static int read_token_stream(Stream4Bit *s, ParsedToken *token) {
    uint64_t value = 0;
    uint64_t argument = 0;
    int negative = 0;
    int have_value = 0;
    int have_argument = 0;
    char operation = '\0';
    char c;

    while ((c = read_next_char(s)) != '\0' && c != ',') {
        if (c == '(' || c == ')') continue;
        if (!have_value && c == '-') {
            negative = 1;
        } else if (c >= '0' && c <= '9') {
            uint64_t *destination = operation ? &argument : &value;
            *destination = *destination * 10 + (uint64_t)(c - '0');
            if (operation) have_argument = 1;
            else have_value = 1;
        } else if ((c == '^' || c == '~') && have_value && !operation) {
            operation = c;
        } else {
            return -1;
        }
    }

    if (!have_value) return 0;
    if (operation && !have_argument) return -1;
    if ((!negative && value > INT32_MAX) ||
        (negative && value > UINT64_C(2147483648)) ||
        argument > UINT32_MAX) return -1;

    token->value = negative ? (int32_t)(-(int64_t)value) : (int32_t)value;
    token->argument = (uint32_t)argument;
    token->operation = operation;
    return 1;
}

// ============================================================================
// DESCOMPRESSÃO DIRETA PARA 14-BIT COM DELTA DECODING
// O acumulador do delta decoding produz int32; o resultado é clampado e
// empacotado como 14-bit signed no buffer de saída.
// ============================================================================
typedef struct {
    int channel_id;
    uint8_t *compressed_4bit;
    size_t compressed_size;
    Buffer14Bit *output_buffer;       // Buffer de saída 14-bit (antes era float)
    thread_ptr thread;
    volatile int finished;
    int use_delta_encoding;
} ChannelLoader;

void process_stream_to_14bit(ChannelLoader *ldr, Stream4Bit *stream, uint64_t *sample_idx,
                              int recursion_depth, int32_t *accumulator) {
    if (recursion_depth > 100) return;
    
    // CORREÇÃO: Usamos bit_pos para verificar o fim do stream
    // Multiplicamos byte_count por 8 para ter o total de bits
    while (stream->bit_pos < (stream->byte_count * 8)) {
        ParsedToken token;
        int token_status = read_token_stream(stream, &token);
        if (token_status <= 0) break;

        uint32_t rep;
        int32_t delta_value;
        
        // Caso 1: Repetição (valor^repeticoes)
        if (token.operation == '^') {
            delta_value = token.value;
            rep = token.argument;
            
            ensure_buffer_capacity_14bit(ldr->output_buffer, rep);
            for (uint32_t i = 0; i < rep; i++) {
                if (ldr->use_delta_encoding) {
                    *accumulator += delta_value;
                    pack14(ldr->output_buffer->data, ldr->output_buffer->count++, *accumulator);
                } else {
                    pack14(ldr->output_buffer->data, ldr->output_buffer->count++, delta_value);
                }
                (*sample_idx)++;
            }
            if (recursion_depth > 0) return;
        }
        // Caso 2: Sniper/Loop (valor~distancia)
        else if (token.operation == '~') {
            delta_value = token.value;
            rep = token.argument;
            
            if (ldr->use_delta_encoding) *accumulator += delta_value;
            else                         *accumulator  = delta_value;
            
            push_value_14bit(ldr->output_buffer, *accumulator);
            (*sample_idx)++;
            
            for (uint32_t i = 0; i < rep; i++) {
                process_stream_to_14bit(ldr, stream, sample_idx, recursion_depth + 1, accumulator);
            }
            
            if (ldr->use_delta_encoding) *accumulator += delta_value;
            else                         *accumulator  = delta_value;
            
            push_value_14bit(ldr->output_buffer, *accumulator);
            (*sample_idx)++;
            if (recursion_depth > 0) return;
        }
        // Caso 3: Valor simples
        else if (token.operation == '\0') {
            delta_value = token.value;
            
            if (ldr->use_delta_encoding) {
                if (*sample_idx == 0) *accumulator  = delta_value;
                else                  *accumulator += delta_value;
            } else {
                *accumulator = delta_value;
            }
            
            push_value_14bit(ldr->output_buffer, *accumulator);
            (*sample_idx)++;
            if (recursion_depth > 0) return;
        }
    }
}

void *loader_thread_func(void *arg) {
    ChannelLoader *ldr = (ChannelLoader*)arg;
    uint64_t sample_idx = 0;
    int32_t accumulator = 0;

    printf("  [Channel %d] Starting Rice Decoding...\n", ldr->channel_id);
    
    if (ldr->use_delta_encoding) {
        printf("  [Channel %d] Decompressing 4bit to 14bit with delta decoding...\n", ldr->channel_id);
    } else {
        printf("  [Channel %d] Decompressing 4bit to 14bit directly...\n", ldr->channel_id);
    }
    
    Stream4Bit stream;
    init_stream(&stream, ldr->compressed_4bit, ldr->compressed_size);
    process_stream_to_14bit(ldr, &stream, &sample_idx, 0, &accumulator);
    
    // NÃO há normalização aqui. A conversão para float acontece ao vivo no
    // callback do sokol (audio_cb), sem armazenar os valores em ponto flutuante na RAM.
    
    printf("  [Channel %d] %llu samples loaded\n",
           ldr->channel_id, (unsigned long long)sample_idx);
    ldr->finished = 1;
    return NULL;
}

// ============================================================================
// ESTRUTURAS PRINCIPAIS
// ============================================================================
typedef struct {
    FILE       *file;
    TXACHeader  header;
    uint8_t    *pcm_data_14bit;          // Buffer final intercalado em 14-bit packed
    uint64_t    total_samples;
    volatile uint64_t playback_cursor;
    volatile int is_paused;
    volatile int running;
    ChannelLoader loaders[MAX_CHANNELS];
    Buffer14Bit   channel_buffers[MAX_CHANNELS];
    float         conversion_factor;     // Pré-calculado uma vez; usado no callback
} txacplay_desc;

// ============================================================================
// INTERCALAÇÃO 14-BIT
// Lê as amostras de cada canal (unpack14), intercala e reempacota (pack14)
// no buffer interleaved final. Zero alocação de float neste passo.
// ============================================================================
void intercalar_canais_14bit(txacplay_desc *tp) {
    printf("\nInterleaving channels in RAM...\n");
    
    uint64_t frames = tp->channel_buffers[0].count;
    for (int i = 1; i < tp->header.channels; i++) {
        if (tp->channel_buffers[i].count < frames)
            frames = tp->channel_buffers[i].count;
    }

    tp->total_samples = frames * tp->header.channels;

    // +4 bytes de margem para leituras seguras no final do buffer
    uint64_t byte_size = bytes_for_14bit(tp->total_samples) + 4;
    tp->pcm_data_14bit = (uint8_t*)calloc(byte_size, 1);
    if (!tp->pcm_data_14bit) {
        printf("Fatal error: No RAM for final 14-bit buffer. (%llu samples)\n",
               (unsigned long long)tp->total_samples);
        exit(1);
    }
    
    for (uint64_t f = 0; f < frames; f++) {
        for (int c = 0; c < tp->header.channels; c++) {
            uint64_t interleaved_idx = f * tp->header.channels + c;
            int32_t  sample          = unpack14(tp->channel_buffers[c].data, f);
            pack14(tp->pcm_data_14bit, interleaved_idx, sample);
        }
    }
    
    double size_mb = (double)byte_size / (1024.0 * 1024.0);
    printf("Ready: %.2f MB of RAM for audio (14-bit packed, ~56%% less than float).\n", size_mb);
}

// ============================================================================
// CÁLCULO DE TEMPO
// ============================================================================
double calculate_time(txacplay_desc *tp) {
    return (double)tp->playback_cursor / (double)(tp->header.sample_rate * tp->header.channels);
}

double calculate_duration(txacplay_desc *tp) {
    return (double)tp->total_samples / (double)(tp->header.sample_rate * tp->header.channels);
}

// ============================================================================
// CALLBACK SOKOL — CONVERSÃO 14-BIT → FLOAT AO VIVO
// Nenhum float é armazenado na RAM; a conversão acontece amostra a amostra
// enquanto o sokol consome o buffer de saída.
// ============================================================================
void audio_cb(float *buffer, int num_frames, int num_channels, void *user_data) {
    txacplay_desc *tp = (txacplay_desc*)user_data;
    
    if (tp->is_paused || !tp->running || !tp->pcm_data_14bit) {
        memset(buffer, 0, num_frames * num_channels * sizeof(float));
        return;
    }
    
    int   total_samples = num_frames * num_channels;
    float cf            = tp->conversion_factor;  // fator pré-calculado: pow(10, dB/20) / 2^31
    
    for (int i = 0; i < total_samples; i++) {
        if (tp->playback_cursor >= tp->total_samples) tp->playback_cursor = 0;

        // unpack14: lê inteiro signed 14-bit → int32
        // Multiplicação por cf: normaliza para -1.0..1.0 com amplificação DB
        // O float resultante NUNCA é armazenado fora deste loop (fica no buffer do sokol)
        buffer[i] = (float)unpack14(tp->pcm_data_14bit, tp->playback_cursor++) * cf;
    }

}

void toggle_pause(txacplay_desc *tp) {
    tp->is_paused = !tp->is_paused;
    if (tp->is_paused) {
        printf("\nPAUSED\n");
    } else {
        printf("\nPLAYING\n");
    }
}

void txacplay_seek_absolute(txacplay_desc *tp, double time_seconds) {
    double   samples_per_second = (double)tp->header.sample_rate * tp->header.channels;
    uint64_t target_samples     = (uint64_t)(time_seconds * samples_per_second);
    
    uint64_t remainder = target_samples % tp->header.channels;
    target_samples -= remainder;
    
    if (target_samples > tp->total_samples)
        target_samples = tp->total_samples - (tp->total_samples % tp->header.channels);
    if (time_seconds < 0) target_samples = 0;
    
    tp->playback_cursor = target_samples;
}

void update_timer(txacplay_desc *tp) {
    printf("\r%.2f / %.2f sec", calculate_time(tp), calculate_duration(tp));
    fflush(stdout);
}

// ============================================================================
// ABERTURA E SETUP
// ============================================================================
txacplay_desc *txacplay_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    
    txacplay_desc *tp = (txacplay_desc*)calloc(1, sizeof(txacplay_desc));
    tp->file = f;
    
    char     magic[4]; fread(magic, 1, 4, f);
    uint32_t version;  fread(&version, 4, 1, f);
    fread(&tp->header.sample_rate,    4, 1, f);
    fread(&tp->header.channels,       2, 1, f);
    fread(&tp->header.bits_per_sample,2, 1, f);
    fread(&tp->header.flags,          4, 1, f);
    fread(&tp->header.total_samples,  8, 1, f);
    fseek(f, 36, SEEK_CUR);
    
    int use_delta = (tp->header.flags & (1 << 1)) != 0;
    
    printf("TXAC version: %u\n", version);
    printf("Delta encoding: %s\n", use_delta ? "YES" : "NO");
    printf("Sample rate: %u Hz\n", tp->header.sample_rate);
    printf("Channels: %u\n", tp->header.channels);
    printf("Bits per sample: %u\n", tp->header.bits_per_sample);
    
    // Fator de conversão pré-calculado: usado no callback para 14-bit → float
    // Mantém a mesma semântica do encoder (amplificação de DB_AMPLIFICATION dB)
    tp->conversion_factor = (float)(pow(10.0, DB_AMPLIFICATION / 20.0) / 2147483648.0);
    
    uint64_t offsets[MAX_CHANNELS], sizes[MAX_CHANNELS];
    for (int i = 0; i < tp->header.channels; i++) {
        fread(&offsets[i], 8, 1, f);
        fread(&sizes[i],   8, 1, f);
    }
    
    // Inicia threads que fazem parsing direto 4bit → 14bit
    for (int i = 0; i < tp->header.channels; i++) {
        init_buffer_14bit(&tp->channel_buffers[i],
                          tp->header.total_samples);
        
        tp->loaders[i].channel_id        = i;
        tp->loaders[i].output_buffer     = &tp->channel_buffers[i];
        tp->loaders[i].compressed_size   = sizes[i];
        tp->loaders[i].use_delta_encoding = use_delta;
        
        tp->loaders[i].compressed_4bit = malloc(sizes[i]);
        fseek(f, offsets[i], SEEK_SET);
        fread(tp->loaders[i].compressed_4bit, 1, sizes[i], f);
        
        CREATE_THREAD(&tp->loaders[i].thread, loader_thread_func, &tp->loaders[i]);
    }
    
    for (int i = 0; i < tp->header.channels; i++) {
        JOIN_THREAD(tp->loaders[i].thread);
        free(tp->loaders[i].compressed_4bit);
    }
    
    intercalar_canais_14bit(tp);
    
    // Libera os buffers por canal; só o buffer intercalado fica na RAM
    for (int i = 0; i < tp->header.channels; i++) {
        free(tp->channel_buffers[i].data);
    }
    
    tp->running = 1;
    return tp;
}

void txacplay_close(txacplay_desc *tp) {
    if (!tp) return;
    tp->running = 0;
    if (tp->pcm_data_14bit) free(tp->pcm_data_14bit);
    if (tp->file) fclose(tp->file);
    free(tp);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Use: %s <file.txac>\n", argv[0]);
        return 1;
    }
    
    printf("Starting TXAC Player v0.3.0...\n");
    txacplay_desc *tp = txacplay_open(argv[1]);
    if (!tp) {
        printf("Error opening file.\n");
        return 1;
    }

    saudio_setup(&(saudio_desc){
        .sample_rate        = tp->header.sample_rate,
        .num_channels       = tp->header.channels,
        .stream_userdata_cb = audio_cb,
        .user_data          = tp,
        .buffer_frames      = 4096
    });
    
    printf("Playing...\n Press:\n [space] to pause\n [x] to go back 5s\n [c] to go forward 5s\n [q] to exit\n\n");
    
    int wants_to_quit = 0;
    while (!wants_to_quit) {
        char c = getch();
        
        switch (c) {
            case ' ':
                toggle_pause(tp);
                break;
            case 'x': {
                double current = calculate_time(tp);
                txacplay_seek_absolute(tp, current - 5.0);
                break;
            }
            case 'c': {
                double current = calculate_time(tp);
                txacplay_seek_absolute(tp, current + 5.0);
                break;
            }
            case 'q':
                wants_to_quit = 1;
                break;
        }
        
        update_timer(tp);
    }
    
    saudio_shutdown();
    txacplay_close(tp);
    printf("\n\nBye bye\n");
    return 0;
}
