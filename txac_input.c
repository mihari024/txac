/*
TXAC Encoder v3.2 - Delta Encoding FIXED
- Delta encoding corrigido para compatibilidade com player 2-stage
- FFmpeg converte para 32-bit (pcm_s32le)
- Suporte nativo para leitura de WAV 32-bit int
- Mantém otimizações AVX2

Compilar:
    zig cc txac_input.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txac_encode.exe
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <immintrin.h>

// Retorna a distância (dist) se encontrar o valor, ou -1 se não encontrar na janela
int find_next_match_avx2(const int32_t *deltas, int start, int limit, int32_t target) {
    // Carrega o valor alvo em todas as 8 posições do registrador AVX
    __m256i target_vec = _mm256_set1_epi32(target);
    
    // Processa em blocos de 8
    int i = start;
    for (; i <= limit - 8; i += 8) {
        // Carrega 8 samples (unaligned para evitar segfaults de borda)
        __m256i data = _mm256_loadu_si256((__m256i*)&deltas[i]);
        
        // Compara o alvo com os 8 samples simultaneamente
        // Retorna 0xFFFFFFFF nos campos onde houver igualdade
        __m256i cmp = _mm256_cmpeq_epi32(data, target_vec);
        
        // Cria uma máscara de bits a partir do resultado da comparação
        int mask = _mm256_movemask_ps(_mm256_castsi256_ps(cmp));
        
        if (mask != 0) {
            // Se mask != 0, pelo menos um dos 8 bateu. 
            // Usamos __builtin_ctz (count trailing zeros) para achar o índice exato.
            return i + __builtin_ctz(mask);
        }
    }

    // Processa o restante (caso o limite não seja múltiplo de 8)
    for (; i <= limit; i++) {
        if (deltas[i] == target) return i;
    }

    return -1;
}

#define TXAC_MAGIC "TXAC"
#define TXAC_VERSION 4
#define DB_REDUCTION 110.0
#define MAX_CHANNELS 32
#define GROWTH_FACTOR 2

const char simbolos[16] = {
    ',','-','1','2','3','4','5','6','7','8','9','0',
    '^', '~', '(', ')'
};

typedef struct {
    int32_t *samples;
    size_t count;
    size_t capacity;
} Channel;

typedef struct {
    uint8_t *data;
    size_t byte_count;
    size_t capacity;
} Binary4BitBuffer;

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t flags;
    uint64_t total_samples;
} TXACHeader;

typedef struct {
    Channel *channel;
    Binary4BitBuffer *output;
    int channel_id;
    int enable_loop_compression;
} ThreadData;

typedef struct {
    Binary4BitBuffer *output;
    uint64_t bits;
    unsigned bit_count;
} RiceBuffer;

// ============================================================================
// BUFFER + ESCRITA DIRETA DE SÍMBOLOS DE TEXTO EM RICE
// ============================================================================

void init_4bit_buffer(Binary4BitBuffer *buf) {
    buf->capacity = 1024 * 1024;
    buf->byte_count = 0;
    buf->data = (uint8_t*)malloc(buf->capacity);
    if (!buf->data) {
        fprintf(stderr, "Error allocating output buffer.\n");
        exit(1);
    }
}

void ensure_4bit_capacity(Binary4BitBuffer *buf, size_t required_bytes) {
    if (buf->byte_count + required_bytes >= buf->capacity) {
        size_t new_cap = buf->capacity * GROWTH_FACTOR;
        while (buf->byte_count + required_bytes >= new_cap) {
            new_cap *= GROWTH_FACTOR;
        }
        uint8_t *new_ptr = (uint8_t*)realloc(buf->data, new_cap);
        if (!new_ptr) {
            fprintf(stderr, "Error reallocating output buffer.\n");
            exit(1);
        }
        buf->data = new_ptr;
        buf->capacity = new_cap;
    }
}

/* Os valores são símbolo+1; entradas zeradas continuam inválidas. */
static const uint8_t char_to_symbol[256] = {
    [','] = 1, ['-'] = 2,
    ['1'] = 3, ['2'] = 4, ['3'] = 5, ['4'] = 6, ['5'] = 7,
    ['6'] = 8, ['7'] = 9, ['8'] = 10, ['9'] = 11, ['0'] = 12,
    ['^'] = 13, ['~'] = 14, ['('] = 15, [')'] = 16
};

/* Códigos Rice(k=1) dos símbolos 0..15, alinhados à direita. */
static const uint16_t rice_code[16] = {
    0x000, 0x001, 0x004, 0x005, 0x00C, 0x00D, 0x01C, 0x01D,
    0x03C, 0x03D, 0x07C, 0x07D, 0x0FC, 0x0FD, 0x1FC, 0x1FD
};
static const uint8_t rice_code_len[16] = {
    2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9
};

static inline void rice_writer_init(RiceBuffer *rb, Binary4BitBuffer *output) {
    rb->output = output;
    rb->bits = 0;
    rb->bit_count = 0;
}

static inline void rice_write_symbol(RiceBuffer *rb, uint8_t symbol) {
    unsigned len = rice_code_len[symbol];
    rb->bits = (rb->bits << len) | rice_code[symbol];
    rb->bit_count += len;

    while (rb->bit_count >= 8) {
        rb->bit_count -= 8;
        ensure_4bit_capacity(rb->output, 1);
        rb->output->data[rb->output->byte_count++] =
            (uint8_t)(rb->bits >> rb->bit_count);
        if (rb->bit_count == 0) rb->bits = 0;
        else rb->bits &= (UINT64_C(1) << rb->bit_count) - 1;
    }
}

static inline void rice_write_char(RiceBuffer *rb, char c) {
    uint8_t encoded = char_to_symbol[(uint8_t)c];
    if (encoded != 0) rice_write_symbol(rb, (uint8_t)(encoded - 1));
}

static void rice_write_u64(RiceBuffer *rb, uint64_t value) {
    char digits[20];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0) rice_write_char(rb, digits[--count]);
}

static void rice_write_i32(RiceBuffer *rb, int32_t value) {
    uint64_t magnitude;
    if (value < 0) {
        rice_write_char(rb, '-');
        magnitude = (uint64_t)(-(int64_t)value);
    } else {
        magnitude = (uint32_t)value;
    }
    rice_write_u64(rb, magnitude);
}

static inline void rice_write_token(RiceBuffer *rb, int32_t value,
                                    char operation, uint64_t argument) {
    rice_write_i32(rb, value);
    if (operation != '\0') {
        rice_write_char(rb, operation);
        rice_write_u64(rb, argument);
    }
    rice_write_char(rb, ',');
}

static void rice_writer_finish(RiceBuffer *rb) {
    if (rb->bit_count != 0) {
        ensure_4bit_capacity(rb->output, 1);
        rb->output->data[rb->output->byte_count++] =
            (uint8_t)(rb->bits << (8 - rb->bit_count));
        rb->bits = 0;
        rb->bit_count = 0;
    }
}

// ============================================================================
// CANAL
// ============================================================================

void init_channel(Channel *ch, size_t initial_capacity) {
    ch->capacity = initial_capacity;
    ch->count = 0;
    ch->samples = (int32_t*)malloc(ch->capacity * sizeof(int32_t));
    if (!ch->samples) {
        fprintf(stderr, "Error allocating channel\n");
        exit(1);
    }
}

void ensure_channel_capacity(Channel *ch, size_t required) {
    if (ch->count + required >= ch->capacity) {
        size_t new_cap = ch->capacity * GROWTH_FACTOR;
        while (ch->count + required >= new_cap) {
            new_cap *= GROWTH_FACTOR;
        }
        int32_t *new_ptr = (int32_t*)realloc(ch->samples, new_cap * sizeof(int32_t));
        if (!new_ptr) {
            fprintf(stderr, "Error reallocating channel\n");
            exit(1);
        }
        ch->samples = new_ptr;
        ch->capacity = new_cap;
    }
}

// ============================================================================
// DELTA ENCODING
// ============================================================================

void apply_delta_encoding(Channel *ch, int32_t **deltas, size_t *delta_count) {
    if (ch->count == 0) {
        *deltas = NULL;
        *delta_count = 0;
        return;
    }
    
    *delta_count = ch->count;
    *deltas = (int32_t*)malloc(ch->count * sizeof(int32_t));
    if (!*deltas) {
        fprintf(stderr, "Error allocating delta buffer\n");
        exit(1);
    }
    
    // Primeiro valor é absoluto
    (*deltas)[0] = ch->samples[0];
    
    // Demais valores são deltas (diferença entre consecutivos)
    for (size_t i = 1; i < ch->count; i++) {
        (*deltas)[i] = ch->samples[i] - ch->samples[i - 1];
    }
    
    // Debug: mostra primeiros deltas
    if (ch->count >= 10) {
        printf("   First deltas: %d, %d, %d, %d, %d...\n", 
               (*deltas)[0], (*deltas)[1], (*deltas)[2], (*deltas)[3], (*deltas)[4]);
    }
}

// ============================================================================
// LEITURA DE ÁUDIO
// ============================================================================

int precisa_converter(const char *filename) {
    const char *ext = strrchr(filename, '.');
    return !ext ||
           !((ext[1] == 'w' || ext[1] == 'W') &&
             (ext[2] == 'a' || ext[2] == 'A') &&
             (ext[3] == 'v' || ext[3] == 'V') &&
              ext[4] == '\0');
}

int convert_to_wav_temp(const char *audio_file, char *temp_wav) {
    sprintf(temp_wav, "temp_txac_%d.wav", (int)time(NULL));
    const char *ext = strrchr(audio_file, '.');
    const char *formato = ext ? ext + 1 : "áudio";
    
    char cmd[2048];
    sprintf(cmd, "ffmpeg -i \"%s\" -f wav -acodec pcm_s32le -rf64 never \"%s\" -y -loglevel error", 
            audio_file, temp_wav);
    
    printf("Converting %s to WAV 32-bit...\n", formato);
    if (system(cmd) != 0) {
        fprintf(stderr, "Error converting FFmpeg\n");
        return 0;
    }
    return 1;
}

int ler_wav_multicanal(const char *arquivo, Channel channels[], TXACHeader *header) {
    FILE *f = fopen(arquivo, "rb");
    if (!f) {
        perror("Error opening WAV");
        return 0;
    }

    uint8_t hdr[44];
    if (fread(hdr, 1, 44, f) != 44) {
        fclose(f);
        return 0;
    }

    memcpy(&header->sample_rate, hdr + 24, 4);
    memcpy(&header->channels, hdr + 22, 2);
    memcpy(&header->bits_per_sample, hdr + 34, 2);

    printf("WAV Info: %d Hz, %d canais, %d bits\n", 
           header->sample_rate, header->channels, header->bits_per_sample);

    fseek(f, 12, SEEK_SET);
    uint8_t chunk_id[4];
    uint32_t chunk_size;
    int found = 0;
    
    while (fread(chunk_id, 1, 4, f) == 4) {
        fread(&chunk_size, 4, 1, f);
        
        if (memcmp(chunk_id, "data", 4) == 0) {
            printf("Chunk 'data' found (%u bytes)\n", chunk_size);
            found = 1;
            break;
        }
        
        if (chunk_size > 0 && chunk_size < 0x7FFFFFFF) {
            fseek(f, chunk_size, SEEK_CUR);
        } else break;
    }
    
    if (!found) {
        fprintf(stderr, "Chunk 'data' not found\n");
        fclose(f);
        return 0;
    }

    size_t bytes_per_sample = header->bits_per_sample / 8;
    size_t samples_per_channel =
        bytes_per_sample && header->channels
        ? (size_t)chunk_size / bytes_per_sample / header->channels
        : 512 * 1024;
    if (samples_per_channel == 0) samples_per_channel = 1;

    for (int i = 0; i < header->channels; i++) {
        init_channel(&channels[i], samples_per_channel);
    }

    float fator_f = (float)pow(10.0, -DB_REDUCTION / 20.0);

    if (header->bits_per_sample == 32) {
        int32_t buffer[8 * MAX_CHANNELS];
        size_t num_read;
        
        while ((num_read = fread(buffer, sizeof(int32_t), 8 * header->channels, f)) > 0) {
            for (size_t i = 0; i < num_read; i++) {
                int ch = i % header->channels;
                int32_t s32 = buffer[i];
                int32_t reduced = (int32_t)(s32 * fator_f);
                
                ensure_channel_capacity(&channels[ch], 1);
                channels[ch].samples[channels[ch].count++] = reduced;
            }
        }
    }
    else if (header->bits_per_sample == 16) {
        int16_t buffer[8 * MAX_CHANNELS];
        size_t num_read;
        
        while ((num_read = fread(buffer, sizeof(int16_t), 8 * header->channels, f)) > 0) {
            for (size_t i = 0; i < num_read; i++) {
                int ch = i % header->channels;
                int16_t s16 = buffer[i];
                int32_t s32 = ((int32_t)s16) << 16;
                int32_t reduced = (int32_t)(s32 * fator_f);
                
                ensure_channel_capacity(&channels[ch], 1);
                channels[ch].samples[channels[ch].count++] = reduced;
            }
        }
    } else {
        fprintf(stderr, "Error: Only 16-bit and 32-bit supported. File is %d-bit.\n", header->bits_per_sample);
        fclose(f);
        return 0;
    }

    fclose(f);
    header->total_samples = channels[0].count;
    printf("Done reading: %zu samples per channel\n", channels[0].count);
    return 1;
}

// ============================================================================
// COMPRESSÃO COM DELTA
// ============================================================================

void *compactar_canal_4bit_thread(void *arg) {
    ThreadData *td = (ThreadData*)arg;
    Channel *ch = td->channel;
    Binary4BitBuffer *out = td->output;
    
    init_4bit_buffer(out);
    RiceBuffer rice_out;
    rice_writer_init(&rice_out, out);
    
    printf("  [Channel %d] Applying delta encoding...\n", td->channel_id);
    
    int32_t *deltas = NULL;
    size_t delta_count = 0;
    apply_delta_encoding(ch, &deltas, &delta_count);
    
    if (!deltas || delta_count == 0) {
        printf("  [Channel %d] Error: no deltas generated\n", td->channel_id);
        return NULL;
    }
    
    printf("  [Channel %d] Compressing %zu deltas to 4-bit...\n", td->channel_id, delta_count);
    
    size_t i = 0;
    
while (i < delta_count) {
        int32_t atual = deltas[i];
        
        // 1. Tenta repetição IMEDIATA (^)
        size_t count = 1;
        while (i + count < delta_count && deltas[i + count] == atual) {
            count++;
        }

        if (count >= 2) {
            rice_write_token(&rice_out, atual, '^', count);
            i += count;
            continue;
        }

        // 2. Tenta Sniper (~) com Look-ahead de 100 samples usando AVX2
        int sniper_found = 0;
        if (!td->enable_loop_compression) {
            rice_write_token(&rice_out, atual, '\0', 0);
            i++;
            continue;
        }
        int janela = 100; // O "100 à frente" que você pediu
        int limite_busca = (i + janela < delta_count) ? i + janela : delta_count - 1;

        // Procura a primeira aparição do valor 'atual' no futuro próximo (mínimo dist 2)
        int found_idx = find_next_match_avx2(deltas, i + 2, limite_busca, atual);

        if (found_idx != -1) {
            int dist = found_idx - i;
            
            // --- CHECAGEM DE EFICIÊNCIA ---
            // Verificamos se dentro desse "buraco" do sniper existe alguma repetição (^)
            // Se existir, é melhor NÃO usar o sniper agora para não quebrar a repetição futura.
            int rep_no_caminho = 0;
            for (int j = i + 1; j < found_idx - 1; j++) {
                if (deltas[j] == deltas[j + 1]) {
                    rep_no_caminho = 1;
                    break;
                }
            }

            if (!rep_no_caminho) {
                // Aplica o Sniper: Valor~Distancia,
                rice_write_token(&rice_out, atual, '~', (uint64_t)(dist - 1));
                
                // Escreve os valores que ficaram no meio
                for (int j = i + 1; j < found_idx; j++) {
                    rice_write_token(&rice_out, deltas[j], '\0', 0);
                }
                
                i = found_idx + 1; // Pula para depois do valor encontrado
                sniper_found = 1;
            }
        }

        // 3. Fallback: Se nada funcionou, escreve o valor simples
        if (!sniper_found) {
            rice_write_token(&rice_out, atual, '\0', 0);
            i++;
        }
    }
    rice_writer_finish(&rice_out);
    
    printf("  [Channel %d] Compressed: %zu bytes (4-bit delta + rice)\n", td->channel_id, out->byte_count);
    
    free(deltas);
    return NULL;
}  

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("\nUsage: %s <input> <output.txac> [--loop]\n", argv[0]);
        return 1;
    }

    const char *input = argv[1];
    const char *output = argv[2];
    int enable_loop = (argc >= 4 && strcmp(argv[3], "--loop") == 0);

    printf("\n=== TXAC Encoder v0.3.0 (Delta Encoding) ===\n");

    char temp_wav[256] = {0};
    int is_temp = 0;

    if (precisa_converter(input)) {
        if (!convert_to_wav_temp(input, temp_wav)) return 1;
        input = temp_wav;
        is_temp = 1;
    }

    Channel channels[MAX_CHANNELS];
    TXACHeader header = {0};
    
    if (!ler_wav_multicanal(input, channels, &header)) {
        if (is_temp) remove(temp_wav);
        return 1;
    }

    printf("\nCompressing %d channels with delta encoding...\n", header.channels);
    
    pthread_t threads[MAX_CHANNELS];
    ThreadData thread_data[MAX_CHANNELS];
    Binary4BitBuffer outputs[MAX_CHANNELS];
    
    for (int i = 0; i < header.channels; i++) {
        thread_data[i].channel = &channels[i];
        thread_data[i].output = &outputs[i];
        thread_data[i].channel_id = i;
        thread_data[i].enable_loop_compression = enable_loop;
        
        pthread_create(&threads[i], NULL, compactar_canal_4bit_thread, &thread_data[i]);
    }
    
    for (int i = 0; i < header.channels; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("\nSaving TXAC file...\n");
    FILE *fout = fopen(output, "wb");
    if (!fout) {
        perror("Error creating file");
        return 1;
    }

    fwrite(TXAC_MAGIC, 1, 4, fout);
    uint32_t version = TXAC_VERSION;
    fwrite(&version, 4, 1, fout);
    fwrite(&header.sample_rate, 4, 1, fout);
    fwrite(&header.channels, 2, 1, fout);
    
    uint16_t save_bits = 32;
    fwrite(&save_bits, 2, 1, fout);
    
    uint32_t flags = enable_loop ? 1 : 0;
    flags |= (1 << 1); // Delta encoding flag
    fwrite(&flags, 4, 1, fout);
    fwrite(&header.total_samples, 8, 1, fout);
    
    uint8_t reserved[36] = {0};
    fwrite(reserved, 1, 36, fout);

    long index_pos = ftell(fout);
    uint64_t offsets[MAX_CHANNELS] = {0};
    uint64_t sizes[MAX_CHANNELS] = {0};
    
    for (int i = 0; i < header.channels; i++) {
        fwrite(&offsets[i], 8, 1, fout);
        fwrite(&sizes[i], 8, 1, fout);
    }

    for (int i = 0; i < header.channels; i++) {
        offsets[i] = ftell(fout);
        fwrite(outputs[i].data, 1, outputs[i].byte_count, fout);
        sizes[i] = outputs[i].byte_count;
    }

    fseek(fout, index_pos, SEEK_SET);
    for (int i = 0; i < header.channels; i++) {
        fwrite(&offsets[i], 8, 1, fout);
        fwrite(&sizes[i], 8, 1, fout);
    }

    fclose(fout);

    printf("\nChannel compression results:\n");
    for (int i = 0; i < header.channels; i++) {
        printf("  Channel %d: %llu bytes\n", i,
               (unsigned long long)sizes[i]);
        free(channels[i].samples);
        free(outputs[i].data);
    }
    
    if (is_temp) remove(temp_wav);

    printf("\nEncoding complete!\n");
    return 0;
}
