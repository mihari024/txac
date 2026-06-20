/*
TXAC Decoder v4.0 - Multi-core, Rice/Golomb + Delta Decoding → WAV 32-bit
- Lê formato TXAC v3+ (multicanal com header completo)
- Decodificação Rice/Golomb (k=1) — substitui o antigo nibble-reader
- Suporte a Delta Encoding (flag bit 1 do header)
- MAX_CHANNELS expandido para 32
- Buffers de decoders alocados dinamicamente
- Multi-threading para descompressão paralela
- Aplica ganho de 110 dB na saída WAV

Compilar:
    zig cc txac_output.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txac_decode.exe

Uso:
    txac_decode input.txac output.wav
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <immintrin.h>

#define TXAC_MAGIC        "TXAC"
#define MAX_CHANNELS      32
#define GAIN_DB           110.0
#define AMPLITUDE_FACTOR  316227.76601683795   /* pow(10.0, 110.0 / 20.0) */
#define INT32_MAX_VAL     2147483647
#define INT32_MIN_VAL    -2147483648

/* ============================================================================
 * TABELA DE SÍMBOLOS — deve ser idêntica à do encoder (txacplay v14)
 * Ordem antiga: '0','1','2','3','4','5','6','7','8','9',',','^','~','(',')','-'
 * Ordem nova  : ',','-','1','2','3','4','5','6','7','8','9','0','^','~','(',')'
 * ========================================================================== */
static const char simbolos[16] = {
    ',', '-', '1', '2', '3', '4', '5', '6',
    '7', '8', '9', '0', '^', '~', '(', ')'
};

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t flags;
    uint64_t total_samples;
} TXACHeader;

/* ============================================================================
 * BUFFER INT32
 * ========================================================================== */
typedef struct {
    int32_t  *data;
    uint64_t  capacity;
    uint64_t  count;
} BufferInt32;

static void init_buffer_int32(BufferInt32 *buf, uint64_t initial_capacity) {
    buf->capacity = initial_capacity ? initial_capacity : 1024;
    buf->count = 0;
    buf->data = (int32_t *)malloc(buf->capacity * sizeof(int32_t));
    if (!buf->data) {
        fprintf(stderr, "Error: Failed to allocate RAM for buffer\n");
        exit(1);
    }
}

static void ensure_buffer_capacity(BufferInt32 *buf, uint64_t required) {
    if (buf->count + required < buf->capacity) return;
    uint64_t new_cap = buf->capacity * 2;
    while (buf->count + required >= new_cap) new_cap *= 2;
    int32_t *new_ptr = (int32_t *)realloc(buf->data, new_cap * sizeof(int32_t));
    if (!new_ptr) {
        fprintf(stderr, "Error: Insufficient memory\n");
        exit(1);
    }
    buf->data     = new_ptr;
    buf->capacity = new_cap;
}

static void push_value_int32(BufferInt32 *buf, int32_t value) {
    ensure_buffer_capacity(buf, 1);
    buf->data[buf->count++] = value;
}

/* ============================================================================
 * STREAM READER — Rice/Golomb com k=1  (igual ao txacplay v14)
 *
 * O formato antigo (v3) usava nibble direto (byte_pos + nibble_pos).
 * O formato novo (v14) empacota cada símbolo com codificação de Golomb:
 *   1. Quociente unário: lê bits 1 até encontrar um 0  →  q
 *   2. Resto binário de k=1 bit                        →  r
 *   3. índice do símbolo = (q << 1) | r
 * ========================================================================== */
typedef struct {
    uint8_t *raw_data;
    size_t   byte_count;
    size_t   bit_pos;      /* cursor em bits; MSB first dentro de cada byte */
} Stream4Bit;

static void init_stream(Stream4Bit *s, uint8_t *data, size_t size) {
    s->raw_data   = data;
    s->byte_count = size;
    s->bit_pos    = 0;
}

static inline int read_bit(Stream4Bit *s) {
    if (s->bit_pos >= s->byte_count * 8) return -1;
    int byte_idx = (int)(s->bit_pos / 8);
    int bit_off  = 7 - (int)(s->bit_pos % 8);   /* MSB → LSB */
    int bit = (s->raw_data[byte_idx] >> bit_off) & 1;
    s->bit_pos++;
    return bit;
}

static inline uint32_t read_bits(Stream4Bit *s, int count) {
    uint32_t value = 0;
    for (int i = 0; i < count; i++) {
        int bit = read_bit(s);
        if (bit < 0) break;
        value = (value << 1) | (uint32_t)bit;
    }
    return value;
}

static char read_next_char(Stream4Bit *s) {
    if (s->bit_pos >= s->byte_count * 8) return '\0';

    const int k = 1;   /* deve ser o mesmo k do encoder */

    /* 1. Quociente unário */
    uint32_t q = 0;
    int bit;
    while ((bit = read_bit(s)) == 1) q++;
    if (bit < 0) return '\0';

    /* 2. Resto de k bits */
    uint32_t r = read_bits(s, k);

    /* 3. Reconstrói índice */
    uint32_t sym = (q << k) | r;
    if (sym >= 16) return ',';   /* proteção contra dados corrompidos */
    return simbolos[sym];
}

typedef struct {
    int32_t  value;
    uint32_t argument;
    char     operation;
} ParsedToken;

/* Analisa diretamente o fluxo de texto lógico do TXAC, sem criar uma string C
 * temporária nem chamar o mecanismo de scanf dependente de localidade. */
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
    if ((!negative && value > INT32_MAX_VAL) ||
        (negative && value > UINT64_C(2147483648)) ||
        argument > UINT32_MAX) return -1;

    token->value = negative ? (int32_t)(-(int64_t)value) : (int32_t)value;
    token->argument = (uint32_t)argument;
    token->operation = operation;
    return 1;
}

/* ============================================================================
 * APLICAÇÃO DE GANHO COM CLIPPING  →  int32
 * ========================================================================== */
static inline int32_t apply_gain_and_clip(double value) {
    double boosted = value * AMPLITUDE_FACTOR;
    if (boosted >  (double)INT32_MAX_VAL) return (int32_t) INT32_MAX_VAL;
    if (boosted <  (double)INT32_MIN_VAL) return (int32_t) INT32_MIN_VAL;
    return (int32_t)boosted;
}

/* ============================================================================
 * DECODER POR CANAL
 * ========================================================================== */
typedef struct {
    int          channel_id;
    uint8_t     *compressed_4bit;
    size_t       compressed_size;
    BufferInt32 *output_buffer;
    pthread_t    thread;
    volatile int finished;
    int          use_delta_encoding;   /* novo: detectado via flag bit 1 */
} ChannelDecoder;

/* Recursão de parsing — espelha process_stream_to_14bit() do txacplay v14,
 * mas produz int32 com ganho para escrita direta no WAV. */
static void process_stream_to_int32(ChannelDecoder *dec, Stream4Bit *stream,
                                    uint64_t *sample_idx, int recursion_depth,
                                    int32_t *accumulator) {
    if (recursion_depth > 100) return;

    while (stream->bit_pos < stream->byte_count * 8) {
        ParsedToken token;
        int token_status = read_token_stream(stream, &token);
        if (token_status <= 0) break;

        uint32_t rep;
        int32_t delta;
        int32_t out;

        /* --- Caso 1: Repetição  valor^N ---------------------------------- */
        if (token.operation == '^') {
            delta = token.value;
            rep = token.argument;
            ensure_buffer_capacity(dec->output_buffer, (uint64_t)rep);

            if (!dec->use_delta_encoding || delta == 0) {
                /* Todos os N samples têm o mesmo valor → AVX2 */
                if (!dec->use_delta_encoding)
                    *accumulator = delta;
                /* (delta==0 com delta_encoding: acumulador não muda) */

                out = apply_gain_and_clip((double)*accumulator);

                if (rep >= 8) {
                    __m256i vv = _mm256_set1_epi32(out);
                    uint32_t blocks = rep / 8;
                    for (uint32_t i = 0; i < blocks; i++) {
                        _mm256_storeu_si256(
                            (__m256i *)&dec->output_buffer->data[dec->output_buffer->count],
                            vv);
                        dec->output_buffer->count += 8;
                        *sample_idx += 8;
                    }
                    uint32_t rem = rep % 8;
                    for (uint32_t i = 0; i < rem; i++) {
                        dec->output_buffer->data[dec->output_buffer->count++] = out;
                        (*sample_idx)++;
                    }
                } else {
                    for (uint32_t i = 0; i < rep; i++) {
                        dec->output_buffer->data[dec->output_buffer->count++] = out;
                        (*sample_idx)++;
                    }
                }
            } else {
                /* Delta != 0: cada sample acumula valor diferente */
                for (uint32_t i = 0; i < rep; i++) {
                    *accumulator += delta;
                    out = apply_gain_and_clip((double)*accumulator);
                    dec->output_buffer->data[dec->output_buffer->count++] = out;
                    (*sample_idx)++;
                }
            }

            if (recursion_depth > 0) return;
        }
        /* --- Caso 2: Sniper  valor~N ------------------------------------- */
        else if (token.operation == '~') {
            delta = token.value;
            rep = token.argument;

            if (dec->use_delta_encoding) *accumulator += delta;
            else                          *accumulator  = delta;

            out = apply_gain_and_clip((double)*accumulator);
            push_value_int32(dec->output_buffer, out);
            (*sample_idx)++;

            for (uint32_t i = 0; i < rep; i++) {
                process_stream_to_int32(dec, stream, sample_idx,
                                        recursion_depth + 1, accumulator);
            }

            /* Repete o valor âncora ao final (comportamento do txacplay) */
            if (dec->use_delta_encoding) *accumulator += delta;
            else                          *accumulator  = delta;

            out = apply_gain_and_clip((double)*accumulator);
            push_value_int32(dec->output_buffer, out);
            (*sample_idx)++;

            if (recursion_depth > 0) return;
        }
        /* --- Caso 3: Valor simples --------------------------------------- */
        else if (token.operation == '\0') {
            delta = token.value;

            if (dec->use_delta_encoding) {
                /* Primeiro sample inicializa; demais acumulam */
                if (*sample_idx == 0) *accumulator  = delta;
                else                  *accumulator += delta;
            } else {
                *accumulator = delta;
            }

            out = apply_gain_and_clip((double)*accumulator);
            push_value_int32(dec->output_buffer, out);
            (*sample_idx)++;
            if (recursion_depth > 0) return;
        }
    }
}

static void *decoder_thread_func(void *arg) {
    ChannelDecoder *dec = (ChannelDecoder *)arg;
    uint64_t sample_idx  = 0;
    int32_t  accumulator = 0;

    if (dec->use_delta_encoding)
        printf("  [Channel %d] Rice/Golomb + delta decoding to int32...\n",
               dec->channel_id);
    else
        printf("  [Channel %d] Rice/Golomb to int32...\n", dec->channel_id);

    Stream4Bit stream;
    init_stream(&stream, dec->compressed_4bit, dec->compressed_size);
    process_stream_to_int32(dec, &stream, &sample_idx, 0, &accumulator);

    printf("  [Channel %d] %llu samples decoded\n",
           dec->channel_id, (unsigned long long)sample_idx);
    dec->finished = 1;
    return NULL;
}

/* ============================================================================
 * INTERCALAÇÃO DE CANAIS
 * ========================================================================== */
static int32_t *intercalar_canais(BufferInt32 *channels, int num_channels,
                                   uint64_t *total_samples_out) {
    printf("\nInterleaving channels...\n");

    uint64_t frames = channels[0].count;
    for (int i = 1; i < num_channels; i++)
        if (channels[i].count < frames) frames = channels[i].count;

    uint64_t total = frames * (uint64_t)num_channels;
    int32_t *buf = (int32_t *)malloc(total * sizeof(int32_t));
    if (!buf) { fprintf(stderr, "Fatal: No RAM for interleaved buffer\n"); exit(1); }

    for (uint64_t f = 0; f < frames; f++)
        for (int c = 0; c < num_channels; c++)
            buf[f * num_channels + c] = channels[c].data[f];

    *total_samples_out = total;
    printf("Interleaved: %.2f MB (%llu samples)\n",
           (double)(total * sizeof(int32_t)) / (1024.0 * 1024.0),
           (unsigned long long)total);
    return buf;
}

/* ============================================================================
 * SALVAR WAV 32-BIT PCM
 * ========================================================================== */
static void salvar_wav_32bit(const char *filename, int32_t *samples,
                              uint64_t sample_count, uint32_t sample_rate,
                              uint16_t channels) {
    FILE *f = fopen(filename, "wb");
    if (!f) { perror("Error creating WAV"); return; }

    printf("\nSaving WAV: %u Hz, %u channels, 32-bit...\n",
           sample_rate, channels);

    const uint16_t bps       = 32;
    const uint16_t audio_fmt = 1;    /* PCM */
    const uint32_t sub1_sz   = 16;
    uint32_t byte_rate   = sample_rate * channels * (bps / 8);
    uint16_t block_align = (uint16_t)(channels * (bps / 8));
    uint32_t data_size   = (uint32_t)(sample_count * sizeof(int32_t));
    uint32_t file_size   = 36 + data_size;

    uint8_t hdr[44];
    memset(hdr, 0, 44);
    memcpy(hdr,      "RIFF", 4);  memcpy(hdr +  4, &file_size,   4);
    memcpy(hdr +  8, "WAVE", 4);  memcpy(hdr + 12, "fmt ",       4);
    memcpy(hdr + 16, &sub1_sz,  4); memcpy(hdr + 20, &audio_fmt,  2);
    memcpy(hdr + 22, &channels, 2); memcpy(hdr + 24, &sample_rate,4);
    memcpy(hdr + 28, &byte_rate,4); memcpy(hdr + 32, &block_align,2);
    memcpy(hdr + 34, &bps,      2); memcpy(hdr + 36, "data",      4);
    memcpy(hdr + 40, &data_size,4);

    fwrite(hdr, 1, 44, f);
    fwrite(samples, sizeof(int32_t), sample_count, f);
    fclose(f);

    printf("WAV saved: %s (%.2f MB)\n", filename,
           (double)data_size / (1024.0 * 1024.0));
}

/* ============================================================================
 * MAIN
 * ========================================================================== */
int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage:   %s <input.txac> <output.wav>\n", argv[0]);
        printf("Example: %s audio.txac  audio.wav\n",     argv[0]);
        return 1;
    }

    const char *input  = argv[1];
    const char *output = argv[2];

    printf("\nTXAC output v0.3.0\n");
    //printf("Input:  %s\n", input);
    //printf("Output: %s\n\n", output);

    /* --- Abre e valida o arquivo ----------------------------------------- */
    FILE *f = fopen(input, "rb");
    if (!f) { fprintf(stderr, "Error: Cannot open %s\n", input); return 1; }

    char magic[4];
    fread(magic, 1, 4, f);
    if (memcmp(magic, TXAC_MAGIC, 4) != 0) {
        fprintf(stderr, "Error: Invalid TXAC file (bad magic)\n");
        fclose(f); return 1;
    }

    /* --- Lê header -------------------------------------------------------- */
    TXACHeader hdr;
    uint32_t   version;
    fread(&version,             4, 1, f);
    fread(&hdr.sample_rate,     4, 1, f);
    fread(&hdr.channels,        2, 1, f);
    fread(&hdr.bits_per_sample, 2, 1, f);
    fread(&hdr.flags,           4, 1, f);
    fread(&hdr.total_samples,   8, 1, f);

    /* Flags — iguais ao txacplay */
    int use_delta    = (hdr.flags & (1 << 1)) != 0;

    printf(" TXAC Info:\n");
    printf("   Version:         %u\n",   version);
    printf("   Sample rate:     %u Hz\n", hdr.sample_rate);
    printf("   Channels:        %u\n",   hdr.channels);
    printf("   Bits per sample: %u\n",   hdr.bits_per_sample);
    printf("   Total samples:   %llu\n", (unsigned long long)hdr.total_samples);
    //printf("   Loop:            %s\n",   loop_enabled ? "Yes" : "No");
    printf("   Delta encoding:  %s\n\n", use_delta    ? "Yes" : "No");

    if (hdr.channels == 0 || hdr.channels > MAX_CHANNELS) {
        fprintf(stderr, "Error: Unsupported channel count (%u)\n", hdr.channels);
        fclose(f); return 1;
    }

    /* --- Pula reserved bytes e lê índice de canais ----------------------- */
    fseek(f, 36, SEEK_CUR);

    uint64_t offsets[MAX_CHANNELS];
    uint64_t sizes[MAX_CHANNELS];
    for (int i = 0; i < (int)hdr.channels; i++) {
        fread(&offsets[i], 8, 1, f);
        fread(&sizes[i],   8, 1, f);
    }

    /* --- Aloca e lança threads de decodificação -------------------------- */
    ChannelDecoder *decoders = (ChannelDecoder *)calloc(hdr.channels,
                                                        sizeof(ChannelDecoder));
    BufferInt32    *cbufs    = (BufferInt32    *)calloc(hdr.channels,
                                                        sizeof(BufferInt32));
    if (!decoders || !cbufs) {
        fprintf(stderr, "Error: Cannot allocate decoder structs\n");
        fclose(f); return 1;
    }

    printf("Starting multi-threaded decompression (%u thread%s)...\n",
           hdr.channels, hdr.channels == 1 ? "" : "s");

    for (int i = 0; i < (int)hdr.channels; i++) {
        uint64_t per_ch = hdr.total_samples > 0 ? hdr.total_samples : 1024;
        init_buffer_int32(&cbufs[i], per_ch);

        decoders[i].channel_id         = i;
        decoders[i].output_buffer      = &cbufs[i];
        decoders[i].compressed_size    = sizes[i];
        decoders[i].use_delta_encoding = use_delta;
        decoders[i].finished           = 0;

        decoders[i].compressed_4bit = (uint8_t *)malloc(sizes[i]);
        if (!decoders[i].compressed_4bit) {
            fprintf(stderr, "Error: Cannot allocate memory for channel %d\n", i);
            fclose(f); return 1;
        }
        fseek(f, (long)offsets[i], SEEK_SET);
        fread(decoders[i].compressed_4bit, 1, sizes[i], f);

        pthread_create(&decoders[i].thread, NULL,
                       decoder_thread_func, &decoders[i]);
    }

    for (int i = 0; i < (int)hdr.channels; i++) {
        pthread_join(decoders[i].thread, NULL);
        free(decoders[i].compressed_4bit);
    }
    fclose(f);

    /* --- Intercala e salva WAV ------------------------------------------- */
    uint64_t total_samples;
    int32_t *interleaved = intercalar_canais(cbufs, (int)hdr.channels,
                                             &total_samples);
    salvar_wav_32bit(output, interleaved, total_samples,
                     hdr.sample_rate, hdr.channels);

    /* --- Cleanup ---------------------------------------------------------- */
    for (int i = 0; i < (int)hdr.channels; i++) free(cbufs[i].data);
    free(cbufs);
    free(decoders);
    free(interleaved);

    printf("\nDecoding completed successfully!\n");
    printf("Audio duration: %.2f seconds\n",
           (double)total_samples / ((double)hdr.sample_rate * hdr.channels));
    return 0;
}
