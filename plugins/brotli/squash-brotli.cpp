/* Copyright (c) 2015 The Squash Authors
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *   Evan Nemerson <evan@nemerson.com>
 */

#include <assert.h>

#include <squash/squash.h>

#include "brotli/enc/encode.h"
#include "brotli/dec/decode.h"

typedef struct SquashBrotliOptions_s {
  SquashOptions base_object;

  brotli::BrotliParams::Mode mode;
  bool enable_transforms;
} SquashBrotliOptions;

#define SQUASH_BROTLI_MAX_COMPRESSED_BLOCK_SIZE(inlen) (inlen + 4)

#define SQUASH_BROTLI_INPUT_BUFFER_SIZE ((1 << 21) - 4)
#define SQUASH_BROTLI_OUTPUT_BUFFER_SIZE (1 << 21)

typedef struct SquashBrotliStream_s {
  SquashStream base_object;

  uint8_t* input_buffer;
  size_t input_buffer_pos;
  size_t input_buffer_length;

  uint8_t* output_buffer;
  size_t output_buffer_pos;
  size_t output_buffer_length;

  union {
    brotli::BrotliCompressor* comp;
    BrotliState decomp;
  } ctx;
} SquashBrotliStream;

extern "C" SquashStatus     squash_plugin_init_codec      (SquashCodec* codec, SquashCodecFuncs* funcs);

static void                 squash_brotli_options_init    (SquashBrotliOptions* options,
                                                           SquashCodec* codec,
                                                           SquashDestroyNotify destroy_notify);
static SquashBrotliOptions* squash_brotli_options_new     (SquashCodec* codec);
static void                 squash_brotli_options_destroy (void* options);
static void                 squash_brotli_options_free    (void* options);

static void                 squash_brotli_stream_init     (SquashBrotliStream* stream,
                                                           SquashCodec* codec,
                                                           SquashStreamType stream_type,
                                                           SquashBrotliOptions* options,
                                                           SquashDestroyNotify destroy_notify);
static SquashBrotliStream*  squash_brotli_stream_new      (SquashCodec* codec,
                                                           SquashStreamType stream_type,
                                                           SquashBrotliOptions* options);
static void                 squash_brotli_stream_destroy  (void* stream);
static void                 squash_brotli_stream_free     (void* stream);

static void
squash_brotli_options_init (SquashBrotliOptions* options, SquashCodec* codec, SquashDestroyNotify destroy_notify) {
  assert (options != NULL);

  squash_options_init ((SquashOptions*) options, codec, destroy_notify);

  options->mode = brotli::BrotliParams::MODE_TEXT;
  options->enable_transforms = false;
}

static SquashBrotliOptions*
squash_brotli_options_new (SquashCodec* codec) {
  SquashBrotliOptions* options;

  options = (SquashBrotliOptions*) malloc (sizeof (SquashBrotliOptions));
  squash_brotli_options_init (options, codec, squash_brotli_options_free);

  return options;
}

static void
squash_brotli_options_destroy (void* options) {
  squash_options_destroy ((SquashOptions*) options);
}

static void
squash_brotli_options_free (void* options) {
  squash_brotli_options_destroy ((SquashBrotliOptions*) options);
  free (options);
}

static SquashOptions*
squash_brotli_create_options (SquashCodec* codec) {
  return (SquashOptions*) squash_brotli_options_new (codec);
}

static SquashStatus
squash_brotli_parse_option (SquashOptions* options, const char* key, const char* value) {
  SquashBrotliOptions* opts = (SquashBrotliOptions*) options;
  char* endptr = NULL;

  assert (opts != NULL);

  if (strcasecmp (key, "mode") == 0) {
    if (strcasecmp (key, "text") == 0) {
      opts->mode = brotli::BrotliParams::MODE_TEXT;
    } else if (strcasecmp (key, "font")) {
      opts->mode = brotli::BrotliParams::MODE_FONT;
    } else {
      return SQUASH_BAD_VALUE;
    }
  } else if (strcasecmp (key, "enable-transforms") == 0) {
    if (strcasecmp (value, "true") == 0) {
      opts->enable_transforms = true;
    } else if (strcasecmp (value, "false")) {
      opts->enable_transforms = false;
    } else {
      return SQUASH_BAD_VALUE;
    }
  } else {
    return SQUASH_BAD_PARAM;
  }

  return SQUASH_OK;
}

static SquashBrotliStream*
squash_brotli_stream_new (SquashCodec* codec, SquashStreamType stream_type, SquashBrotliOptions* options) {
  SquashBrotliStream* stream;

  assert (codec != NULL);
  assert (stream_type == SQUASH_STREAM_COMPRESS || stream_type == SQUASH_STREAM_DECOMPRESS);

  stream = (SquashBrotliStream*) malloc (sizeof (SquashBrotliStream));
  squash_brotli_stream_init (stream, codec, stream_type, options, squash_brotli_stream_free);

  return stream;
}

static void
squash_brotli_stream_init (SquashBrotliStream* s,
                           SquashCodec* codec,
                           SquashStreamType stream_type,
                           SquashBrotliOptions* options,
                           SquashDestroyNotify destroy_notify) {
  SquashStream* stream = (SquashStream*) s;

  squash_stream_init (stream, codec, stream_type, (SquashOptions*) options, destroy_notify);

  if (stream->stream_type == SQUASH_STREAM_COMPRESS) {
    brotli::BrotliParams params;

    if (options != NULL) {
      SquashBrotliOptions* opts = (SquashBrotliOptions*) options;
      params.mode = opts->mode;
      params.enable_transforms = opts->enable_transforms;
    }

    s->ctx.comp = new brotli::BrotliCompressor (params);
    s->ctx.comp->WriteStreamHeader ();
  } else {
    BrotliStateInit (&(s->ctx.decomp));
  }

  s->input_buffer = NULL;
  s->input_buffer_length = 0;
  s->input_buffer_pos = 0;

  s->output_buffer = NULL;
  s->output_buffer_length = 0;
  s->output_buffer_pos = 0;
}

static void
squash_brotli_stream_destroy (void* stream) {
  SquashBrotliStream* s = (SquashBrotliStream*) stream;

  if (s->base_object.stream_type == SQUASH_STREAM_COMPRESS) {
    delete s->ctx.comp;
  } else {
    BrotliStateCleanup (&(s->ctx.decomp));
    assert (0);
  }

  squash_stream_destroy (stream);
}

static void
squash_brotli_stream_free (void* stream) {
  squash_brotli_stream_destroy (stream);
  free (stream);
}

static SquashStream*
squash_brotli_create_stream (SquashCodec* codec, SquashStreamType stream_type, SquashOptions* options) {
  return (SquashStream*) squash_brotli_stream_new (codec, stream_type, (SquashBrotliOptions*) options);
}

static SquashStatus
squash_brotli_compress_stream (SquashStream* stream, SquashOperation operation) {
  SquashBrotliStream* s = (SquashBrotliStream*) stream;
  SquashStatus res = SQUASH_FAILED;
  bool progress = false;

  if (s->output_buffer_length != 0) {
    const size_t remaining = s->output_buffer_length - s->output_buffer_pos;
    const size_t cp_size = (stream->avail_out < remaining) ? stream->avail_out : remaining;

    if (cp_size != 0) {
      memcpy (stream->next_out, s->output_buffer + s->output_buffer_pos, cp_size);
      stream->next_out += cp_size;
      stream->avail_out -= cp_size;

      s->output_buffer_pos += cp_size;
      if (s->output_buffer_pos == s->output_buffer_length) {
        s->output_buffer_pos = 0;
        s->output_buffer_length = 0;
        if (stream->avail_in == 0)
          return SQUASH_OK;
      } else {
        return SQUASH_PROCESSING;
      }

      progress = true;
    } else {
      return SQUASH_BUFFER_FULL;
    }
  }

  fprintf (stderr, "%d: Operation: %zu\n", __LINE__, operation);

  while ((stream->avail_in != 0 && stream->avail_out != 0) ||
         (operation != SQUASH_PROCESSING && !progress)) {
    if (s->input_buffer_length != 0 || (!progress && (stream->avail_in < SQUASH_BROTLI_INPUT_BUFFER_SIZE))) {
      if (s->input_buffer == NULL) {
        s->input_buffer = (uint8_t*) malloc (SQUASH_BROTLI_INPUT_BUFFER_SIZE);
        if (s->input_buffer == NULL)
          return progress ? SQUASH_PROCESSING : SQUASH_MEMORY;
      }

      const size_t buffer_remaining = SQUASH_BROTLI_INPUT_BUFFER_SIZE - s->input_buffer_length;
      const size_t cp_size = (stream->avail_in < buffer_remaining) ? stream->avail_in : buffer_remaining;

      memcpy (s->input_buffer + s->input_buffer_length, stream->next_in, cp_size);
      stream->next_in += cp_size;
      stream->avail_in -= cp_size;

      s->input_buffer_length += cp_size;
    }

      fprintf (stderr, "%d %zu, %zu\n", __LINE__, stream->avail_in, s->input_buffer_length);
    if (stream->avail_in >= SQUASH_BROTLI_INPUT_BUFFER_SIZE ||
        s->input_buffer_length == SQUASH_BROTLI_INPUT_BUFFER_SIZE ||
        operation != SQUASH_OPERATION_FLUSH) {
      const uint8_t* input_buffer;
      size_t input_size;
      uint8_t* output_buffer;
      size_t output_size;

      if (s->input_buffer_length != 0) {
        input_buffer = s->input_buffer;
        input_size = s->input_buffer_length;
      } else {
        input_buffer = stream->next_in;
        input_size = stream->avail_in;
      }

      if (input_size != 0 || operation == SQUASH_OPERATION_FINISH) {
        if (SQUASH_BROTLI_MAX_COMPRESSED_BLOCK_SIZE(input_size) < stream->avail_out) {
          if (s->output_buffer == NULL)
            s->output_buffer = (uint8_t*) malloc (SQUASH_BROTLI_OUTPUT_BUFFER_SIZE);
          output_buffer = s->output_buffer;
          output_size = SQUASH_BROTLI_OUTPUT_BUFFER_SIZE;
        } else {
          output_buffer = stream->next_out;
          output_size = stream->avail_out;
        }

        if (!s->ctx.comp->WriteMetaBlock (input_size, input_buffer, input_size == 0, &output_size, output_buffer)) {
          return SQUASH_FAILED;
        }

        progress = true;

        if (input_buffer == s->input_buffer) {
          s->input_buffer_length = 0;
          s->input_buffer_pos = 0;
        } else {
          stream->next_in += input_size;
          stream->avail_in -= input_size;
        }

        if (output_buffer == s->output_buffer) {
          s->output_buffer_length = output_size;
          s->output_buffer_pos = 0;
        } else {
          stream->next_out += output_size;
          stream->avail_out -= output_size;
        }
      }
    }
  }

  if (s->output_buffer_length != 0) {
    const size_t remaining = s->output_buffer_length - s->output_buffer_pos;
    const size_t cp_size = (stream->avail_out < remaining) ? stream->avail_out : remaining;

    if (cp_size != 0) {
      memcpy (stream->next_out, s->output_buffer + s->output_buffer_pos, cp_size);
      stream->next_out += cp_size;
      stream->avail_out -= cp_size;

      s->output_buffer_pos += cp_size;
      if (s->output_buffer_pos == s->output_buffer_length) {
        s->output_buffer_pos = 0;
        s->output_buffer_length = 0;
        if (stream->avail_in == 0)
          return SQUASH_OK;
      } else {
        return SQUASH_PROCESSING;
      }

      progress = true;
    } else {
      return SQUASH_BUFFER_FULL;
    }
  }

  if (s->output_buffer_length != 0)
    return SQUASH_PROCESSING;
  if (stream->avail_in == 0)
    return SQUASH_OK;
  else if (progress)
    return SQUASH_PROCESSING;
  else
    return SQUASH_FAILED;
}

static SquashStatus
squash_brotli_decompress_stream (SquashStream* stream, SquashOperation operation) {
  SquashBrotliStream* s = (SquashBrotliStream*) stream;
  SquashStatus res = SQUASH_FAILED;

  assert (false);

  return res;
}

static SquashStatus
squash_brotli_process_stream (SquashStream* stream, SquashOperation operation) {
  if (stream->stream_type == SQUASH_STREAM_COMPRESS)
    return squash_brotli_compress_stream (stream, operation);
  else
    return squash_brotli_decompress_stream (stream, operation);
}

static size_t
squash_brotli_get_max_compressed_size (SquashCodec* codec, size_t uncompressed_length) {
  return uncompressed_length + 4;
}

static SquashStatus
squash_brotli_decompress_buffer (SquashCodec* codec,
                                 uint8_t* decompressed, size_t* decompressed_length,
                                 const uint8_t* compressed, size_t compressed_length,
                                 SquashOptions* options) {
  int res = BrotliDecompressBuffer (compressed_length, compressed,
                                    decompressed_length, decompressed);

  return (res == 1) ? SQUASH_OK : SQUASH_FAILED;
}

static SquashStatus
squash_brotli_compress_buffer (SquashCodec* codec,
                            uint8_t* compressed, size_t* compressed_length,
                            const uint8_t* uncompressed, size_t uncompressed_length,
                            SquashOptions* options) {
  brotli::BrotliParams params;

  if (options != NULL) {
    SquashBrotliOptions* opts = (SquashBrotliOptions*) options;
    params.mode = opts->mode;
    params.enable_transforms = opts->enable_transforms;
  }

  int res = brotli::BrotliCompressBuffer (params,
                                          uncompressed_length, uncompressed,
                                          compressed_length, compressed);

  return (res == 1) ? SQUASH_OK : SQUASH_FAILED;
}

extern "C" SquashStatus
squash_plugin_init_codec (SquashCodec* codec, SquashCodecFuncs* funcs) {
  const char* name = squash_codec_get_name (codec);

  if (strcmp ("brotli", name) == 0) {
    funcs->get_max_compressed_size = squash_brotli_get_max_compressed_size;
    funcs->create_options = squash_brotli_create_options;
    funcs->parse_option = squash_brotli_parse_option;
    funcs->create_stream = squash_brotli_create_stream;
    funcs->process_stream = squash_brotli_process_stream;
    funcs->decompress_buffer = squash_brotli_decompress_buffer;
    funcs->compress_buffer = squash_brotli_compress_buffer;
  } else {
    return SQUASH_UNABLE_TO_LOAD;
  }

  return SQUASH_OK;
}
