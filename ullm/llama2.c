/*
 * MIT License
 *
 * Copyright (c) 2023 Andrej
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "sys/memory.h"
#include "ullm/llama2.h"
#include "util/log.h"

#define ULLM_LOG_TAG "llama2"

static UllmStatus UllmLlama2MallocRunState(UllmLlama2Transformer* t) {
  UllmLlama2RunState* s = &t->state;
  UllmLlama2Config* p = &t->config;
  int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
  s->x = UllmMemoryAlloc(p->dim * sizeof(float));
  s->xb = UllmMemoryAlloc(p->dim * sizeof(float));
  s->xb2 = UllmMemoryAlloc(p->dim * sizeof(float));
  s->hb = UllmMemoryAlloc(p->hidden_dim * sizeof(float));
  s->hb2 = UllmMemoryAlloc(p->hidden_dim * sizeof(float));
  s->q = UllmMemoryAlloc(p->dim * sizeof(float));
  s->key_cache = UllmMemoryAlloc(p->n_layers * p->seq_len * kv_dim * sizeof(float));
  s->value_cache = UllmMemoryAlloc(p->n_layers * p->seq_len * kv_dim * sizeof(float));
  s->att = UllmMemoryAlloc(p->n_heads * p->seq_len * sizeof(float));
  s->logits = UllmMemoryAlloc(p->vocab_size * sizeof(float));
  if (!s->x || !s->xb || !s->xb2 || !s->hb || !s->hb2 || !s->q || !s->key_cache
      || !s->value_cache || !s->att || !s->logits) {
    ULOGE("Failed to allocate run state");
    return ULLM_STATUS_OOM;
  }

  return ULLM_STATUS_OK;
}

static void memory_map_weights(UllmLlama2TransformerWeights *w, UllmLlama2Config* p, float* ptr, int shared_weights) {
  int head_size = p->dim / p->n_heads;
  uint64_t n_layers = p->n_layers;
  w->token_embedding_table = ptr;
  ptr += p->vocab_size * p->dim;
  w->rms_att_weight = ptr;
  ptr += n_layers * p->dim;
  w->wq = ptr;
  ptr += n_layers * p->dim * (p->n_heads * head_size);
  w->wk = ptr;
  ptr += n_layers * p->dim * (p->n_kv_heads * head_size);
  w->wv = ptr;
  ptr += n_layers * p->dim * (p->n_kv_heads * head_size);
  w->wo = ptr;
  ptr += n_layers * (p->n_heads * head_size) * p->dim;
  w->rms_ffn_weight = ptr;
  ptr += n_layers * p->dim;
  w->w1 = ptr;
  ptr += n_layers * p->dim * p->hidden_dim;
  w->w2 = ptr;
  ptr += n_layers * p->hidden_dim * p->dim;
  w->w3 = ptr;
  ptr += n_layers * p->dim * p->hidden_dim;
  w->rms_final_weight = ptr;
  ptr += p->dim;
  ptr += p->seq_len * head_size / 2; // skip what used to be freq_cis_real (for RoPE)
  ptr += p->seq_len * head_size / 2; // skip what used to be freq_cis_imag (for RoPE)
  w->wcls = shared_weights ? w->token_embedding_table : ptr;
}

static UllmStatus UllmLlama2ReadCheckpoint(const UllmLlama2RunConfig* config,
    UllmLlama2Transformer* t) {
  FILE *file = fopen(config->checkpoint_path, "rb");
  if (!file) {
    ULOGE("Failed to read checkpoint '%s'", config->checkpoint_path);
    return ULLM_STATUS_INVALID_ARGUMENT;
  }

  if (fread(&t->config, sizeof(UllmLlama2Config), 1, file) != 1) {
    ULOGE("Failed to read config header");
    return ULLM_STATUS_IO_ERROR;
  }

  // negative vocab size is hacky way of signaling unshared weights. bit yikes.
  int shared_weights = t->config.vocab_size > 0 ? 1 : 0;
  t->config.vocab_size = abs(t->config.vocab_size);

  // figure out the file size
  fseek(file, 0, SEEK_END); // move file pointer to end of file
  t->file_size = ftell(file); // get the file size, in bytes
  fclose(file);

  // memory map the Transformer weights into the data pointer
  t->fd = open(config->checkpoint_path, O_RDONLY); // open in read only mode
  if (t->fd == -1) {
    ULOGE("Failed to open checkpoint '%s'", config->checkpoint_path);
    return ULLM_STATUS_INVALID_ARGUMENT;
  }

  t->data = mmap(NULL, t->file_size, PROT_READ, MAP_PRIVATE, t->fd, 0);
  if (t->data == MAP_FAILED) {
    ULOGE("Failed to mmap");
    return ULLM_STATUS_IO_ERROR;
  }

  float* weights_ptr = t->data + sizeof(UllmLlama2Config) / sizeof(float);
  memory_map_weights(&t->weights, &t->config, weights_ptr, shared_weights);
  return ULLM_STATUS_OK;
}

static UllmStatus UllmLlama2BuildTransformer(const UllmLlama2RunConfig* config,
    UllmLlama2State* state) {
  UllmLlama2Transformer* t = &state->transformer;
  ULLM_RETURN_IF_ERROR(UllmLlama2ReadCheckpoint(config, t));
  if (config->steps > t->config.seq_len) {
    ULOGE("steps out of range: %u vs %" PRIu32,
        config->steps, state->transformer.config.seq_len);
    return ULLM_STATUS_INVALID_ARGUMENT;
  }

  return UllmLlama2MallocRunState(t);
}

static void UllmLlama2FreeTransformer(UllmLlama2Transformer* t) {
  if (t->data != MAP_FAILED) { munmap(t->data, t->file_size); }
  if (t->fd != -1) { close(t->fd); }
  UllmMemoryFree(t->state.x);
  UllmMemoryFree(t->state.xb);
  UllmMemoryFree(t->state.xb2);
  UllmMemoryFree(t->state.hb);
  UllmMemoryFree(t->state.hb2);
  UllmMemoryFree(t->state.q);
  UllmMemoryFree(t->state.att);
  UllmMemoryFree(t->state.logits);
  UllmMemoryFree(t->state.key_cache);
  UllmMemoryFree(t->state.value_cache);
}

// ----------------------------------------------------------------------------
// neural net blocks; the dynamics of the Transformer

void rmsnorm(float* o, float* x, float* weight, int size) {
  // calculate sum of squares
  float ss = 0.0f;
  for (int j = 0; j < size; j++) {
      ss += x[j] * x[j];
  }
  ss /= size;
  ss += 1e-5f;
  ss = 1.0f / sqrtf(ss);
  // normalize and scale
  for (int j = 0; j < size; j++) {
      o[j] = weight[j] * (ss * x[j]);
  }
}

void softmax(float* x, int size) {
  // find max value (for numerical stability)
  float max_val = x[0];
  for (int i = 1; i < size; i++) {
    if (x[i] > max_val) {
      max_val = x[i];
    }
  }
  // exp and sum
  float sum = 0.0f;
  for (int i = 0; i < size; i++) {
    x[i] = expf(x[i] - max_val);
    sum += x[i];
  }
  // normalize
  for (int i = 0; i < size; i++) {
    x[i] /= sum;
  }
}

void matmul(float* xout, float* x, float* w, int n, int d) {
  // W (d,n) @ x (n,) -> xout (d,)
  // by far the most amount of time is spent inside this little function
  for (int i = 0; i < d; i++) {
    float val = 0.0f;
    for (int j = 0; j < n; j++) {
      val += w[i * n + j] * x[j];
    }
    xout[i] = val;
  }
}

float* forward(UllmLlama2Transformer* transformer, int token, int pos) {
  // a few convenience variables
  UllmLlama2Config* p = &transformer->config;
  UllmLlama2TransformerWeights* w = &transformer->weights;
  UllmLlama2RunState* s = &transformer->state;
  float *x = s->x;
  int dim = p->dim;
  int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
  int kv_mul = p->n_heads / p->n_kv_heads; // integer multiplier of the kv sharing in multiquery
  int hidden_dim =  p->hidden_dim;
  int head_size = dim / p->n_heads;

  // copy the token embedding into x
  float* content_row = w->token_embedding_table + token * dim;
  memcpy(x, content_row, dim * sizeof(*x));

  // forward all the layers
  for(unsigned long long l = 0; l < p->n_layers; l++) {
    // attention rmsnorm
    rmsnorm(s->xb, x, w->rms_att_weight + l*dim, dim);

    // key and value point to the kv cache
    int loff = l * p->seq_len * kv_dim; // kv cache layer offset for convenience
    s->k = s->key_cache + loff + pos * kv_dim;
    s->v = s->value_cache + loff + pos * kv_dim;

    // qkv matmuls for this position
    matmul(s->q, s->xb, w->wq + l*dim*dim, dim, dim);
    matmul(s->k, s->xb, w->wk + l*dim*kv_dim, dim, kv_dim);
    matmul(s->v, s->xb, w->wv + l*dim*kv_dim, dim, kv_dim);

    // RoPE relative positional encoding: complex-valued rotate q and k in each head
    for (int i = 0; i < dim; i+=2) {
      int head_dim = i % head_size;
      float freq = 1.0f / powf(10000.0f, head_dim / (float)head_size);
      float val = pos * freq;
      float fcr = cosf(val);
      float fci = sinf(val);
      int rotn = i < kv_dim ? 2 : 1; // how many vectors? 2 = q & k, 1 = q only
      for (int v = 0; v < rotn; v++) {
        float* vec = v == 0 ? s->q : s->k; // the vector to rotate (query or key)
        float v0 = vec[i];
        float v1 = vec[i+1];
        vec[i]   = v0 * fcr - v1 * fci;
        vec[i+1] = v0 * fci + v1 * fcr;
      }
    }

    // multihead attention. iterate over all heads
    for (int h = 0; h < p->n_heads; h++) {
      // get the query vector for this head
      float* q = s->q + h * head_size;
      // attention scores for this head
      float* att = s->att + h * p->seq_len;
      // iterate over all timesteps, including the current one
      for (int t = 0; t <= pos; t++) {
        // get the key vector for this head and at this timestep
        float* k = s->key_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
        // calculate the attention score as the dot product of q and k
        float score = 0.0f;
        for (int i = 0; i < head_size; i++) {
          score += q[i] * k[i];
        }
        score /= sqrtf(head_size);
        // save the score to the attention buffer
        att[t] = score;
      }

      // softmax the scores to get attention weights, from 0..pos inclusively
      softmax(att, pos + 1);

      // weighted sum of the values, store back into xb
      float* xb = s->xb + h * head_size;
      memset(xb, 0, head_size * sizeof(float));
      for (int t = 0; t <= pos; t++) {
        // get the value vector for this head and at this timestep
        float* v = s->value_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
        // get the attention weight for this timestep
        float a = att[t];
        // accumulate the weighted value into xb
        for (int i = 0; i < head_size; i++) {
          xb[i] += a * v[i];
        }
      }
    }

    // final matmul to get the output of the attention
    matmul(s->xb2, s->xb, w->wo + l*dim*dim, dim, dim);

    // residual connection back into x
    for (int i = 0; i < dim; i++) {
      x[i] += s->xb2[i];
    }

    // ffn rmsnorm
    rmsnorm(s->xb, x, w->rms_ffn_weight + l*dim, dim);

    // Now for FFN in PyTorch we have: self.w2(F.silu(self.w1(x)) * self.w3(x))
    // first calculate self.w1(x) and self.w3(x)
    matmul(s->hb, s->xb, w->w1 + l*dim*hidden_dim, dim, hidden_dim);
    matmul(s->hb2, s->xb, w->w3 + l*dim*hidden_dim, dim, hidden_dim);

    // SwiGLU non-linearity
    for (int i = 0; i < hidden_dim; i++) {
      float val = s->hb[i];
      // silu(x)=x*σ(x), where σ(x) is the logistic sigmoid
      val *= (1.0f / (1.0f + expf(-val)));
      // elementwise multiply with w3(x)
      val *= s->hb2[i];
      s->hb[i] = val;
    }

    // final matmul to get the output of the ffn
    matmul(s->xb, s->hb, w->w2 + l*dim*hidden_dim, hidden_dim, dim);

    // residual connection
    for (int i = 0; i < dim; i++) {
      x[i] += s->xb[i];
    }
  }

  // final rmsnorm
  rmsnorm(x, x, w->rms_final_weight, dim);

  // classifier into logits
  matmul(s->logits, x, w->wcls, p->dim, p->vocab_size);
  return s->logits;
}

// ----------------------------------------------------------------------------
// The Byte Pair Encoding (BPE) Tokenizer that translates strings <-> tokens

int compare_tokens(const void *a, const void *b) {
    return strcmp(((UllmLlama2TokenIndex*)a)->str, ((UllmLlama2TokenIndex*)b)->str);
}

UllmStatus UllmLlama2BuildTokenizer(const UllmLlama2RunConfig* config,
    UllmLlama2State* state) {
  // TODO(aarossig): Check UllmMemoryAllocs
  // UllmMemoryAlloc space to hold the scores and the strings
  UllmLlama2Tokenizer* t = &state->tokenizer;
  const int32_t vocab_size = state->transformer.config.vocab_size;
  t->vocab = (char**)UllmMemoryAlloc(vocab_size * sizeof(char*));
  if (t->vocab == NULL) {
    ULOGE("Failed to allocate vocab buffer");
    return ULLM_STATUS_OOM;
  }

  t->vocab_scores = (float*)UllmMemoryAlloc(vocab_size * sizeof(float));
  if (t->vocab_scores == NULL) {
    ULOGE("Failed to allocate vocab_scores buffer");
    return ULLM_STATUS_OOM;
  }

  t->sorted_vocab = UllmMemoryAlloc(vocab_size * sizeof(UllmLlama2TokenIndex));
  if (t->sorted_vocab == NULL) {
    ULOGE("Failed to allocate sorted_vocab buffer");
    return ULLM_STATUS_OOM;
  }

  FILE *file = fopen(config->tokenizer_path, "rb");
  if (!file) {
    ULOGE("Failed to load tokenizer '%s'", config->tokenizer_path);
    return ULLM_STATUS_INVALID_ARGUMENT;
  }

  uint32_t max_token_length;
  if (fread(&max_token_length, sizeof(uint32_t), 1, file) != 1) {
    ULOGE("Failed to read max token length");
    return ULLM_STATUS_IO_ERROR;
  }

  // Create a temporary buffer that will store merge candidates of always two
  // consecutive tokens. Double for concat, +1 for null terminator +2 for UTF8
  // (in case max_token_length is 1)
  size_t token_buffer_size = max_token_length * 2 + 1 + 1;
  t->token_buffer = UllmMemoryAlloc(token_buffer_size);
  if (t->token_buffer == NULL) {
    ULOGE("Failed to allocate token_buffer");
    return ULLM_STATUS_OOM;
  }

  for (int i = 0; i < vocab_size; i++) {
    if (fread(t->vocab_scores + i, sizeof(float), 1, file) != 1) {
      ULOGE("Failed to read vocab scores");
      return ULLM_STATUS_IO_ERROR;
    }

    int len;
    if (fread(&len, sizeof(int), 1, file) != 1) {
      ULOGE("Failed to read len");
      return ULLM_STATUS_IO_ERROR;
    }

    t->vocab[i] = (char *)UllmMemoryAlloc(len + 1);
    if (fread(t->vocab[i], len, 1, file) != 1) {
      ULOGE("Failed to read vocab");
      return ULLM_STATUS_IO_ERROR;
    }

    t->vocab[i][len] = '\0'; // add the string terminating token
  }

  for (int i = 0; i < vocab_size; i++) {
      t->sorted_vocab[i].str = t->vocab[i];
      t->sorted_vocab[i].id = i;
  }
  qsort(t->sorted_vocab, vocab_size, sizeof(UllmLlama2TokenIndex), compare_tokens);

  fclose(file);
  return ULLM_STATUS_OK;
}

static void UllmLlama2FreeTokenizer(UllmLlama2State* state) {
  UllmLlama2Tokenizer* t = &state->tokenizer;
  for (int i = 0; i < state->transformer.config.vocab_size; i++) {
    UllmMemoryFree(t->vocab[i]);
  }
  UllmMemoryFree(t->vocab);
  UllmMemoryFree(t->vocab_scores);
  UllmMemoryFree(t->sorted_vocab);
  UllmMemoryFree(t->token_buffer);
}

static void UllmLlama2EmitPiece(const UllmLlama2RunConfig* config,
    const char *piece) {
  // Filter out empty, invalid tokens, or non-printable characers.
  if (config->output_callback == NULL || piece == NULL || piece[0] == '\0'
      || (piece[1] == '\0' && !isprint(piece[0]) && !isspace(piece[0]))) {
    return;
  }

  config->output_callback(piece, config->cookie);
}

static void UllmLlama2Decode(const UllmLlama2RunConfig* config,
    UllmLlama2Tokenizer* t, int prev_token, int token) {
  // following BOS (1) token strip leading whitespace
  const char *piece = t->vocab[token];
  if (prev_token == 1 && piece[0] == ' ') {
    piece++;
  }

  // careful, some tokens designate raw bytes, and look like e.g. '<0x01>'
  // parse this and convert and return the actual byte
  unsigned char byte_val;
  if (sscanf(piece, "<0x%02hhX>", &byte_val) == 1) {
    char byte_piece[2] = {};
    byte_piece[0] = byte_val;
    UllmLlama2EmitPiece(config, byte_piece);
  } else {
    UllmLlama2EmitPiece(config, piece);
  }
}

int str_lookup(const char *str, UllmLlama2TokenIndex *sorted_vocab, int vocab_size) {
  // efficiently find the perfect match for str in vocab, return its index or -1 if not found
  UllmLlama2TokenIndex tok = { .str = str }; // acts as the key to search for
  UllmLlama2TokenIndex *res = bsearch(&tok, sorted_vocab, vocab_size, sizeof(UllmLlama2TokenIndex), compare_tokens);
  return res != NULL ? res->id : -1;
}

static void UllmLlama2Encode(const UllmLlama2RunConfig* config,
    UllmLlama2State* state, int8_t bos, int8_t eos, int *tokens,
    int *n_tokens) {
  // encode the string text (input) into an upper-bound preallocated tokens[] array
  // bos != 0 means prepend the BOS token (=1), eos != 0 means append the EOS token (=2)
  int32_t vocab_size = state->transformer.config.vocab_size;
  UllmLlama2Tokenizer* t = &state->tokenizer;
  size_t str_len = 0;

  // start at 0 tokens
  *n_tokens = 0;

  // add optional BOS (=1) token, if desired
  if (bos) {
    tokens[(*n_tokens)++] = 1;
  }

  // add_dummy_prefix is true by default
  // so prepend a dummy prefix token to the input string, but only if text != ""
  // TODO: pretty sure this isn't correct in the general case but I don't have the
  // energy to read more of the sentencepiece code to figure out what it's doing
  if (config->prompt[0] != '\0') {
    int dummy_prefix = str_lookup(" ", t->sorted_vocab, vocab_size);
    tokens[(*n_tokens)++] = dummy_prefix;
  }

  // Okay UTF-8 time. This will get messy. Here is the reference from Wikipedia:
  // Code point ↔ UTF-8 conversion
  // First code point	Last code point	Byte 1	Byte 2	Byte 3	Byte 4
  // U+0000	U+007F	    0xxxxxxx
  // U+0080	U+07FF	    110xxxxx	10xxxxxx
  // U+0800	U+FFFF	    1110xxxx	10xxxxxx	10xxxxxx
  // U+10000	U+10FFFF    11110xxx	10xxxxxx	10xxxxxx	10xxxxxx

  // process the raw (UTF-8) byte sequence of the input string
  for (const char *c = config->prompt; *c != '\0'; c++) {
    // reset buffer if the current byte is ASCII or a leading byte
    // 0xC0 is 11000000, so (*c & 0xC0) keeps the first 2 bits and zeros the rest
    // 0x80 is 10000000
    // in UTF-8, all continuation bytes start with "10" in first two bits
    // so in English this is: "if this byte is not a continuation byte"
    if ((*c & 0xC0) != 0x80) {
      // this byte must be either a leading byte (11...) or an ASCII char (0x...)
      // => reset our location, as we're starting a new UTF-8 codepoint
      str_len = 0;
    }

    // append the current byte to the buffer
    t->token_buffer[str_len++] = *c; // ++ is post-increment, incremented after this line
    t->token_buffer[str_len] = '\0';

    // while the next character is a continuation byte, continue appending
    // but if there are too many of them, just stop to avoid overruning
    // token_buffer size.
    if ((*(c+1) & 0xC0) == 0x80 && str_len < 4) {
      continue;
    }

    // ok c+1 is not a continuation byte, so we've read in a full codepoint
    int id = str_lookup(t->token_buffer, t->sorted_vocab, vocab_size);

    if (id != -1) {
      // we found this codepoint in vocab, add it as a token
      tokens[(*n_tokens)++] = id;
    } else {
      // byte_fallback encoding: just encode each byte as a token
      // +3 is here because the first 3 vocab elements are <unk>, <s>, </s>
      // so the individual bytes only start at index 3
      for (int i=0; i < str_len; i++) {
        tokens[(*n_tokens)++] = (unsigned char)t->token_buffer[i] + 3;
      }
    }
    str_len = 0; // protect against a sequence of stray UTF8 continuation bytes
  }

  // merge the best consecutive pair each iteration, according the scores in vocab_scores
  while (1) {
    float best_score = -1e10;
    int best_id = -1;
    int best_idx = -1;

    for (int i=0; i < (*n_tokens-1); i++) {
      // check if we can merge the pair (tokens[i], tokens[i+1])
      sprintf(t->token_buffer, "%s%s", t->vocab[tokens[i]], t->vocab[tokens[i+1]]);
      int id = str_lookup(t->token_buffer, t->sorted_vocab, vocab_size);
      if (id != -1 && t->vocab_scores[id] > best_score) {
        // this merge pair exists in vocab! record its score and position
        best_score = t->vocab_scores[id];
        best_id = id;
        best_idx = i;
      }
    }

    if (best_idx == -1) {
      break; // we couldn't find any more pairs to merge, so we're done
    }

    // merge the consecutive pair (best_idx, best_idx+1) into new token best_id
    tokens[best_idx] = best_id;
    // delete token at position best_idx+1, shift the entire sequence back 1
    for (int i = best_idx+1; i < (*n_tokens-1); i++) {
      tokens[i] = tokens[i+1];
    }
    (*n_tokens)--; // token length decreased
  }

  // add optional EOS (=2) token, if desired
  if (eos) {
    tokens[(*n_tokens)++] = 2;
  }
}

// ----------------------------------------------------------------------------
// The Sampler, which takes logits and returns a sampled token
// sampling can be done in a few ways: greedy argmax, sampling, top-p sampling

int sample_argmax(float* probabilities, int n) {
    // return the index that has the highest probability
    int max_i = 0;
    float max_p = probabilities[0];
    for (int i = 1; i < n; i++) {
        if (probabilities[i] > max_p) {
            max_i = i;
            max_p = probabilities[i];
        }
    }
    return max_i;
}

int sample_mult(float* probabilities, int n, float coin) {
    // sample index from probabilities (they must sum to 1!)
    // coin is a random number in [0, 1), usually from random_f32()
    float cdf = 0.0f;
    for (int i = 0; i < n; i++) {
        cdf += probabilities[i];
        if (coin < cdf) {
            return i;
        }
    }
    return n - 1; // in case of rounding errors
}

int compare(const void* a, const void* b) {
    UllmLlama2ProbIndex* a_ = (UllmLlama2ProbIndex*) a;
    UllmLlama2ProbIndex* b_ = (UllmLlama2ProbIndex*) b;
    if (a_->prob > b_->prob) return -1;
    if (a_->prob < b_->prob) return 1;
    return 0;
}

int sample_topp(float* probabilities, int n, float topp, UllmLlama2ProbIndex* probindex, float coin) {
    // top-p sampling (or "nucleus sampling") samples from the smallest set of
    // tokens that exceed probability topp. This way we never sample tokens that
    // have very low probabilities and are less likely to go "off the rails".
    // coin is a random number in [0, 1), usually from random_f32()

    int n0 = 0;
    // quicksort indices in descending order of probabilities
    // values smaller than (1 - topp) / (n - 1) cannot be part of the result
    // so for efficiency we crop these out as candidates before sorting
    const float cutoff = (1.0f - topp) / (n - 1);
    for (int i = 0; i < n; i++) {
        if (probabilities[i] >= cutoff) {
            probindex[n0].index = i;
            probindex[n0].prob = probabilities[i];
            n0++;
        }
    }
    qsort(probindex, n0, sizeof(UllmLlama2ProbIndex), compare);

    // truncate the list where cumulative probability exceeds topp
    float cumulative_prob = 0.0f;
    int last_idx = n0 - 1; // in case of rounding errors consider all elements
    for (int i = 0; i < n0; i++) {
        cumulative_prob += probindex[i].prob;
        if (cumulative_prob > topp) {
            last_idx = i;
            break; // we've exceeded topp by including last_idx
        }
    }

    // sample from the truncated list
    float r = coin * cumulative_prob;
    float cdf = 0.0f;
    for (int i = 0; i <= last_idx; i++) {
        cdf += probindex[i].prob;
        if (r < cdf) {
            return probindex[i].index;
        }
    }
    return probindex[last_idx].index; // in case of rounding errors
}

static UllmStatus UllmLlama2BuildSampler(const UllmLlama2RunConfig* config,
    UllmLlama2State* state) {
  UllmLlama2Sampler* sampler = &state->sampler;
  sampler->rng_state = config->rng_seed;
  sampler->probindex = UllmMemoryAlloc(state->transformer.config.vocab_size
      * sizeof(UllmLlama2ProbIndex));
  if (sampler->probindex == NULL) {
    return ULLM_STATUS_OOM;
  }

  return ULLM_STATUS_OK;
}

static void UllmLlama2FreeSampler(UllmLlama2Sampler* sampler) {
    UllmMemoryFree(sampler->probindex);
}

uint32_t random_u32(uint64_t *state) {
  // xorshift rng: https://en.wikipedia.org/wiki/Xorshift#xorshift.2A
  *state ^= *state >> 12;
  *state ^= *state << 25;
  *state ^= *state >> 27;
  return (*state * 0x2545F4914F6CDD1Dull) >> 32;
}

float random_f32(uint64_t *state) { // random float32 in [0,1)
  return (random_u32(state) >> 8) / 16777216.0f;
}

int sample(const UllmLlama2RunConfig* config, UllmLlama2State* state, float* logits) {
  // sample the token given the logits and some hyperparameters
  UllmLlama2Sampler* sampler = &state->sampler;
  const int32_t vocab_size = state->transformer.config.vocab_size;
  int next;
  if (config->temperature == 0.0f) {
    // greedy argmax sampling: take the token with the highest probability
    next = sample_argmax(logits, vocab_size);
  } else {
    // apply the temperature to the logits
    for (int q=0; q < vocab_size; q++) { logits[q] /= config->temperature; }
    // apply softmax to the logits to get the probabilities for next token
    softmax(logits, vocab_size);
    // flip a (float) coin (this is our source of entropy for sampling)
    float coin = random_f32(&sampler->rng_state);
    // we sample from this distribution to get the next token
    if (config->topp <= 0 || config->topp >= 1) {
      // simply sample from the predicted probability distribution
      next = sample_mult(logits, vocab_size, coin);
    } else {
      // top-p (nucleus) sampling, clamping the least likely tokens to zero
      next = sample_topp(logits, vocab_size, config->topp, sampler->probindex, coin);
    }
  }
  return next;
}

// Validates that the supplied config is correct.
static UllmStatus UllmLlama2ValidateConfig(const UllmLlama2RunConfig* config) {
  if (config->prompt == NULL) {
    ULOGE("prompt must not be NULL");
    return ULLM_STATUS_INVALID_ARGUMENT;
  }

  if (config->temperature < 0.0) {
    ULOGE("temperature must not be negative");
    return ULLM_STATUS_INVALID_ARGUMENT;
  }

  if (config->topp < 0.0f || config->topp > 1.0f) {
    ULOGE("topp must be between 0.0f and 1.0f");
    return ULLM_STATUS_INVALID_ARGUMENT;
  }

  if (config->steps == 0) {
    ULOGE("steps must be greater than 0");
    return ULLM_STATUS_INVALID_ARGUMENT;
  }

  return ULLM_STATUS_OK;
}

void UllmLlama2RunConfigInit(UllmLlama2RunConfig* config) {
  config->prompt = NULL;
  config->checkpoint_path = NULL;
  config->tokenizer_path = NULL;
  config->temperature = 1.0f;
  config->topp = 0.9f;
  config->steps = 256;
  config->rng_seed = 0;
  config->output_callback = NULL;
  config->cookie = NULL;
}

UllmStatus UllmLlama2Init(const UllmLlama2RunConfig* config,
    UllmLlama2State* state) {
  ULLM_RETURN_IF_ERROR(UllmLlama2ValidateConfig(config));
  ULLM_RETURN_IF_ERROR(UllmLlama2BuildTransformer(config, state));
  ULLM_RETURN_IF_ERROR(UllmLlama2BuildTokenizer(config, state));
  ULLM_RETURN_IF_ERROR(UllmLlama2BuildSampler(config, state));
  return ULLM_STATUS_OK;
}

UllmStatus UllmLlama2Generate(const UllmLlama2RunConfig* config,
    UllmLlama2State* state) {
  // +3 for '\0', ?BOS, ?EOS
  int* prompt_tokens = (int*)UllmMemoryAlloc((strlen(config->prompt) + 3) * sizeof(int));
  // encode the (string) prompt into tokens sequence
  int num_prompt_tokens = 0;
  UllmLlama2Encode(config, state, 1, 0, prompt_tokens, &num_prompt_tokens);
  if (num_prompt_tokens == 0) {
      ULOGE("Prompt contains zero tokens");
      return ULLM_STATUS_INVALID_ARGUMENT;
  }

  // start the main loop
  int next;        // will store the next token in the sequence
  int token = prompt_tokens[0]; // kick off with the first token in the prompt
  unsigned int pos = 0;     // position in the sequence
  while (pos < config->steps) {
      // forward the transformer to get logits for the next token
      float* logits = forward(&state->transformer, token, pos);

      // advance the state machine
      if (pos < num_prompt_tokens - 1) {
          // if we are still processing the input prompt, force the next prompt token
          next = prompt_tokens[pos + 1];
      } else {
          // otherwise sample the next token from the logits
          next = sample(config, state, logits);
      }
      pos++;

      // data-dependent terminating condition: the BOS (=1) token delimits sequences
      if (next == 1) { break; }

      UllmLlama2Decode(config, &state->tokenizer, token, next);
      token = next;
  }

  UllmLlama2EmitPiece(config, "\n");
  UllmMemoryFree(prompt_tokens);
  return ULLM_STATUS_OK;
}

void UllmLlama2Deinit(UllmLlama2State* state) {
  UllmLlama2FreeSampler(&state->sampler);
  UllmLlama2FreeTokenizer(state);
  UllmLlama2FreeTransformer(&state->transformer);
}
