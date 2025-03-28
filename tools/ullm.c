/*
 * Copyright 2025 Andrew Rossignol andrew.rossignol@gmail.com
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <string.h>

#include "c-flags.h"
#include "ullm/llama2.h"
#include "util/log.h"
#include "util/macros.h"

#define ULLM_LOG_TAG "ullm"

// Args to forward to runners.
typedef struct {
  const char* prompt;
  const char* checkpoint_path;
  const char* tokenizer_path;
} Args;

void OutputHandler(const char* token, void* cookie) {
  printf("%s", token);
}

UllmStatus UllmRunLlama2(const Args* args) {
  UllmLlama2RunConfig run_config;
  UllmLlama2RunConfigInit(&run_config);
  run_config.checkpoint_path = args->checkpoint_path;
  run_config.tokenizer_path = args->tokenizer_path;
  run_config.prompt = args->prompt;
  run_config.output_callback = OutputHandler;

  UllmLlama2State state;
  UllmStatus status = ULLM_STATUS_OK;
  ULLM_GOTO_IF_ERROR(cleanup, status, UllmLlama2Init(&run_config, &state));
  ULLM_GOTO_IF_ERROR(cleanup, status, UllmLlama2Generate(&run_config, &state));

cleanup:
  UllmLlama2Deinit(&state);
  return status;
}

const struct {
  const char* name;
  UllmStatus (*run)(const Args* args);
} model_runners[] = {
  {
    .name = "llama2",
    .run = UllmRunLlama2,
  },
};

UllmStatus UllmRun(const char* model, const Args* args) {
  for (size_t i = 0; i < ARRAY_SIZE(model_runners); i++) {
    if (strcmp(model, model_runners[i].name) == 0) {
      return model_runners[i].run(args);
    }
  }

  ULOGE("Unsupported model: '%s'", model);
  return ULLM_STATUS_INVALID_ARGUMENT;
}

int main(int argc, char** argv) {
  if (argc > 0) {
    c_flags_set_application_name(argv[0]);
  }

  char** model = c_flag_string(
      "model", "m", "LLM model name", "llama2");
  char** prompt = c_flag_string(
      "prompt", "p", "LLM prompt", "");
  char** checkpoint_path = c_flag_string(
      "checkpoint", "c", "checkpoint path", "");
  char** tokenizer_path = c_flag_string(
      "tokenizer_path", "t", "tokenizer path", "");
  bool *help = c_flag_bool(
      "help", "h", "show usage", false);
  c_flags_parse(&argc, &argv, false);

  if (*help) {
    c_flags_usage();
    return 0;
  } else if (strlen(*prompt) == 0 || strlen(*model) == 0) {
    c_flags_usage();
  } else {
    Args args = {
      .prompt = *prompt,
      .checkpoint_path = strlen(*checkpoint_path) == 0
          ? "ullm/tinystories110M.bin" : *checkpoint_path,
      .tokenizer_path = strlen(*tokenizer_path) == 0
          ? "ullm/tokenizer.bin" : *tokenizer_path,
    };

    setbuf(stdout, NULL);
    UllmStatus status = UllmRun(*model, &args);
    if (status == ULLM_STATUS_OK) {
      return 0;
    } else {
      ULOGE("Failed to run inference: %s", UllmStatusToString(status));
    }
  }

  return -1;
}
